/*
 * anomaly_buffer.c — implementation
 */

#define _GNU_SOURCE
#include "anomaly_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    if (mkdir(path, 0755) == 0) return 1;
    if (errno == EEXIST) return 1;
    fprintf(stderr, "[AB] mkdir(%s) failed: %s\n", path, strerror(errno));
    return 0;
}

int ab_init(anomaly_buffer_t *ab, const char *root_dir) {
    memset(ab, 0, sizeof(*ab));
    pthread_mutex_init(&ab->mutex, NULL);

    snprintf(ab->root_dir, sizeof(ab->root_dir), "%s", root_dir);
    snprintf(ab->segments_dir, sizeof(ab->segments_dir),
             "%s/segments", root_dir);
    snprintf(ab->index_path, sizeof(ab->index_path),
             "%s/index.bin", root_dir);

    if (!ensure_dir(ab->root_dir)) return 0;
    if (!ensure_dir(ab->segments_dir)) return 0;

    /* Open or create index file */
    ab->index_fd = open(ab->index_path, O_RDWR | O_CREAT, 0644);
    if (ab->index_fd < 0) {
        fprintf(stderr, "[AB] open index: %s\n", strerror(errno));
        return 0;
    }

    /* Size the file to exactly sizeof(index_file_t) */
    if (ftruncate(ab->index_fd, sizeof(index_file_t)) != 0) {
        fprintf(stderr, "[AB] ftruncate index: %s\n", strerror(errno));
        close(ab->index_fd);
        return 0;
    }

    ab->index = mmap(NULL, sizeof(index_file_t),
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     ab->index_fd, 0);
    if (ab->index == MAP_FAILED) {
        fprintf(stderr, "[AB] mmap index: %s\n", strerror(errno));
        close(ab->index_fd);
        return 0;
    }

    /* Initialize header if fresh */
    if (ab->index->magic != EADS_INDEX_MAGIC) {
        memset(ab->index, 0, sizeof(index_file_t));
        ab->index->magic       = EADS_INDEX_MAGIC;
        ab->index->version     = EADS_INDEX_VERSION;
        ab->index->max_entries = MAX_SEGMENTS;
        msync(ab->index, sizeof(index_file_t), MS_SYNC);
        fprintf(stderr, "[AB] Initialized fresh index at %s\n", ab->index_path);
    } else {
        int active = 0, ready = 0, draining = 0, done = 0;
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            switch (ab->index->entries[i].status) {
                case SEG_ACTIVE:    active++;   break;
                case SEG_READY:     ready++;    break;
                case SEG_DRAINING:  draining++; break;
                case SEG_DONE:      done++;     break;
            }
        }
        fprintf(stderr,
                "[AB] Loaded existing index: active=%d ready=%d draining=%d done=%d\n",
                active, ready, draining, done);
    }

    return 1;
}

void ab_close(anomaly_buffer_t *ab) {
    if (ab->index && ab->index != MAP_FAILED) {
        msync(ab->index, sizeof(index_file_t), MS_SYNC);
        munmap(ab->index, sizeof(index_file_t));
    }
    if (ab->index_fd > 0) close(ab->index_fd);
    pthread_mutex_destroy(&ab->mutex);
}

