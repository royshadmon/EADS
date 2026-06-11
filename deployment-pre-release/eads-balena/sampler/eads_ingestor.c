/*
 * EADS Data Ingestor — Phase 1 + 2 + 3 integrated
 *
 * Phase 1:  per-window anomaly detection (RMS + SAD) + disk safeguard
 * Phase 2:  SD-backed segment buffer + pre-trigger context + adaptive drain
 * Phase 3:  heartbeat bursts + node_health table
 *
 * Flow on ANOMALY trigger:
 *   1. Reader detects anomaly, mints anomaly_id
 *   2. segwriter_open() creates segment file, allocates index slot
 *   3. Pre-trigger ring contents flushed to segment (is_anomaly=0)
 *   4. Reader writes every new sample to segment (is_anomaly=1)
 *   5. After 5s hysteresis: segwriter_close(), marks slot READY
 *
 * Drain thread (always running):
 *   - Polls ab_find_drainable() for slots in READY or DRAINING state
 *   - Reads batch from segment file
 *   - PUTs to AnyLog at adaptive rate (token bucket)
 *   - On 200 OK: ab_set_uploaded(), ab_fsync_index() — commit point
 *   - When fully uploaded: ab_delete_segment()
 *
 * Threads:
 *   reader_loop (main, CPU 2)
 *   drain_thread       (CPU 3)  — continuous, processes segment backlog
 *   heartbeat_thread   (CPU 3)  — 10s tick, node_health + burst
 *   disk_monitor_thread          — pauses writes at 85% disk
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/statvfs.h>
#include <curl/curl.h>

#include "anomaly_detector.h"
#include "anomaly_buffer.h"
#include "pretrigger_ring.h"

/* ── Configuration ─────────────────────────────────────────────────── */
#define DEFAULT_SOCKET      "/var/sampling/samples.sock"
#define DEFAULT_CONN        "127.0.0.1:32149"
#define DEFAULT_BUFFER_PATH "/var/eads-anomaly"
#define DEFAULT_DRAIN_BATCH 2000
#define DBMS_NAME           "eads"
#define VOLTAGE_TABLE       "voltage_calibrated"
#define HEALTH_TABLE        "node_health"

#define ADC_VOLTS_PER_COUNT  (3.3 / 4096.0)
#define ADC_MID_VOLTS        1.692297
#define ADC_SCALE            281.793554

#define ANOMALY_EXIT_QUIET_US  (1ULL * 1000000ULL)   /* 1 sec instead of 5 */
#define ANOMALY_MAX_DURATION_US (60ULL * 1000000ULL)  /* hard cap 60 sec */

/* Sanity bounds — reject impossibly large/small readings (likely sampler glitch) */
#define MAX_PLAUSIBLE_VOLTAGE  500.0f    /* 500V is unreachable at 120V wall */
#define MIN_PLAUSIBLE_VOLTAGE  -500.0f

#define BURST_SIZE         512
#define HEARTBEAT_SEC      10

#define DEFAULT_DISK_PATH        "/"
#define DISK_PAUSE_PCT           85.0
#define DISK_RESUME_PCT          75.0
#define DISK_CHECK_INTERVAL_SEC  30

#define LINE_BUF_SIZE   128
#define RECV_BUF_SIZE   65536
#define JSON_ROW_SIZE   220
#define RECONNECT_DELAY 3
#define ANOMALY_ID_LEN  17

/* Adaptive drain parameters
 * Measured reality on Pi 3 A+: PUT completes ~236ms, AnyLog CPU 15-20%.
 * 3000/sec is the empirically TESTED safe sustained rate — we cap there
 * rather than pushing higher, since 3000 is proven stable on real hardware.
 * The throttle can still drop below 3000 if AnyLog ever genuinely struggles. */
#define DRAIN_RATE_INITIAL    3000.0   /* samples/sec — tested limit */
#define DRAIN_RATE_MIN        2000.0   /* floor if AnyLog slows */
#define DRAIN_RATE_MAX        3000.0   /* hard cap at tested limit */
#define DRAIN_FAST_RTT_MS     400      /* below this, speed up toward cap */
#define DRAIN_SLOW_RTT_MS     1200     /* only back off if genuinely slow */
#define DRAIN_GROW_FACTOR     1.10
#define DRAIN_SHRINK_FACTOR   0.85
#define DRAIN_ERROR_FACTOR    0.60

/* Backlog cap — total bytes of un-uploaded segments allowed on disk.
 * Prevents the "140MB backlog poisons every restart" failure. When exceeded,
 * the OLDEST un-uploaded segments are purged (their data is lost, but the
 * system stays healthy rather than choking on an unbounded backlog).
 * 500 MB ≈ 11M samples ≈ 24 minutes of anomaly at 7680Hz. */
#define MAX_BACKLOG_BYTES   (500ULL * 1024 * 1024)

/* ── Long-term metrics logger ───────────────────────────────────────────
 * Appends one compact CSV line of system + app health every
 * METRICS_INTERVAL_SEC to a size-capped rotating log on the SD card. Built
 * for multi-week soak tests: negligible CPU (a few /proc reads + one write),
 * bounded disk (rotates at METRICS_MAX_BYTES, keeps one previous file), and
 * crash-survivable (plain append, lives on the persistent volume). CSV so it
 * drops straight into a spreadsheet / pandas for post-hoc analysis. */
#define METRICS_INTERVAL_SEC   30
#define METRICS_MAX_BYTES      (10ULL * 1024 * 1024)   /* rotate at 10MB */
#define METRICS_SUBDIR         "metrics"
#define METRICS_FILE           "metrics.csv"
#define METRICS_FILE_OLD       "metrics.1.csv"

/* ── Globals ───────────────────────────────────────────────────────── */
static volatile int running = 1;

static int batch_size = DEFAULT_DRAIN_BATCH;
static const char *socket_path  = DEFAULT_SOCKET;
static const char *anylog_conn  = DEFAULT_CONN;
static const char *buffer_path  = DEFAULT_BUFFER_PATH;
static char anylog_url[256];
static char node_id[64];

static anomaly_buffer_t  ab;
static pretrigger_ring_t pretrigger;
static pq_detector_t     detector;

/* State machine */
typedef enum { MODE_NORMAL = 0, MODE_ANOMALY = 1 } node_mode_t;
static node_mode_t  current_mode      = MODE_NORMAL;
static uint64_t     mode_entered_us   = 0;
static uint64_t     last_anomaly_us   = 0;
static char         current_anomaly_id[ANOMALY_ID_LEN] = "";
static pthread_mutex_t state_mutex    = PTHREAD_MUTEX_INITIALIZER;

/* Reader → segment writer handoff */
static segment_writer_t  active_writer;
static bool              writer_open = false;
static pthread_mutex_t   writer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Segment write queue — decouples reader from slow disk I/O
 * Reader pushes samples; segment_writer_thread drains to SD.
 * If queue fills (disk too slow), reader drops oldest samples
 * rather than blocking. */
#define SEGQ_SIZE  32768   /* power of 2, ~1.5 MB at 48 bytes per sample */
#define SEGQ_MASK  (SEGQ_SIZE - 1)
typedef struct {
    segment_sample_t sample;
    /* Control codes: 0=sample, 1=open new segment, 2=close current */
    uint8_t  ctrl;
    char     ctrl_anomaly_id[16];
    uint64_t ctrl_ts_us;
} segq_entry_t;
static segq_entry_t segq[SEGQ_SIZE];
static volatile size_t segq_head = 0;   /* writer (reader thread) */
static volatile size_t segq_tail = 0;   /* reader (segwriter thread) */
static pthread_mutex_t segq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  segq_cond  = PTHREAD_COND_INITIALIZER;
static volatile uint64_t segq_dropped = 0;

