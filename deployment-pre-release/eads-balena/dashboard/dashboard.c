/*
 * EADS Local Dashboard — minimal single-binary HTTP server.
 *
 * Serves a live node-health page on port 8080 (host network) so anyone on
 * the Pi's LAN can check the node with a browser — no Balena dashboard, no
 * SSH, no cloud dependency.
 *
 * Design constraints (Pi 3 A+, 512MB RAM, cores 0-1 shared with AnyLog):
 *   - ONE thread, sequential accept loop. A local status page has one
 *     viewer; concurrency machinery would be pure overhead.
 *   - Zero heap allocation per request. Fixed stack/static buffers.
 *   - Read-only consumers: /sys, /proc, vcgencmd, and the LAST LINE of the
 *     ingestor's metrics.csv (read via the shared eads-anomaly volume,
 *     mounted ro). Never writes anything anywhere.
 *   - Socket timeouts everywhere so a wedged client can't hang the server.
 *
 * Endpoints:
 *   GET /            → embedded HTML page (auto-refreshing)
 *   GET /api/status  → JSON snapshot
 *   anything else    → 404
 *
 * Resource cost measured on amd64: <1MB RSS, ~0% CPU between requests.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PORT_DEFAULT     8080
#define METRICS_CSV      "/var/eads-anomaly/metrics/metrics.csv"
#define REQ_BUF          2048
#define RESP_BUF         16384
#define IO_TIMEOUT_SEC   5

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

/* ── tiny file/command readers (all bounded, all failure-tolerant) ────── */

static int read_file_str(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = '\0'; return 0; }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    /* trim trailing newline/space */
    while (n && (out[n-1] == '\n' || out[n-1] == ' ')) out[--n] = '\0';
    return 1;
}

static long read_file_long(const char *path, long dflt) {
    char b[64];
    if (!read_file_str(path, b, sizeof(b))) return dflt;
    char *end; long v = strtol(b, &end, 10);
    return (end == b) ? dflt : v;
}

/* Run a command with a short timeout, capture first line. Returns 1 on
 * success. Used only for vcgencmd, which is absent off-Pi → graceful 0. */
static int read_cmd_line(const char *cmd, char *out, size_t cap) {
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    int ok = (fgets(out, (int)cap, p) != NULL);
    pclose(p);
    if (!ok) return 0;
    size_t n = strlen(out);
    while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    return n > 0;
}

/* Last non-empty line of metrics.csv without reading the whole file:
 * seek near the end, scan back. File rotates at 10MB so the tail read is
 * always cheap. Returns 1 if a data line (not the header) was found. */
static int read_metrics_last_line(char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen(METRICS_CSV, "r");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    long back = size < 4096 ? size : 4096;
    if (fseek(f, size - back, SEEK_SET) != 0) { fclose(f); return 0; }
    static char chunk[4100];
    size_t n = fread(chunk, 1, (size_t)back, f);
    fclose(f);
    chunk[n] = '\0';
    /* find last non-empty line */
    char *last = NULL, *line = strtok(chunk, "\n");
    while (line) {
        if (*line && strncmp(line, "wall_iso", 8) != 0) last = line;
        line = strtok(NULL, "\n");
    }
    if (!last) return 0;
    snprintf(out, cap, "%s", last);
    return 1;
}

/* metrics.csv column order (must match eads_ingestor.c CSV_HEADER):
 *  0 wall_iso        1 uptime_s        2 mode            3 samples_per_s
 *  4 anom_rows_total 5 drain_rate      6 segs_done       7 transitions
 *  8 segq_drop       9 drop_sanity    10 backlog_bytes  11 disk_pct
 * 12 disk_paused    13 load1          14 cpu_temp_c     15 mem_used_pct
 * 16 proc_rss_kb    17 ts_offset_valid
 * 18 svc_nebula_up  19 svc_operator_up 20 svc_master_up 21 svc_clock_synced */
#define METRICS_NFIELDS 22
static int split_csv(char *line, char *fields[], int max) {
    int n = 0;
    char *p = line;
    while (n < max && p) {
        fields[n++] = p;
        p = strchr(p, ',');
        if (p) *p++ = '\0';
    }
    return n;
}