int ab_alloc_slot(anomaly_buffer_t *ab,
                  const char *anomaly_id,
                  uint64_t start_us,
                  char *out_filename, size_t fnlen) {
    pthread_mutex_lock(&ab->mutex);

    /* First, free any DONE slots (file already deleted) */
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (ab->index->entries[i].status == SEG_DONE) {
            memset(&ab->index->entries[i], 0, sizeof(segment_entry_t));
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (ab->index->entries[i].status == SEG_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&ab->mutex);
        fprintf(stderr, "[AB] No free slots\n");
        return -1;
    }

    /* Build filename: <ISO timestamp>_<anomaly_id>.bin */
    time_t t = (time_t)(start_us / 1000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y%m%dT%H%M%S", &tm);

    char id_short[17];
    memcpy(id_short, anomaly_id, 16);
    id_short[16] = '\0';

    segment_entry_t *e = &ab->index->entries[slot];
    memset(e, 0, sizeof(*e));
    memcpy(e->anomaly_id, anomaly_id, 16);
    snprintf(e->filename, sizeof(e->filename), "%s_%s.bin", tsbuf, id_short);
    e->start_us       = start_us;
    e->end_us         = 0;
    e->total_bytes    = 0;
    e->uploaded_bytes = 0;
    e->created_unix   = (uint64_t)time(NULL);
    e->status         = SEG_ACTIVE;
    msync(e, sizeof(*e), MS_SYNC);

    if (out_filename) snprintf(out_filename, fnlen, "%s", e->filename);
    pthread_mutex_unlock(&ab->mutex);
    return slot;
}

void ab_set_status(anomaly_buffer_t *ab, int slot, uint8_t status) {
    if (slot < 0 || slot >= MAX_SEGMENTS) return;
    pthread_mutex_lock(&ab->mutex);
    ab->index->entries[slot].status = status;
    pthread_mutex_unlock(&ab->mutex);
}

void ab_set_total_bytes(anomaly_buffer_t *ab, int slot, uint64_t bytes) {
    if (slot < 0 || slot >= MAX_SEGMENTS) return;
    pthread_mutex_lock(&ab->mutex);
    ab->index->entries[slot].total_bytes = bytes;
    pthread_mutex_unlock(&ab->mutex);
}

void ab_set_end_us(anomaly_buffer_t *ab, int slot, uint64_t end_us) {
    if (slot < 0 || slot >= MAX_SEGMENTS) return;
    pthread_mutex_lock(&ab->mutex);
    ab->index->entries[slot].end_us = end_us;
    pthread_mutex_unlock(&ab->mutex);
}

void ab_set_uploaded(anomaly_buffer_t *ab, int slot, uint64_t bytes) {
    if (slot < 0 || slot >= MAX_SEGMENTS) return;
    pthread_mutex_lock(&ab->mutex);
    ab->index->entries[slot].uploaded_bytes = bytes;
    pthread_mutex_unlock(&ab->mutex);
}

void ab_fsync_index(anomaly_buffer_t *ab) {
    pthread_mutex_lock(&ab->mutex);
    msync(ab->index, sizeof(index_file_t), MS_SYNC);
    pthread_mutex_unlock(&ab->mutex);
}

segment_entry_t ab_get_entry(anomaly_buffer_t *ab, int slot) {
    segment_entry_t e = {0};
    if (slot < 0 || slot >= MAX_SEGMENTS) return e;
    pthread_mutex_lock(&ab->mutex);
    e = ab->index->entries[slot];
    pthread_mutex_unlock(&ab->mutex);
    return e;
}

int ab_find_drainable(anomaly_buffer_t *ab) {
    pthread_mutex_lock(&ab->mutex);
    int best = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t s = ab->index->entries[i].status;
        if (s == SEG_READY || s == SEG_DRAINING) {
            if (ab->index->entries[i].created_unix < oldest) {
                oldest = ab->index->entries[i].created_unix;
                best = i;
            }
        }
    }
    pthread_mutex_unlock(&ab->mutex);
    return best;
}

void ab_recover_after_restart(anomaly_buffer_t *ab) {
    pthread_mutex_lock(&ab->mutex);
    int recovered = 0;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        segment_entry_t *e = &ab->index->entries[i];
        if (e->status == SEG_ACTIVE) {
            /* Was being written when we crashed; mark for upload as-is */
            char full[600];
            snprintf(full, sizeof(full), "%s/%s",
                     ab->segments_dir, e->filename);
            struct stat st;
            if (stat(full, &st) == 0) {
                /* Round DOWN to a whole-sample boundary. A container killed
                 * mid-write can leave a partial (<48 byte) sample at the end;
                 * including it would make total_bytes un-reachable by the
                 * drain (which only reads complete samples) and cause an
                 * infinite partial-retry loop. */
                uint64_t sz = (uint64_t)st.st_size;
                sz -= (sz % sizeof(segment_sample_t));
                e->total_bytes = sz;
                e->status = SEG_READY;
                fprintf(stderr,
                        "[AB] Recovered segment slot %d: %s (%llu bytes, "
                        "%llu samples)\n",
                        i, e->filename, (unsigned long long)e->total_bytes,
                        (unsigned long long)(e->total_bytes / sizeof(segment_sample_t)));
                recovered++;
            } else {
                /* File missing — free the slot */
                memset(e, 0, sizeof(*e));
            }
        } else if (e->status == SEG_DRAINING) {
            /* Was being uploaded; resume from uploaded_bytes */
            fprintf(stderr,
                    "[AB] Resuming drain of slot %d: %s "
                    "(uploaded %llu / %llu bytes)\n",
                    i, e->filename,
                    (unsigned long long)e->uploaded_bytes,
                    (unsigned long long)e->total_bytes);
            recovered++;
        } else if (e->status == SEG_DONE) {
            memset(e, 0, sizeof(*e));
        }
    }
    msync(ab->index, sizeof(index_file_t), MS_SYNC);
    pthread_mutex_unlock(&ab->mutex);
    if (recovered) {
        fprintf(stderr, "[AB] Recovery complete: %d segments resumed\n",
                recovered);
    }
}