/* Burst rotating buffer (Phase 3) */
static segment_sample_t  burst_buf[BURST_SIZE];
static volatile size_t   burst_write_idx = 0;
static volatile uint64_t burst_total_written = 0;
static pthread_mutex_t   burst_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Disk safeguard */
static const char *disk_path        = DEFAULT_DISK_PATH;
static double      disk_pause_pct   = DISK_PAUSE_PCT;
static double      disk_resume_pct  = DISK_RESUME_PCT;
static volatile bool   disk_paused      = false;
static volatile double last_disk_pct    = 0.0;
static volatile uint64_t samples_dropped_disk = 0;

/* curl handle shared between drain + heartbeat */
static CURL *curl_handle = NULL;
static pthread_mutex_t curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Stats */
static volatile uint64_t total_anomaly_windows = 0;
static volatile uint64_t total_normal_windows  = 0;
static volatile uint64_t total_transitions     = 0;
static volatile uint64_t total_heartbeats      = 0;
static volatile uint64_t total_bursts_sent     = 0;
static volatile uint64_t total_anomaly_rows_sent = 0;
static volatile double   drain_rate_current   = DRAIN_RATE_INITIAL;
static volatile uint64_t total_segments_done  = 0;
static time_t start_time = 0;

/* ── Helpers ───────────────────────────────────────────────────────── */
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* Read CLOCK_MONOTONIC in microseconds. This is derived from the SAME BCM2835
 * free-running system timer that sampling_bin uses for its per-sample
 * timestamps (microseconds since boot). */
static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* Per-sample timestamps from sampling_bin are microseconds-since-boot (BCM2835
 * system timer). To convert to real wall-clock microseconds we add an offset:
 *     offset = wall_clock_now - monotonic_now
 *     real_ts = sample_counter_us + offset
 * Both clocks tick at 1 MHz from the same hardware, so a constant offset gives
 * true microsecond wall-clock time per sample. The offset is refreshed
 * periodically so it tracks chrony's GPS discipline of the system clock.
 * Guarded by a mutex because reader + heartbeat both read it. */
static volatile int64_t  ts_offset_us = 0;
static volatile bool     ts_offset_valid = false;
static pthread_mutex_t   ts_offset_mutex = PTHREAD_MUTEX_INITIALIZER;

static void refresh_ts_offset(void) {
    /* Sample both clocks as close together as possible. */
    uint64_t mono = monotonic_us();
    uint64_t wall = now_us();
    pthread_mutex_lock(&ts_offset_mutex);
    ts_offset_us = (int64_t)wall - (int64_t)mono;
    ts_offset_valid = true;
    pthread_mutex_unlock(&ts_offset_mutex);
}

/* Convert a sampling_bin counter timestamp (μs since boot) to wall-clock μs.
 * If the offset isn't established yet, returns the raw counter (best effort). */
static uint64_t sample_ts_to_wallclock(uint64_t counter_us) {
    pthread_mutex_lock(&ts_offset_mutex);
    int64_t off = ts_offset_us;
    bool valid = ts_offset_valid;
    pthread_mutex_unlock(&ts_offset_mutex);
    if (!valid) return counter_us;
    return (uint64_t)((int64_t)counter_us + off);
}
static void handle_signal(int sig) { (void)sig; running = 0; }
static void pin_to_cpu(int cpu) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

/* Phase 4: set real-time priority on the calling thread.
 * SCHED_FIFO means this thread preempts all normal threads — guarantees the
 * reader gets CPU even when AnyLog/drain are saturating the system, which is
 * what prevents the sampling_bin DMA-buffer assertion under load.
 * Requires CAP_SYS_NICE (granted by privileged: true in docker-compose). */
static int set_realtime_priority(int prio) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;
    int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (r != 0) {
        fprintf(stderr, "[RT] pthread_setschedparam(prio=%d) failed: %s "
                        "(continuing at normal priority)\n",
                prio, strerror(r));
        return 0;
    }
    fprintf(stderr, "[RT] Thread now SCHED_FIFO priority %d\n", prio);
    return 1;
}

static inline char *fast_itoa(int v, char *p) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[12]; int n = 0;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
    while (n--) *p++ = tmp[n];
    return p;
}
static inline char *fast_u64toa(uint64_t v, char *p) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n--) *p++ = tmp[n];
    return p;
}
static inline char *fast_ftoa6(float v, char *p) {
    if (v < 0) { *p++ = '-'; v = -v; }
    long long scaled = (long long)(v * 1000000.0f + 0.5f);
    long long whole  = scaled / 1000000;
    long long frac   = scaled % 1000000;
    char tmp[16]; int n = 0;
    if (whole == 0) tmp[n++] = '0';
    else while (whole) { tmp[n++] = '0' + (whole % 10); whole /= 10; }
    while (n--) *p++ = tmp[n];
    *p++ = '.';
    char fbuf[6];
    for (int i = 5; i >= 0; i--) { fbuf[i] = '0' + (frac % 10); frac /= 10; }
    for (int i = 0; i < 6; i++) *p++ = fbuf[i];
    return p;
}

static void mint_anomaly_id(char *out) {
    static const char hex[] = "0123456789abcdef";
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t mix = ((uint64_t)ts.tv_sec << 30) ^ (uint64_t)ts.tv_nsec
                   ^ ((uint64_t)rand() << 16) ^ (uint64_t)rand();
    for (int i = 15; i >= 0; i--) { out[i] = hex[mix & 0xF]; mix >>= 4; }
    out[16] = '\0';
}

/* ── libcurl helpers ───────────────────────────────────────────────── */
static size_t curl_discard(void *p, size_t s, size_t n, void *u) {
    (void)p; (void)u; return s * n;
}
static int curl_init_persistent(void) {
    curl_handle = curl_easy_init();
    if (!curl_handle) return 0;
    curl_easy_setopt(curl_handle, CURLOPT_URL,            anylog_url);
    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST,  "PUT");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION,  curl_discard);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE,  1L);
    return 1;
}

/* Header lists are immutable per table, so build each ONCE and reuse for
 * every PUT. Saves 7 allocs + 7 frees per push — meaningful during anomaly
 * drain where PUTs fire continuously. Guarded by curl_mutex like the handle. */
static struct curl_slist *cached_hdrs_voltage = NULL;
static struct curl_slist *cached_hdrs_health  = NULL;
static struct curl_slist *build_table_hdrs(const char *table) {
    struct curl_slist *h = NULL;
    char table_hdr[64];
    snprintf(table_hdr, sizeof(table_hdr), "table: %s", table);
    h = curl_slist_append(h, "Content-Type: text/plain");
    h = curl_slist_append(h, "type: json");
    h = curl_slist_append(h, "dbms: " DBMS_NAME);
    h = curl_slist_append(h, table_hdr);
    h = curl_slist_append(h, "mode: streaming");
    h = curl_slist_append(h, "Connection: keep-alive");
    h = curl_slist_append(h, "Expect:");
    return h;
}