/* JSON-escape a short string into out (handles quotes/backslash/control). */
static void jesc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ── status JSON builder ──────────────────────────────────────────────── */

static size_t build_status_json(char *out, size_t cap) {
    /* live system reads */
    long temp_mc = read_file_long("/sys/class/thermal/thermal_zone0/temp", -1);
    long freq0   = read_file_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", -1);
    long freq2   = read_file_long("/sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq", -1);

    char loadavg[64] = "";
    read_file_str("/proc/loadavg", loadavg, sizeof(loadavg));
    char load1[16] = "0";
    sscanf(loadavg, "%15s", load1);

    long mem_total = -1, mem_avail = -1;
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                if (mem_total < 0) sscanf(line, "MemTotal: %ld kB", &mem_total);
                if (mem_avail < 0) sscanf(line, "MemAvailable: %ld kB", &mem_avail);
                if (mem_total > 0 && mem_avail > 0) break;
            }
            fclose(f);
        }
    }

    double uptime_s = 0;
    {
        char b[64];
        if (read_file_str("/proc/uptime", b, sizeof(b))) uptime_s = atof(b);
    }

    /* nebula overlay interface present/up? (host netns) */
    char nebstate[16] = "";
    int nebula_if_up = 0;
    if (read_file_str("/sys/class/net/nebula1/operstate", nebstate, sizeof(nebstate)))
        nebula_if_up = (strncmp(nebstate, "up", 2) == 0 ||
                        strncmp(nebstate, "unknown", 7) == 0);

    /* vcgencmd — undervoltage/throttle bits + core voltage. Absent on
     * non-Pi or without /dev/vchiq: fields become null. */
    char throttled[64] = "", volts[64] = "";
    long thr_bits = -1; double core_v = -1;
    if (read_cmd_line("vcgencmd get_throttled 2>/dev/null", throttled, sizeof(throttled))) {
        char *eq = strchr(throttled, '=');
        if (eq) thr_bits = strtol(eq + 1, NULL, 16);
    }
    if (read_cmd_line("vcgencmd measure_volts core 2>/dev/null", volts, sizeof(volts))) {
        char *eq = strchr(volts, '=');
        if (eq) core_v = atof(eq + 1);
    }

    /* ingestor metrics tail */
    static char mline[4100];
    char *mf[METRICS_NFIELDS] = {0};
    int have_metrics = 0, mn = 0;
    if (read_metrics_last_line(mline, sizeof(mline))) {
        mn = split_csv(mline, mf, METRICS_NFIELDS);
        have_metrics = (mn >= METRICS_NFIELDS);
    }

    char now_iso[40];
    time_t t = time(NULL);
    struct tm tmv; gmtime_r(&t, &tmv);
    strftime(now_iso, sizeof(now_iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    const char *node_name = getenv("EADS_NODE_NAME");
    char node_buf[80] = "";
    if (!node_name || !*node_name) {
        /* Auto-enrolled boxes have no env var; nebula writes the name here. */
        if (read_file_str("/shared-nebula/node_name", node_buf, sizeof(node_buf))
            && node_buf[0])
            node_name = node_buf;
    }
    const char *neb_ip = getenv("EADS_NEBULA_IP");
    char ip_buf[48] = "";
    if (!neb_ip || !*neb_ip) {
        if (read_file_str("/shared-nebula/ip", ip_buf, sizeof(ip_buf))
            && ip_buf[0])
            neb_ip = ip_buf;
    }
    char node_esc[80] = "", ip_esc[48] = "";
    jesc(node_name && *node_name ? node_name : "unknown", node_esc, sizeof(node_esc));
    jesc(neb_ip ? neb_ip : "", ip_esc, sizeof(ip_esc));

    /* Pre-format the maybe-null fields so the main snprintf stays simple. */
    char volts_j[24], thr_j[24];
    if (core_v >= 0) snprintf(volts_j, sizeof(volts_j), "%.4f", core_v);
    else             snprintf(volts_j, sizeof(volts_j), "null");
    if (thr_bits >= 0) snprintf(thr_j, sizeof(thr_j), "%ld", thr_bits);
    else               snprintf(thr_j, sizeof(thr_j), "null");

    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o,
        "{\"now\":\"%s\",\"node\":\"%s\",\"nebula_ip\":\"%s\","
        "\"host\":{\"uptime_s\":%.0f,\"load1\":%s,"
        "\"cpu_temp_c\":%.1f,\"freq_mhz_core0\":%ld,\"freq_mhz_core2\":%ld,"
        "\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld,"
        "\"core_volts\":%s,\"throttled_bits\":%s,"
        "\"undervolt_now\":%s,\"undervolt_ever\":%s,"
        "\"nebula_if_up\":%s},",
        now_iso, node_esc, ip_esc,
        uptime_s, load1[0] ? load1 : "0",
        temp_mc >= 0 ? temp_mc / 1000.0 : -1.0,
        freq0 >= 0 ? freq0 / 1000 : -1, freq2 >= 0 ? freq2 / 1000 : -1,
        mem_total, mem_avail,
        volts_j, thr_j,
        thr_bits >= 0 ? ((thr_bits & 0x1)     ? "true" : "false") : "null",
        thr_bits >= 0 ? ((thr_bits & 0x10000) ? "true" : "false") : "null",
        nebula_if_up ? "true" : "false");

    if (have_metrics) {
        char iso_esc[48], mode_esc[16];
        jesc(mf[0], iso_esc, sizeof(iso_esc));
        jesc(mf[2], mode_esc, sizeof(mode_esc));
        o += (size_t)snprintf(out + o, cap - o,
            "\"ingestor\":{\"reported_at\":\"%s\",\"uptime_s\":%s,"
            "\"mode\":\"%s\",\"samples_per_s\":%s,\"anom_rows_total\":%s,"
            "\"drain_rate\":%s,\"segments_done\":%s,\"transitions\":%s,"
            "\"backlog_bytes\":%s,\"disk_pct\":%s,\"disk_paused\":%s,"
            "\"rss_kb\":%s,\"ts_offset_valid\":%s,"
            "\"svc\":{\"nebula\":%s,\"operator\":%s,\"master\":%s,\"clock\":%s}}}",
            iso_esc, mf[1], mode_esc, mf[3], mf[4], mf[5], mf[6], mf[7],
            mf[10], mf[11], mf[12], mf[16], mf[17],
            mf[18], mf[19], mf[20], mf[21]);
    } else {
        o += (size_t)snprintf(out + o, cap - o, "\"ingestor\":null}");
    }
    return o;
}