int ab_delete_segment(anomaly_buffer_t *ab, int slot) {
    if (slot < 0 || slot >= MAX_SEGMENTS) return -1;
    pthread_mutex_lock(&ab->mutex);
    segment_entry_t *e = &ab->index->entries[slot];
    char full[600];
    snprintf(full, sizeof(full), "%s/%s",
             ab->segments_dir, e->filename);
    int r = unlink(full);
    if (r != 0 && errno != ENOENT) {
        fprintf(stderr, "[AB] unlink(%s): %s\n", full, strerror(errno));
    }
    e->status = SEG_DONE;
    msync(e, sizeof(*e), MS_SYNC);
    pthread_mutex_unlock(&ab->mutex);
    return r;
}

int ab_disk_pct(anomaly_buffer_t *ab, double *out_pct) {
    struct statvfs st;
    if (statvfs(ab->root_dir, &st) != 0) return 0;
    uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
    uint64_t avail = (uint64_t)st.f_bavail * st.f_frsize;
    if (total == 0) return 0;
    if (out_pct) *out_pct = 100.0 * (total - avail) / total;
    return 1;
}

uint64_t ab_total_backlog_bytes(anomaly_buffer_t *ab) {
    pthread_mutex_lock(&ab->mutex);
    uint64_t total = 0;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t s = ab->index->entries[i].status;
        if (s == SEG_READY || s == SEG_DRAINING || s == SEG_ACTIVE) {
            total += ab->index->entries[i].total_bytes
                   - ab->index->entries[i].uploaded_bytes;
        }
    }
    pthread_mutex_unlock(&ab->mutex);
    return total;
}

int ab_enforce_backlog_cap(anomaly_buffer_t *ab, uint64_t max_bytes) {
    /* DATA RETENTION POLICY (anomaly protection):
     * Un-acked anomaly segments are NEVER deleted — they are the entire
     * scientific point of the device, and during a long internet outage
     * the backlog growing past the soft cap is exactly the situation in
     * which that evidence matters most. This function therefore no longer
     * purges anything; it reports pressure so the caller can warn loudly.
     * The disk-usage safeguard (pause at --disk-pause-pct, default 85%)
     * remains the hard stop that prevents the card from actually filling:
     * it halts NEW writes while preserving everything already captured.
     * Segments are still deleted on the normal path: only after AnyLog
     * acks their final batch (SEG_DONE in the drain loop).
     * Returns 1 if the backlog exceeds max_bytes, else 0. */
    uint64_t bl = ab_total_backlog_bytes(ab);
    if (bl > max_bytes) {
        fprintf(stderr,
                "[AB] BACKLOG OVER SOFT CAP: %.1f MB unsent (> %llu MB). "
                "Protected anomaly data is NOT purged; drain will catch up "
                "when the master is reachable. Disk safeguard pauses new "
                "writes if the card approaches full.\n",
                bl / 1024.0 / 1024.0,
                (unsigned long long)(max_bytes / 1024 / 1024));
        return 1;
    }
    return 0;
}