/* Returns 1 on HTTP 200, 0 otherwise. Writes elapsed RTT to *rtt_ms. */
static int rest_put_to_table(const char *table, const char *data, size_t len,
                             long *rtt_ms) {
    pthread_mutex_lock(&curl_mutex);
    struct curl_slist *hdrs;
    if (strcmp(table, VOLTAGE_TABLE) == 0) {
        if (!cached_hdrs_voltage) cached_hdrs_voltage = build_table_hdrs(table);
        hdrs = cached_hdrs_voltage;
    } else {
        if (!cached_hdrs_health) cached_hdrs_health = build_table_hdrs(table);
        hdrs = cached_hdrs_health;
    }
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS,    data);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, (long)len);
    uint64_t t0 = now_us();
    CURLcode res = curl_easy_perform(curl_handle);
    long http = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http);
    uint64_t t1 = now_us();
    if (rtt_ms) *rtt_ms = (long)((t1 - t0) / 1000ULL);
    pthread_mutex_unlock(&curl_mutex);
    if (res != CURLE_OK) {
        fprintf(stderr, "[PUT %s] %s\n", table, curl_easy_strerror(res));
        return 0;
    }
    if (http != 200) {
        fprintf(stderr, "[PUT %s] HTTP %ld\n", table, http);
        return 0;
    }
    return 1;
}

/* ── JSON builders ─────────────────────────────────────────────────── */
/* Build JSON array from segment_sample_t (used by both drain and burst) */
static size_t build_voltage_json(char *out, size_t cap,
                                 const segment_sample_t *samples, size_t n) {
    (void)cap;
    static const char ROW_TS[]    = "{\"timestamp_us\":\"";
    static const char ROW_RAW[]   = "\",\"raw_count\":\"";
    static const char ROW_INP[]   = "\",\"adc_input_volts\":\"";
    static const char ROW_CEN[]   = "\",\"adc_centered_volts\":\"";
    static const char ROW_EST[]   = "\",\"voltage_est\":\"";
    static const char ROW_ANOM[]  = "\",\"is_anomaly\":\"";
    static const char ROW_ID[]    = "\",\"anomaly_id\":\"";
    static const char ROW_END[]   = "\"}";
    char *p = out;
    *p++ = '[';
    for (size_t i = 0; i < n; i++) {
        if (i > 0) *p++ = ',';
        memcpy(p, ROW_TS, sizeof(ROW_TS)-1); p += sizeof(ROW_TS)-1;
        p = fast_u64toa(samples[i].ts_us, p);
        memcpy(p, ROW_RAW, sizeof(ROW_RAW)-1); p += sizeof(ROW_RAW)-1;
        p = fast_itoa(samples[i].raw_count, p);
        memcpy(p, ROW_INP, sizeof(ROW_INP)-1); p += sizeof(ROW_INP)-1;
        p = fast_ftoa6(samples[i].adc_input, p);
        memcpy(p, ROW_CEN, sizeof(ROW_CEN)-1); p += sizeof(ROW_CEN)-1;
        p = fast_ftoa6(samples[i].adc_centered, p);
        memcpy(p, ROW_EST, sizeof(ROW_EST)-1); p += sizeof(ROW_EST)-1;
        p = fast_ftoa6(samples[i].voltage_est, p);
        memcpy(p, ROW_ANOM, sizeof(ROW_ANOM)-1); p += sizeof(ROW_ANOM)-1;
        *p++ = samples[i].is_anomaly ? '1' : '0';
        memcpy(p, ROW_ID, sizeof(ROW_ID)-1); p += sizeof(ROW_ID)-1;
        if (samples[i].is_anomaly || samples[i].anomaly_id[0] != 0) {
            memcpy(p, samples[i].anomaly_id, 16); p += 16;
        }
        memcpy(p, ROW_END, sizeof(ROW_END)-1); p += sizeof(ROW_END)-1;
    }
    *p++ = ']'; *p = '\0';
    return p - out;
}

static size_t build_health_json(char *out, size_t cap) {
    pthread_mutex_lock(&state_mutex);
    const char *mode_str = (current_mode == MODE_ANOMALY) ? "ANOMALY" : "NORMAL";
    char anom_id[ANOMALY_ID_LEN];
    memcpy(anom_id, current_anomaly_id, ANOMALY_ID_LEN);
    pthread_mutex_unlock(&state_mutex);

    /* Count pending segments + their bytes */
    int pending_segs = 0;
    uint64_t pending_bytes = 0;
    pthread_mutex_lock(&ab.mutex);
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t s = ab.index->entries[i].status;
        if (s == SEG_READY || s == SEG_DRAINING || s == SEG_ACTIVE) {
            pending_segs++;
            pending_bytes += ab.index->entries[i].total_bytes
                           - ab.index->entries[i].uploaded_bytes;
        }
    }
    pthread_mutex_unlock(&ab.mutex);
    uint64_t pending_rows = pending_bytes / sizeof(segment_sample_t);

    time_t elapsed = time(NULL) - start_time;
    double rate = (elapsed > 0)
                  ? ((double)burst_total_written / elapsed) : 0.0;

    /* GPS fix published by the gpsd container ("lat,lon,mode"). Stationary
     * box: re-reading every heartbeat is cheap and picks up a late fix
     * (cold start indoors can take a while). No fix → zeros, and the fleet
     * service ignores coordinates unless gps_fix >= 2. */
    double gps_lat = 0.0, gps_lon = 0.0;
    int gps_fix = 0;
    {
        FILE *gf = fopen("/shared-nebula/gps", "r");
        if (gf) {
            if (fscanf(gf, "%lf,%lf,%d", &gps_lat, &gps_lon, &gps_fix) != 3) {
                gps_lat = gps_lon = 0.0;
                gps_fix = 0;
            }
            fclose(gf);
        }
    }

    int n = snprintf(out, cap,
        "[{\"timestamp_us\":\"%llu\","
        "\"node_id\":\"%s\","
        "\"mode\":\"%s\","
        "\"current_anomaly_id\":\"%s\","
        "\"ring_buffer_pct\":\"%.2f\","
        "\"pending_upload_rows\":\"%llu\","
        "\"pending_segments\":\"%d\","
        "\"samples_sec_actual\":\"%.1f\","
        "\"drain_rate\":\"%.0f\","
        "\"uptime_sec\":\"%ld\","
        "\"disk_pct\":\"%.2f\","
        "\"disk_paused\":\"%d\","
        "\"lat\":\"%.6f\","
        "\"lon\":\"%.6f\","
        "\"gps_fix\":\"%d\"}]",
        (unsigned long long)now_us(),
        node_id, mode_str, anom_id,
        0.0,  /* ring_buffer_pct deprecated in Phase 2 */
        (unsigned long long)pending_rows,
        pending_segs,
        rate,
        drain_rate_current,
        (long)elapsed,
        last_disk_pct,
        disk_paused ? 1 : 0,
        gps_lat, gps_lon, gps_fix);
    return (n > 0) ? (size_t)n : 0;
}

/* ── Segment write queue helpers ────────────────────────────────────── */

/* Add a control entry or sample without blocking. */
static void segq_push_sample(const segment_sample_t *s) {
    pthread_mutex_lock(&segq_mutex);
    size_t next = (segq_head + 1) & SEGQ_MASK;
    if (next == segq_tail) {
        /* Queue full — advance tail to drop oldest */
        segq_tail = (segq_tail + 1) & SEGQ_MASK;
        segq_dropped++;
    }
    segq[segq_head].sample = *s;
    segq[segq_head].ctrl   = 0;
    segq_head = next;
    pthread_cond_signal(&segq_cond);
    pthread_mutex_unlock(&segq_mutex);
}

static void segq_push_ctrl_open(const char *anomaly_id, uint64_t ts_us) {
    pthread_mutex_lock(&segq_mutex);
    /* For control entries, drop oldest sample (not other ctrl) if full */
    size_t next = (segq_head + 1) & SEGQ_MASK;
    while (next == segq_tail) {
        segq_tail = (segq_tail + 1) & SEGQ_MASK;
        segq_dropped++;
        next = (segq_head + 1) & SEGQ_MASK;
    }
    segq[segq_head].ctrl = 1;
    memcpy(segq[segq_head].ctrl_anomaly_id, anomaly_id, 16);
    segq[segq_head].ctrl_ts_us = ts_us;
    segq_head = next;
    pthread_cond_signal(&segq_cond);
    pthread_mutex_unlock(&segq_mutex);
}