/* ── embedded HTML page ───────────────────────────────────────────────── */

static const char PAGE_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>EADS Node</title><style>"
"body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0d1117;"
"color:#e6edf3;margin:0;padding:16px}h1{font-size:18px;margin:0 0 2px}"
"#sub{color:#8b949e;font-size:12px;margin-bottom:14px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px}"
".k{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px}"
".v{font-size:20px;font-weight:600;margin-top:2px}"
".badges{display:flex;gap:8px;flex-wrap:wrap;margin:14px 0}"
".b{padding:4px 10px;border-radius:12px;font-size:12px;font-weight:600}"
".ok{background:#1a3a24;color:#3fb950}.bad{background:#3d1d20;color:#f85149}"
".na{background:#21262d;color:#8b949e}"
".mode-NORMAL{color:#3fb950}.mode-ANOMALY{color:#f85149}"
"#err{color:#f85149;font-size:12px;display:none;margin-bottom:8px}"
"</style></head><body>"
"<h1>EADS <span id='node'>node</span> <span id='mode'></span></h1>"
"<div id='sub'>overlay <span id='ip'>-</span> · updated <span id='ts'>-</span></div>"
"<div id='err'>status fetch failed — retrying</div>"
"<div class='badges' id='badges'></div>"
"<div class='grid' id='grid'></div>"
"<script>"
"const F=(v,d)=>v==null||v<0?'\\u2014':(+v).toFixed(d==null?0:d);"
"function badge(n,v){const c=v==null?'na':(v?'ok':'bad');"
"const t=v==null?'n/a':(v?'OK':'DOWN');"
"return `<span class='b ${c}'>${n}: ${t}</span>`}"
"function card(k,v){return `<div class='card'><div class='k'>${k}</div>"
"<div class='v'>${v}</div></div>`}"
"async function tick(){try{"
"const r=await fetch('/api/status');const s=await r.json();"
"document.getElementById('err').style.display='none';"
"document.getElementById('node').textContent=s.node;"
"document.getElementById('ip').textContent=s.nebula_ip||'?';"
"document.getElementById('ts').textContent=s.now;"
"const h=s.host,i=s.ingestor;"
"const m=document.getElementById('mode');"
"if(i){m.textContent=i.mode;m.className='mode-'+i.mode}else{m.textContent=''}"
"let bd='';"
"if(i&&i.svc){bd+=badge('nebula',i.svc.nebula==1)+badge('operator',i.svc.operator==1)"
"+badge('master',i.svc.master==1)+badge('clock',i.svc.clock==1)}"
"else{bd+=badge('nebula if',h.nebula_if_up)}"
"bd+=badge('power',h.undervolt_now==null?null:!h.undervolt_now);"
"document.getElementById('badges').innerHTML=bd;"
"let g='';"
"g+=card('CPU temp',F(h.cpu_temp_c,1)+' °C');"
"g+=card('Load (1m)',F(h.load1,2));"
"g+=card('Mem free',F(h.mem_avail_kb/1024)+' MB');"
"g+=card('Core 0 MHz',F(h.freq_mhz_core0));"
"g+=card('Core 2 MHz',F(h.freq_mhz_core2));"
"if(h.core_volts!=null)g+=card('Core volts',F(h.core_volts,3)+' V');"
"g+=card('Host uptime',F(h.uptime_s/3600,1)+' h');"
"if(i){"
"g+=card('Samples/s',F(i.samples_per_s));"
"g+=card('Drain rate',F(i.drain_rate)+'/s');"
"g+=card('Backlog',F(i.backlog_bytes/1048576,1)+' MB');"
"g+=card('Disk used',F(i.disk_pct,1)+' %');"
"g+=card('Segments done',F(i.segments_done));"
"g+=card('Anomaly rows',F(i.anom_rows_total));"
"g+=card('Ingestor RSS',F(i.rss_kb/1024,1)+' MB');"
"}else{g+=card('Ingestor','no metrics yet')}"
"document.getElementById('grid').innerHTML=g;"
"}catch(e){document.getElementById('err').style.display='block'}}"
"tick();setInterval(tick,5000);"
"</script></body></html>";