/* ── Segment writer ─────────────────────────────────────────────────── */
int segwriter_open(segment_writer_t *w, anomaly_buffer_t *ab,
                   const char *anomaly_id, uint64_t start_us) {
    memset(w, 0, sizeof(*w));
    char filename[SEGMENT_FILENAME_LEN];
    int slot = ab_alloc_slot(ab, anomaly_id, start_us,
                             filename, sizeof(filename));
    if (slot < 0) return -1;
    w->slot = slot;
    memcpy(w->anomaly_id, anomaly_id, 16);
    w->anomaly_id[16] = '\0';
    snprintf(w->full_path, sizeof(w->full_path),
             "%s/%s", ab->segments_dir, filename);
    w->fp = fopen(w->full_path, "wb");
    if (!w->fp) {
        fprintf(stderr, "[SegW] fopen(%s): %s\n",
                w->full_path, strerror(errno));
        ab_set_status(ab, slot, SEG_FREE);
        return -1;
    }
    /* 64KB buffer for fwrite */
    setvbuf(w->fp, NULL, _IOFBF, 65536);
    w->bytes_written = 0;
    w->last_fsync_us = now_us();
    w->fsync_interval_us = 5ULL * 1000000ULL;  /* fsync every 5 sec */
    return 0;
}

int segwriter_append(segment_writer_t *w, anomaly_buffer_t *ab,
                     const segment_sample_t *sample) {
    return segwriter_append_array(w, ab, sample, 1);
}

int segwriter_append_array(segment_writer_t *w, anomaly_buffer_t *ab,
                           const segment_sample_t *samples, size_t n) {
    if (!w->fp) return -1;
    if (w->bytes_written + n * sizeof(segment_sample_t) > MAX_SEGMENT_BYTES) {
        return -2;  /* segment full — caller should rotate */
    }
    size_t r = fwrite(samples, sizeof(segment_sample_t), n, w->fp);
    if (r != n) {
        fprintf(stderr, "[SegW] short write: %zu/%zu (%s)\n",
                r, n, strerror(errno));
        return -1;
    }
    w->bytes_written += n * sizeof(segment_sample_t);

    /* Periodic fsync */
    uint64_t now = now_us();
    if (now - w->last_fsync_us >= w->fsync_interval_us) {
        fflush(w->fp);
        fsync(fileno(w->fp));
        w->last_fsync_us = now;
        /* Also update total_bytes in index periodically */
        ab_set_total_bytes(ab, w->slot, w->bytes_written);
    }
    return 0;
}

int segwriter_close(segment_writer_t *w, anomaly_buffer_t *ab,
                    uint64_t end_us) {
    if (!w->fp) return -1;
    fflush(w->fp);
    fsync(fileno(w->fp));
    fclose(w->fp);
    w->fp = NULL;
    ab_set_total_bytes(ab, w->slot, w->bytes_written);
    ab_set_end_us(ab, w->slot, end_us);
    ab_set_status(ab, w->slot, SEG_READY);
    ab_fsync_index(ab);
    fprintf(stderr,
            "[SegW] Closed slot %d: %s  bytes=%llu  samples=%llu  end_us=%llu\n",
            w->slot, w->full_path,
            (unsigned long long)w->bytes_written,
            (unsigned long long)(w->bytes_written / sizeof(segment_sample_t)),
            (unsigned long long)end_us);
    return 0;
}

/* ── Segment reader ─────────────────────────────────────────────────── */
int segreader_open(segment_reader_t *r, anomaly_buffer_t *ab, int slot) {
    memset(r, 0, sizeof(*r));
    segment_entry_t e = ab_get_entry(ab, slot);
    r->slot = slot;
    r->total_bytes = e.total_bytes;
    r->bytes_read  = e.uploaded_bytes;
    snprintf(r->full_path, sizeof(r->full_path),
             "%s/%s", ab->segments_dir, e.filename);
    r->fp = fopen(r->full_path, "rb");
    if (!r->fp) {
        fprintf(stderr, "[SegR] fopen(%s): %s\n",
                r->full_path, strerror(errno));
        return -1;
    }
    if (r->bytes_read > 0) {
        fseek(r->fp, (long)r->bytes_read, SEEK_SET);
    }
    return 0;
}

ssize_t segreader_read(segment_reader_t *r, segment_sample_t *out, size_t max_n) {
    if (!r->fp) return -1;
    if (r->bytes_read >= r->total_bytes) return 0;
    size_t remaining = (r->total_bytes - r->bytes_read) / sizeof(segment_sample_t);
    size_t want = max_n < remaining ? max_n : remaining;
    size_t got = fread(out, sizeof(segment_sample_t), want, r->fp);
    r->bytes_read += got * sizeof(segment_sample_t);
    return (ssize_t)got;
}

void segreader_close(segment_reader_t *r) {
    if (r->fp) { fclose(r->fp); r->fp = NULL; }
}