static void segq_push_ctrl_close(uint64_t end_us) {
    pthread_mutex_lock(&segq_mutex);
    size_t next = (segq_head + 1) & SEGQ_MASK;
    while (next == segq_tail) {
        segq_tail = (segq_tail + 1) & SEGQ_MASK;
        segq_dropped++;
        next = (segq_head + 1) & SEGQ_MASK;
    }
    segq[segq_head].ctrl = 2;
    segq[segq_head].ctrl_ts_us = end_us;
    segq_head = next;
    pthread_cond_signal(&segq_cond);
    pthread_mutex_unlock(&segq_mutex);
}

/* ── SEGMENT WRITER THREAD — does all disk I/O for active segments ──── */
static void *segwriter_thread(void *arg) {
    (void)arg;
    pin_to_cpu(3);
    /* Phase 4: medium RT priority — disk persistence stays ahead of drain
     * and heartbeat, but below the reader. */
    set_realtime_priority(30);
    fprintf(stderr, "[SegWriter] Thread started\n");

    while (running) {
        pthread_mutex_lock(&segq_mutex);
        while (running && segq_head == segq_tail) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1;
            pthread_cond_timedwait(&segq_cond, &segq_mutex, &timeout);
        }
        if (!running) { pthread_mutex_unlock(&segq_mutex); break; }

        /* Drain a batch from queue (so we don't hold mutex during I/O) */
        segq_entry_t local[256];
        size_t got = 0;
        while (got < 256 && segq_tail != segq_head) {
            local[got++] = segq[segq_tail];
            segq_tail = (segq_tail + 1) & SEGQ_MASK;
        }
        pthread_mutex_unlock(&segq_mutex);

        /* Now process the batch (slow disk I/O is here, off the reader path) */
        for (size_t i = 0; i < got; i++) {
            if (local[i].ctrl == 1) {
                /* Open new segment */
                pthread_mutex_lock(&writer_mutex);
                if (writer_open) {
                    segwriter_close(&active_writer, &ab, local[i].ctrl_ts_us);
                    writer_open = false;
                }
                if (segwriter_open(&active_writer, &ab,
                                   local[i].ctrl_anomaly_id,
                                   local[i].ctrl_ts_us) == 0) {
                    writer_open = true;
                    /* Flush pre-trigger ring */
                    size_t cap = PRETRIGGER_CAPACITY;
                    segment_sample_t *snap = malloc(cap * sizeof(segment_sample_t));
                    if (snap) {
                        size_t n = pretrigger_snapshot(&pretrigger, snap, cap);
                        for (size_t j = 0; j < n; j++) {
                            snap[j].is_anomaly = 0;
                            memcpy(snap[j].anomaly_id,
                                   local[i].ctrl_anomaly_id, 16);
                        }
                        if (n > 0) {
                            segwriter_append_array(&active_writer, &ab, snap, n);
                            fprintf(stderr,
                                "[SegWriter] Flushed %zu pre-trigger samples "
                                "(%.2f sec)\n", n, (double)n / PRETRIGGER_RATE);
                        }
                        free(snap);
                    }
                }
                pthread_mutex_unlock(&writer_mutex);
            } else if (local[i].ctrl == 2) {
                /* Close current segment */
                pthread_mutex_lock(&writer_mutex);
                if (writer_open) {
                    segwriter_close(&active_writer, &ab, local[i].ctrl_ts_us);
                    writer_open = false;
                }
                pthread_mutex_unlock(&writer_mutex);
            } else {
                /* Sample — append to active segment if one is open */
                pthread_mutex_lock(&writer_mutex);
                if (writer_open) {
                    int r = segwriter_append(&active_writer, &ab,
                                             &local[i].sample);
                    if (r == -2) {
                        /* Segment cap hit, rotate */
                        fprintf(stderr,
                                "[SegWriter] Segment cap hit, rotating\n");
                        segwriter_close(&active_writer, &ab,
                                        local[i].sample.ts_us);
                        if (segwriter_open(&active_writer, &ab,
                                           local[i].sample.anomaly_id,
                                           local[i].sample.ts_us) == 0) {
                            writer_open = true;
                            segwriter_append(&active_writer, &ab,
                                             &local[i].sample);
                        } else {
                            writer_open = false;
                        }
                    }
                }
                pthread_mutex_unlock(&writer_mutex);
            }
        }
    }

    /* Drain remaining queue on shutdown */
    pthread_mutex_lock(&writer_mutex);
    if (writer_open) {
        segwriter_close(&active_writer, &ab, now_us());
        writer_open = false;
    }
    pthread_mutex_unlock(&writer_mutex);
    fprintf(stderr, "[SegWriter] Thread stopped\n");
    return NULL;
}


static void enter_anomaly_locked(uint64_t ts_us, uint32_t flags,
                                 float rms, float sad);

static void enter_anomaly_locked(uint64_t ts_us, uint32_t flags,
                                 float rms, float sad) {
    current_mode    = MODE_ANOMALY;
    mode_entered_us = ts_us;
    last_anomaly_us = ts_us;
    mint_anomaly_id(current_anomaly_id);
    total_transitions++;
    char fbuf[64];
    pq_flags_to_str(flags, fbuf, sizeof(fbuf));
    fprintf(stderr,
            "[State] NORMAL→ANOMALY  id=%s  ts_us=%llu  flags=%s  "
            "rms=%.2fV  sad=%.2f\n",
            current_anomaly_id, (unsigned long long)ts_us, fbuf, rms, sad);

    /* Queue control msg — disk I/O happens in segwriter_thread */
    segq_push_ctrl_open(current_anomaly_id, ts_us);
}