/* ── HTTP plumbing ────────────────────────────────────────────────────── */

static void send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void respond(int fd, const char *status, const char *ctype,
                    const char *body, size_t blen) {
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, ctype, blen);
    send_all(fd, hdr, (size_t)hl);
    send_all(fd, body, blen);
}

int main(int argc, char **argv) {
    int port = PORT_DEFAULT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv, 8) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[dashboard] listening on :%d\n", port);

    static char req[REQ_BUF];
    static char resp[RESP_BUF];

    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        struct timeval tv = { .tv_sec = IO_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
        if (n > 0) {
            req[n] = '\0';
            if (strncmp(req, "GET /api/status", 15) == 0) {
                size_t blen = build_status_json(resp, sizeof(resp));
                respond(fd, "200 OK", "application/json", resp, blen);
            } else if (strncmp(req, "GET / ", 6) == 0 ||
                       strncmp(req, "GET /\r", 6) == 0) {
                respond(fd, "200 OK", "text/html; charset=utf-8",
                        PAGE_HTML, sizeof(PAGE_HTML) - 1);
            } else {
                respond(fd, "404 Not Found", "text/plain", "not found\n", 10);
            }
        }
        close(fd);
    }
    close(srv);
    fprintf(stderr, "[dashboard] stopped\n");
    return 0;
}