static void maybe_exit_anomaly(uint64_t now_ts_us) {
    pthread_mutex_lock(&state_mutex);
    if (current_mode == MODE_ANOMALY) {
        bool quiet_exit = (now_ts_us - last_anomaly_us >= ANOMALY_EXIT_QUIET_US);
        bool duration_exit = (now_ts_us - mode_entered_us >= ANOMALY_MAX_DURATION_US);
        if (quiet_exit || duration_exit) {
            uint64_t duration_us = now_ts_us - mode_entered_us;
            fprintf(stderr,
                    "[State] ANOMALY→NORMAL  id=%s  duration=%.2fs%s\n",
                    current_anomaly_id, duration_us / 1e6,
                    duration_exit ? "  (DURATION CAP)" : "");
            current_mode = MODE_NORMAL;
            current_anomaly_id[0] = '\0';
            total_transitions++;
            /* Queue close ctrl message — segwriter_thread does fclose+fsync */
            segq_push_ctrl_close(now_ts_us);
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

/* ── DRAIN THREAD — adaptive throttled SD-to-AnyLog uploader ────────── */
static void *drain_thread(void *arg) {
    (void)arg;
    pin_to_cpu(3);

    size_t json_cap = (size_t)batch_size * JSON_ROW_SIZE + 64;
    char  *json_buf = malloc(json_cap);
    segment_sample_t *batch_buf =
        malloc((size_t)batch_size * sizeof(segment_sample_t));
    if (!json_buf || !batch_buf) {
        fprintf(stderr, "[Drain] OOM\n");
        free(json_buf); free(batch_buf);
        return NULL;
    }

    fprintf(stderr, "[Drain] Started. Initial rate: %.0f samples/sec\n",
            drain_rate_current);

    while (running) {
        int slot = ab_find_drainable(&ab);
        if (slot < 0) {
            sleep(2);  /* nothing to do */
            continue;
        }

        ab_set_status(&ab, slot, SEG_DRAINING);
        ab_fsync_index(&ab);

        segment_reader_t reader;
        if (segreader_open(&reader, &ab, slot) != 0) {
            fprintf(stderr, "[Drain] Failed to open segment slot=%d\n", slot);
            ab_set_status(&ab, slot, SEG_READY);
            sleep(5);
            continue;
        }

        segment_entry_t entry = ab_get_entry(&ab, slot);
        fprintf(stderr, "[Drain] Starting slot=%d %s  bytes=%llu uploaded=%llu\n",
                slot, entry.filename,
                (unsigned long long)entry.total_bytes,
                (unsigned long long)entry.uploaded_bytes);

        while (running) {
            /* Adaptive throttle: cap how fast we send */
            uint64_t target_us = (uint64_t)
                ((double)batch_size / drain_rate_current * 1e6);

            ssize_t got = segreader_read(&reader, batch_buf, (size_t)batch_size);
            if (got <= 0) break;  /* segment fully read */

            size_t json_len = build_voltage_json(json_buf, json_cap,
                                                  batch_buf, (size_t)got);
            uint64_t t0 = now_us();
            long rtt_ms = 0;
            int ok = rest_put_to_table(VOLTAGE_TABLE, json_buf, json_len,
                                       &rtt_ms);
            uint64_t t1 = now_us();

            if (ok) {
                /* Commit point: update index and fsync */
                ab_set_uploaded(&ab, slot, reader.bytes_read);
                ab_fsync_index(&ab);
                total_anomaly_rows_sent += got;

                /* Adapt rate up/down based on RTT */
                if (rtt_ms < DRAIN_FAST_RTT_MS) {
                    drain_rate_current *= DRAIN_GROW_FACTOR;
                    if (drain_rate_current > DRAIN_RATE_MAX)
                        drain_rate_current = DRAIN_RATE_MAX;
                } else if (rtt_ms > DRAIN_SLOW_RTT_MS) {
                    drain_rate_current *= DRAIN_SHRINK_FACTOR;
                    if (drain_rate_current < DRAIN_RATE_MIN)
                        drain_rate_current = DRAIN_RATE_MIN;
                }
            } else {
                /* Error — back off and retry whole batch */
                segreader_close(&reader);
                drain_rate_current *= DRAIN_ERROR_FACTOR;
                if (drain_rate_current < DRAIN_RATE_MIN)
                    drain_rate_current = DRAIN_RATE_MIN;
                fprintf(stderr, "[Drain] PUT failed, rate=%.0f, retry in 5s\n",
                        drain_rate_current);
                ab_set_status(&ab, slot, SEG_READY);
                sleep(5);
                goto next_slot;
            }

            /* Sleep remainder of token bucket window */
            uint64_t elapsed = t1 - t0;
            if (target_us > elapsed) {
                usleep((useconds_t)(target_us - elapsed));
            }
        }

        segreader_close(&reader);

        /* Check if fully uploaded. A segment is "done" when all COMPLETE
         * samples have been uploaded. If total_bytes isn't a clean multiple
         * of the sample size (truncated/partial write from a killed
         * container), the trailing <48-byte remainder can never form a
         * sample — so we treat "remainder smaller than one sample" as done,
         * rather than retrying forever. */
        segment_entry_t e = ab_get_entry(&ab, slot);
        uint64_t remaining = (e.total_bytes > e.uploaded_bytes)
                           ? (e.total_bytes - e.uploaded_bytes) : 0;
        if (remaining < sizeof(segment_sample_t)) {
            /* Fully drained (any leftover is a sub-sample fragment) */
            if (remaining > 0) {
                fprintf(stderr,
                    "[Drain] Slot %d: discarding %llu-byte partial-sample "
                    "remainder (truncated segment)\n",
                    slot, (unsigned long long)remaining);
            }
            ab_delete_segment(&ab, slot);
            ab_fsync_index(&ab);
            total_segments_done++;
            fprintf(stderr, "[Drain] Slot %d uploaded + deleted (%llu rows)\n",
                    slot,
                    (unsigned long long)(e.uploaded_bytes / sizeof(segment_sample_t)));
        } else {
            /* Genuine partial (a full sample or more still pending, e.g. the
             * PUT failed mid-segment). Keep for retry, but SLEEP first so we
             * never spin in a tight loop. */
            fprintf(stderr,
                    "[Drain] Slot %d partial: %llu/%llu (%llu bytes left), "
                    "retry in 5s\n",
                    slot,
                    (unsigned long long)e.uploaded_bytes,
                    (unsigned long long)e.total_bytes,
                    (unsigned long long)remaining);
            ab_set_status(&ab, slot, SEG_READY);
            sleep(5);
        }

      next_slot: ;
    }

    free(json_buf);
    free(batch_buf);
    return NULL;
}

/* ── HEARTBEAT THREAD ──────────────────────────────────────────────── */
static void *heartbeat_thread(void *arg) {
    (void)arg;
    pin_to_cpu(3);

    size_t burst_json_cap = BURST_SIZE * JSON_ROW_SIZE + 64;
    char *burst_json = malloc(burst_json_cap);
    char  health_json[1024];
    segment_sample_t burst_snapshot[BURST_SIZE];
    if (!burst_json) return NULL;

    while (running) {
        for (int i = 0; i < HEARTBEAT_SEC && running; i++) sleep(1);
        if (!running) break;

        /* Refresh the counter→wallclock offset each cycle so per-sample
         * timestamps track chrony's GPS discipline of the system clock. */
        refresh_ts_offset();

        /* Health row */
        size_t hlen = build_health_json(health_json, sizeof(health_json));
        if (hlen > 0) {
            long rtt;
            if (rest_put_to_table(HEALTH_TABLE, health_json, hlen, &rtt))
                total_heartbeats++;
        }

        /* Burst in NORMAL mode */
        pthread_mutex_lock(&state_mutex);
        bool send_burst = (current_mode == MODE_NORMAL);
        pthread_mutex_unlock(&state_mutex);

        if (send_burst && !disk_paused && burst_total_written >= BURST_SIZE) {
            pthread_mutex_lock(&burst_mutex);
            size_t start = burst_write_idx % BURST_SIZE;
            for (size_t i = 0; i < BURST_SIZE; i++)
                burst_snapshot[i] = burst_buf[(start + i) % BURST_SIZE];
            pthread_mutex_unlock(&burst_mutex);

            size_t blen = build_voltage_json(burst_json, burst_json_cap,
                                              burst_snapshot, BURST_SIZE);
            long rtt;
            if (rest_put_to_table(VOLTAGE_TABLE, burst_json, blen, &rtt))
                total_bursts_sent++;
        }

        if (total_heartbeats % 6 == 1) {
            time_t elapsed = time(NULL) - start_time;
            pthread_mutex_lock(&state_mutex);
            const char *mode_str = current_mode == MODE_ANOMALY ? "ANOMALY" : "NORMAL";
            pthread_mutex_unlock(&state_mutex);
            double dpct = 0;
            ab_disk_pct(&ab, &dpct);
            fprintf(stderr,
                "[Status] uptime=%lds mode=%s heartbeats=%llu bursts=%llu "
                "anom_rows=%llu drain_rate=%.0f segs_done=%llu transitions=%llu "
                "disk=%.1f%%%s drop_disk=%llu segq_drop=%llu samples_seen=%llu\n",
                (long)elapsed, mode_str,
                (unsigned long long)total_heartbeats,
                (unsigned long long)total_bursts_sent,
                (unsigned long long)total_anomaly_rows_sent,
                drain_rate_current,
                (unsigned long long)total_segments_done,
                (unsigned long long)total_transitions,
                last_disk_pct,
                disk_paused ? "*PAUSED*" : "",
                (unsigned long long)samples_dropped_disk,
                (unsigned long long)segq_dropped,
                (unsigned long long)burst_total_written);
        }
    }
    free(burst_json);
    return NULL;
}

/* ── LONG-TERM METRICS LOGGER ───────────────────────────────────────────
 * One CSV line every METRICS_INTERVAL_SEC. Reads a few /proc files for
 * system health, reuses existing app counters, computes per-interval deltas,
 * and appends to a rotating size-capped log. Designed to be nearly free so
 * it can run for weeks during soak testing. */

/* Read total & available memory (kB) from /proc/meminfo. */
static void read_meminfo(long *mem_total_kb, long *mem_avail_kb) {
    *mem_total_kb = 0; *mem_avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (!*mem_total_kb && sscanf(line, "MemTotal: %ld kB", mem_total_kb) == 1) continue;
        if (!*mem_avail_kb && sscanf(line, "MemAvailable: %ld kB", mem_avail_kb) == 1) continue;
        if (*mem_total_kb && *mem_avail_kb) break;
    }
    fclose(f);
}

/* 1-minute load average from /proc/loadavg. */
static double read_loadavg1(void) {
    double l = 0.0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &l) != 1) l = 0.0; fclose(f); }
    return l;
}

/* CPU temperature in milli-°C from the thermal zone (Pi: zone0). */
static int read_cpu_temp_mc(void) {
    int t = 0;
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) { if (fscanf(f, "%d", &t) != 1) t = 0; fclose(f); }
    return t;
}

/* This process's RSS (kB) from /proc/self/statm (pages * page_size). */
static long read_self_rss_kb(void) {
    long rss_pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long total;
        if (fscanf(f, "%ld %ld", &total, &rss_pages) != 2) rss_pages = 0;
        fclose(f);
    }
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return rss_pages * page_kb;
}

/* ── Cross-service health probes (cheap; run once per metrics interval) ──
 * The sampler shares the nebula container's network namespace, so it can see
 * the overlay interface and reach the operator + master directly.
 * These checks log basic service health. */

/* Is the Nebula overlay interface up? Reads operstate (one tiny file read).
 * Returns 1 up, 0 down/absent. */
static int probe_nebula_up(void) {
    FILE *f = fopen("/sys/class/net/nebula1/operstate", "r");
    if (!f) return 0;
    char s[16] = {0};
    if (!fgets(s, sizeof(s), f)) { fclose(f); return 0; }
    fclose(f);
    /* tun devices report "unknown" when up (no carrier concept), or "up". */
    return (strncmp(s, "up", 2) == 0 || strncmp(s, "unknown", 7) == 0) ? 1 : 0;
}

/* Non-blocking TCP reachability check with a short timeout. Returns 1 if we
 * can establish a connection to host:port within timeout_ms, else 0. Used to
 * probe the local operator (127.0.0.1:32149) and the master (over overlay). */
static int probe_tcp(const char *host, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); return 0; }

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    int ok = 0;
    if (rc == 0) {
        ok = 1;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0; socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                ok = 1;
        }
    }
    close(fd);
    return ok;
}

/* Clock-sync health proxy: if the wall clock reads a sane recent year, some
 * time source (GPS or NTP fallback) has disciplined it. Returns 1 if the year
 * is >= 2025 (i.e. clock is NOT stuck at boot-epoch), else 0. */
static int probe_clock_synced(void) {
    time_t t = time(NULL);
    struct tm tmv; gmtime_r(&t, &tmv);
    return (tmv.tm_year + 1900) >= 2025 ? 1 : 0;
}

static void *metrics_thread(void *arg) {
    (void)arg;
    pin_to_cpu(3);

    /* Build the metrics directory path on the persistent buffer volume. */
    char dir[600], path[700], path_old[700];
    snprintf(dir, sizeof(dir), "%s/%s", buffer_path, METRICS_SUBDIR);
    mkdir(dir, 0755);
    snprintf(path,     sizeof(path),     "%s/%s", dir, METRICS_FILE);
    snprintf(path_old, sizeof(path_old), "%s/%s", dir, METRICS_FILE_OLD);

    /* Write a header if the file is new/empty. */
    struct stat st;
    bool need_header = (stat(path, &st) != 0 || st.st_size == 0);
    static const char *CSV_HEADER =
        "wall_iso,uptime_s,mode,samples_per_s,anom_rows_total,"
        "drain_rate,segs_done,transitions,segq_drop,drop_sanity,"
        "backlog_bytes,disk_pct,disk_paused,load1,cpu_temp_c,"
        "mem_used_pct,proc_rss_kb,ts_offset_valid,"
        "svc_nebula_up,svc_operator_up,svc_master_up,svc_clock_synced\n";
    if (need_header) {
        FILE *h = fopen(path, "a");
        if (h) { fputs(CSV_HEADER, h); fclose(h); }
    }

    uint64_t last_samples = burst_total_written;
    time_t   last_t = time(NULL);

    while (running) {
        for (int i = 0; i < METRICS_INTERVAL_SEC && running; i++) sleep(1);
        if (!running) break;

        /* ---- gather ---- */
        time_t now_t = time(NULL);
        double dt = difftime(now_t, last_t);
        if (dt < 1) dt = 1;
        uint64_t now_samples = burst_total_written;
        double samples_per_s = (double)(now_samples - last_samples) / dt;
        last_samples = now_samples;
        last_t = now_t;

        long mem_total, mem_avail;
        read_meminfo(&mem_total, &mem_avail);
        double mem_used_pct = (mem_total > 0)
            ? 100.0 * (mem_total - mem_avail) / mem_total : 0.0;
        double load1 = read_loadavg1();
        int temp_mc = read_cpu_temp_mc();
        long rss_kb = read_self_rss_kb();
        uint64_t backlog = ab_total_backlog_bytes(&ab);

        const char *mode_str =
            (current_mode == MODE_ANOMALY) ? "ANOMALY" : "NORMAL";

        /* Service health probes (cheap, once per interval). The local AnyLog
         * operator listens on 32149 in our shared netns; the master is at the
         * overlay address from LEDGER_CONN (parse host:port). */
        int svc_nebula   = probe_nebula_up();
        int svc_operator = probe_tcp("127.0.0.1", 32149, 300);
        int svc_clock    = probe_clock_synced();
        /* Master overlay reachability: parse host+port from LEDGER_CONN env
         * (e.g. "10.42.0.1:32048"); default to that if unset. */
        int svc_master = 0;
        {
            const char *lc = getenv("LEDGER_CONN");
            char host[64] = "10.42.0.1"; int mport = 32048;
            if (lc && *lc) {
                const char *colon = strrchr(lc, ':');
                if (colon && (size_t)(colon - lc) < sizeof(host)) {
                    memcpy(host, lc, colon - lc);
                    host[colon - lc] = '\0';
                    mport = atoi(colon + 1);
                    if (mport <= 0) mport = 32048;
                }
            }
            svc_master = probe_tcp(host, mport, 500);
        }

        /* Wall-clock ISO timestamp (uses the GPS-synced system clock). */
        char iso[32];
        time_t wt = now_t;
        struct tm tmv;
        gmtime_r(&wt, &tmv);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

        /* ---- rotate if oversized ---- */
        if (stat(path, &st) == 0 && (uint64_t)st.st_size >= METRICS_MAX_BYTES) {
            unlink(path_old);          /* drop the previous-old file */
            rename(path, path_old);    /* current becomes old */
            FILE *h = fopen(path, "a"); /* new current gets a fresh header */
            if (h) { fputs(CSV_HEADER, h); fclose(h); }
        }

        /* ---- append one line ---- */
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f,
                "%s,%ld,%s,%.0f,%llu,%.0f,%llu,%llu,%llu,%llu,"
                "%llu,%.1f,%d,%.2f,%d,%.1f,%ld,%d,"
                "%d,%d,%d,%d\n",
                iso,
                (long)(now_t - start_time),
                mode_str,
                samples_per_s,
                (unsigned long long)total_anomaly_rows_sent,
                drain_rate_current,
                (unsigned long long)total_segments_done,
                (unsigned long long)total_transitions,
                (unsigned long long)segq_dropped,
                (unsigned long long)samples_dropped_disk,
                (unsigned long long)backlog,
                last_disk_pct,
                disk_paused ? 1 : 0,
                load1,
                temp_mc / 1000,
                mem_used_pct,
                rss_kb,
                ts_offset_valid ? 1 : 0,
                svc_nebula,
                svc_operator,
                svc_master,
                svc_clock);
            fclose(f);   /* close each write = crash-safe, flushed to SD */
        }
    }
    return NULL;
}

/* ── DISK MONITOR + STALL WATCHDOG ──────────────────────────────────── */
static void *disk_monitor_thread(void *arg) {
    (void)arg;
    fprintf(stderr,
            "[DiskMon] Monitoring %s   pause>=%.1f%%  resume<=%.1f%%  every %ds\n",
            disk_path, disk_pause_pct, disk_resume_pct, DISK_CHECK_INTERVAL_SEC);

    /* Stall watchdog state: if the reader stops seeing new samples for
     * STALL_LIMIT seconds, the sampling pipe is dead (sampling_bin hung or
     * crashed in a way we didn't catch). Exit so start.sh restarts us clean. */
    uint64_t last_seen_count = burst_total_written;
    int stall_seconds = 0;
    const int STALL_LIMIT = 60;

    while (running) {
        struct statvfs st;
        if (statvfs(disk_path, &st) == 0) {
            uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
            uint64_t avail = (uint64_t)st.f_bavail * st.f_frsize;
            uint64_t used  = total - avail;
            double pct = total ? (100.0 * used / total) : 0.0;
            last_disk_pct = pct;
            bool was_paused = disk_paused;
            if (!was_paused && pct >= disk_pause_pct) {
                disk_paused = true;
                fprintf(stderr, "[DiskMon] *** PAUSE *** %.1f%% full\n", pct);
            } else if (was_paused && pct <= disk_resume_pct) {
                disk_paused = false;
                fprintf(stderr, "[DiskMon] *** RESUME *** %.1f%% full\n", pct);
            }
        }

        /* Backlog soft-cap check: warns when unsent anomaly data exceeds
         * the cap. Nothing is purged — un-acked anomaly segments are
         * protected (see ab_enforce_backlog_cap). */
        ab_enforce_backlog_cap(&ab, MAX_BACKLOG_BYTES);

        /* Stall detection */
        uint64_t now_count = burst_total_written;
        if (now_count == last_seen_count) {
            stall_seconds += DISK_CHECK_INTERVAL_SEC;
            if (stall_seconds >= STALL_LIMIT) {
                fprintf(stderr,
                        "[Watchdog] No samples for %d sec — sampling pipe is "
                        "dead. Exiting so start.sh restarts cleanly.\n",
                        stall_seconds);
                running = 0;
                /* Hard exit — don't wait for graceful join, the pipe is dead */
                _exit(42);
            }
        } else {
            stall_seconds = 0;
            last_seen_count = now_count;
        }

        for (int i = 0; i < DISK_CHECK_INTERVAL_SEC && running; i++) sleep(1);
    }
    return NULL;
}

/* ── READER LOOP ───────────────────────────────────────────────────── */
static void reader_loop(void) {
    /* Core 3: ALL ingestor threads share core 3. Core 2 is reserved
     * exclusively for sampling_bin (pinned + RT in start.sh), whose DMA
     * double-buffer loop is HARD real-time — if starved it asserts and
     * aborts. Our reader is SOFT real-time: the 32K segq + socket buffer
     * absorb scheduling delay, so we must NOT share sampling_bin's core. */
    pin_to_cpu(3);
    /* RT priority so the reader still preempts our own drain/heartbeat on
     * core 3 and keeps the socket drained — but it's isolated to core 3, so
     * it can no longer starve sampling_bin on core 2. */
    set_realtime_priority(50);
    char recv_buf[RECV_BUF_SIZE];
    char line_buf[LINE_BUF_SIZE];
    size_t partial = 0;

    while (running) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { sleep(RECONNECT_DELAY); continue; }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
        fprintf(stderr, "[Socket] Connecting to %s...\n", socket_path);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "[Socket] connect: %s\n", strerror(errno));
            close(fd); sleep(RECONNECT_DELAY); continue;
        }
        fprintf(stderr, "[Socket] Connected\n");
        partial = 0;

        while (running) {
            ssize_t n = recv(fd, recv_buf, sizeof(recv_buf) - 1, 0);
            if (n <= 0) {
                if (n < 0) perror("[Socket] recv");
                else fprintf(stderr, "[Socket] Connection closed\n");
                break;
            }
            for (ssize_t i = 0; i < n; i++) {
                char c = recv_buf[i];
                if (c == '\n') {
                    line_buf[partial] = '\0';
                    partial = 0;
                    char *p1 = strchr(line_buf, ',');
                    if (!p1) continue;
                    *p1 = '\0';
                    char *p2 = strchr(p1 + 1, ',');
                    if (!p2) continue;
                    *p2 = '\0';
                    int channel   = atoi(p1 + 1);
                    int raw_count = atoi(p2 + 1);
                    if (channel != 1) continue;
                    if (disk_paused) { samples_dropped_disk++; continue; }

                    uint64_t ts_us = strtoull(line_buf, NULL, 10);
                    /* ts_us here is the raw BCM2835 counter (μs since boot).
                     * Convert to real wall-clock μs using the GPS-synced
                     * offset. Detector keeps using the raw counter for window
                     * timing (relative deltas), but persisted/streamed data
                     * carries true wall-clock time. */
                    uint64_t wall_ts_us = sample_ts_to_wallclock(ts_us);
                    float adc_input    = (float)(raw_count * ADC_VOLTS_PER_COUNT);
                    float adc_centered = adc_input - (float)ADC_MID_VOLTS;
                    float voltage_est  = adc_centered * (float)ADC_SCALE;

                    /* Sanity check — sampling_bin can output garbage values
                     * during/after a crash. Reject implausibly large readings
                     * so they don't trigger fake anomalies. */
                    if (voltage_est > MAX_PLAUSIBLE_VOLTAGE ||
                        voltage_est < MIN_PLAUSIBLE_VOLTAGE) {
                        samples_dropped_disk++;  /* reuse counter for stats */
                        continue;
                    }

                    pq_result_t result;
                    if (pq_ingest(&detector, voltage_est, ts_us, &result)) {
                        if (result.anomaly_flags) {
                            total_anomaly_windows++;
                            pthread_mutex_lock(&state_mutex);
                            if (current_mode == MODE_NORMAL) {
                                enter_anomaly_locked(result.window_start_us,
                                                     result.anomaly_flags,
                                                     result.measured_rms_v,
                                                     result.waveform_sad);
                            } else {
                                last_anomaly_us = ts_us;
                            }
                            pthread_mutex_unlock(&state_mutex);
                        } else {
                            total_normal_windows++;
                        }
                        maybe_exit_anomaly(ts_us);
                    }

                    /* Build sample */
                    segment_sample_t s = {0};
                    s.ts_us        = wall_ts_us;   /* real wall-clock μs */
                    s.raw_count    = raw_count;
                    s.adc_input    = adc_input;
                    s.adc_centered = adc_centered;
                    s.voltage_est  = voltage_est;
                    pthread_mutex_lock(&state_mutex);
                    bool in_anomaly = (current_mode == MODE_ANOMALY);
                    if (in_anomaly) {
                        s.is_anomaly = 1;
                        memcpy(s.anomaly_id, current_anomaly_id, 16);
                    }
                    pthread_mutex_unlock(&state_mutex);

                    /* Always update pre-trigger ring */
                    pretrigger_push(&pretrigger, &s);

                    /* Always update rotating burst buffer */
                    pthread_mutex_lock(&burst_mutex);
                    burst_buf[burst_write_idx % BURST_SIZE] = s;
                    burst_write_idx++;
                    burst_total_written++;
                    pthread_mutex_unlock(&burst_mutex);

                    /* During ANOMALY: push to segment queue (NOT disk).
                     * segwriter_thread handles the slow disk I/O off our path. */
                    if (in_anomaly) {
                        segq_push_sample(&s);
                    }
                } else {
                    if (partial < LINE_BUF_SIZE - 1)
                        line_buf[partial++] = c;
                }
            }
        }

        close(fd);
        if (running) sleep(RECONNECT_DELAY);
    }
}

/* ── Entry ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    float sample_rate = PQ_DEFAULT_SAMPLE_RATE;
    pq_tolerances_t tol = { 5.0f, 10.0f, 50.0f };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i+1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--conn") && i+1 < argc) anylog_conn = argv[++i];
        else if (!strcmp(argv[i], "--buffer-path") && i+1 < argc) buffer_path = argv[++i];
        else if (!strcmp(argv[i], "--drain-batch") && i+1 < argc) batch_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample-rate") && i+1 < argc) sample_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rms-pct") && i+1 < argc) tol.rms_deviation_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--outage-v") && i+1 < argc) tol.outage_rms_v = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sad-limit") && i+1 < argc) tol.waveform_sad = atof(argv[++i]);
        else if (!strcmp(argv[i], "--disk-path") && i+1 < argc) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--disk-pause-pct") && i+1 < argc) disk_pause_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--disk-resume-pct") && i+1 < argc) disk_resume_pct = atof(argv[++i]);
    }

    snprintf(anylog_url, sizeof(anylog_url), "http://%s", anylog_conn);
    /* Node identity for health rows, in preference order:
     *   1. EADS_NODE_NAME env (set at enrollment)
     *   2. /shared-nebula/node_name (written by the nebula container after
     *      auto-enrollment; requires the ro volume mount in docker-compose)
     *   3. hostname (fallback) */
    const char *nn = getenv("EADS_NODE_NAME");
    if (nn && *nn) {
        snprintf(node_id, sizeof(node_id), "%s", nn);
    } else {
        FILE *nf = fopen("/shared-nebula/node_name", "r");
        if (nf) {
            if (fgets(node_id, sizeof(node_id), nf)) {
                char *nl = strchr(node_id, '\n');
                if (nl) *nl = '\0';
            }
            fclose(nf);
        }
        if (node_id[0] == '\0' && gethostname(node_id, sizeof(node_id)) != 0)
            snprintf(node_id, sizeof(node_id), "unknown");
    }

    signal(SIGINT, handle_signal); signal(SIGTERM, handle_signal);
    srand((unsigned)time(NULL));

    pq_init(&detector, sample_rate, &tol);
    pretrigger_init(&pretrigger);
    if (!ab_init(&ab, buffer_path)) {
        fprintf(stderr, "FATAL: ab_init failed\n");
        return 1;
    }
    ab_recover_after_restart(&ab);
    /* Report startup backlog. A large accumulation (e.g. after a long
     * outage) is preserved and drained, never trimmed — the adaptive
     * drain (2000-3000 rows/s) clears ~1 GB of segments in under an hour
     * once the master is reachable. */
    {
        uint64_t bl = ab_total_backlog_bytes(&ab);
        fprintf(stderr, "[AB] Startup backlog: %llu bytes (%.1f MB)\n",
                (unsigned long long)bl, bl / 1024.0 / 1024.0);
        ab_enforce_backlog_cap(&ab, MAX_BACKLOG_BYTES);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!curl_init_persistent()) return 1;

    fprintf(stderr, "\n[Ingestor] EADS C Ingestor (Phase 1+2+3+4 hardened)\n");
    fprintf(stderr, "  Node:          %s\n", node_id);
    fprintf(stderr, "  Socket:        %s\n", socket_path);
    fprintf(stderr, "  Target:        %s\n", anylog_url);
    fprintf(stderr, "  Buffer path:   %s\n", buffer_path);
    fprintf(stderr, "  Sample rate:   %.1f Hz\n", sample_rate);
    fprintf(stderr, "  Drain batch:   %d samples\n", batch_size);
    fprintf(stderr, "  Drain rate:    %.0f samples/sec (adaptive, %.0f-%.0f)\n",
            drain_rate_current, DRAIN_RATE_MIN, DRAIN_RATE_MAX);
    fprintf(stderr, "  Pre-trigger:   %d samples (%.1f sec)\n",
            PRETRIGGER_CAPACITY, (double)PRETRIGGER_SECONDS);
    fprintf(stderr, "  Heartbeat:     every %d sec  burst=%d samples\n",
            HEARTBEAT_SEC, BURST_SIZE);
    fprintf(stderr, "  Tolerances:    rms_pct=%.1f%% outage_v=%.1fV sad=%.1f\n",
            tol.rms_deviation_pct, tol.outage_rms_v, tol.waveform_sad);
    fprintf(stderr, "  Disk:          %s  pause>=%.1f%% resume<=%.1f%%\n",
            disk_path, disk_pause_pct, disk_resume_pct);
    fprintf(stderr, "  Metrics log:   %s/%s/%s  (every %ds, rotate %lluMB)\n\n",
            buffer_path, METRICS_SUBDIR, METRICS_FILE,
            METRICS_INTERVAL_SEC,
            (unsigned long long)(METRICS_MAX_BYTES / 1024 / 1024));

    start_time = time(NULL);

    /* Establish the counter→wallclock offset before sampling begins so the
     * very first samples carry real timestamps (assumes the system clock is
     * already GPS-synced by chrony; if not, offset auto-corrects once it is). */
    refresh_ts_offset();

    pthread_t drain_tid, hb_tid, disk_tid, segw_tid, metrics_tid;
    pthread_create(&segw_tid, NULL, segwriter_thread,    NULL);
    pthread_create(&drain_tid, NULL, drain_thread,       NULL);
    pthread_create(&hb_tid,   NULL, heartbeat_thread,    NULL);
    pthread_create(&disk_tid, NULL, disk_monitor_thread, NULL);
    pthread_create(&metrics_tid, NULL, metrics_thread,   NULL);

    reader_loop();

    running = 0;
    pthread_cond_broadcast(&segq_cond);
    pthread_join(segw_tid,  NULL);
    pthread_join(drain_tid, NULL);
    pthread_join(hb_tid,    NULL);
    pthread_join(disk_tid,  NULL);
    pthread_join(metrics_tid, NULL);

    /* segwriter_thread already closes active segment on shutdown */

    ab_close(&ab);
    if (cached_hdrs_voltage) curl_slist_free_all(cached_hdrs_voltage);
    if (cached_hdrs_health)  curl_slist_free_all(cached_hdrs_health);
    if (curl_handle) curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return 0;
}
