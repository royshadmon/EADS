/*
 * anomaly_buffer.h — Persistent SD-backed anomaly capture
 *
 * One segment file per anomaly event under <root>/segments/.
 * One central index file at <root>/index.bin tracks each segment's state.
 * Drain thread reads segments at adaptive rate, sends to AnyLog, deletes
 * segment file when fully uploaded.
 *
 * Concurrency model:
 *   - Reader thread writes to one segment at a time via segment_writer_t
 *   - Drain thread only reads segments with status READY or DRAINING
 *   - Index mutex protects index reads/writes
 */

#ifndef ANOMALY_BUFFER_H
#define ANOMALY_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>

/* On-disk sample format (48 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t ts_us;
    int32_t  raw_count;
    float    adc_input;
    float    adc_centered;
    float    voltage_est;
    char     anomaly_id[16];   /* NOT null-terminated */
    uint8_t  is_anomaly;        /* 1 for anomaly samples, 0 for pre-trigger context */
    uint8_t  reserved[3];
} segment_sample_t;

#define MAX_SEGMENTS         256
#define SEGMENT_FILENAME_LEN 80
#define MAX_SEGMENT_BYTES    (1ULL << 30)   /* 1 GB max per segment file  */

/* Status values for segment_entry_t.status */
#define SEG_FREE       0
#define SEG_ACTIVE     1   /* currently being written by reader */
#define SEG_READY      2   /* writing complete, awaiting drain */
#define SEG_DRAINING   3   /* drain in progress */
#define SEG_DONE       4   /* fully uploaded, file deleted (slot will be freed) */

/* Index entry (160 bytes; packed, persistent on disk) */
typedef struct __attribute__((packed)) {
    char     anomaly_id[16];
    char     filename[SEGMENT_FILENAME_LEN];
    uint64_t start_us;
    uint64_t end_us;
    uint64_t total_bytes;
    uint64_t uploaded_bytes;
    uint64_t created_unix;
    uint8_t  status;
    uint8_t  flags;
    uint8_t  reserved[14];
} segment_entry_t;

/* On-disk index header */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* 0xEADBUF01 */
    uint32_t version;
    uint32_t max_entries;
    uint32_t reserved;
    segment_entry_t entries[MAX_SEGMENTS];
} index_file_t;

#define EADS_INDEX_MAGIC    0xEAD0BF01
#define EADS_INDEX_VERSION  1

/* Runtime anomaly buffer manager */
typedef struct {
    char            root_dir[256];
    char            segments_dir[256];
    char            index_path[256];

    int             index_fd;
    index_file_t   *index;          /* mmap'd */

    pthread_mutex_t mutex;          /* protects index access */
} anomaly_buffer_t;

/* Initialize the buffer manager. Creates directories, opens/creates index.
 * Returns 1 on success, 0 on failure. */
int  ab_init(anomaly_buffer_t *ab, const char *root_dir);
void ab_close(anomaly_buffer_t *ab);

/* Find slot with status==SEG_FREE, allocate, return slot index. -1 if full. */
int  ab_alloc_slot(anomaly_buffer_t *ab,
                   const char *anomaly_id,
                   uint64_t start_us,
                   char *out_filename, size_t fnlen);

/* Update slot fields (acquires mutex) */
void ab_set_status(anomaly_buffer_t *ab, int slot, uint8_t status);
void ab_set_total_bytes(anomaly_buffer_t *ab, int slot, uint64_t bytes);
void ab_set_end_us(anomaly_buffer_t *ab, int slot, uint64_t end_us);
void ab_set_uploaded(anomaly_buffer_t *ab, int slot, uint64_t bytes);
void ab_fsync_index(anomaly_buffer_t *ab);

/* Get snapshot of an entry */
segment_entry_t ab_get_entry(anomaly_buffer_t *ab, int slot);

/* Find next slot in status READY (or DRAINING — resumes after restart).
 * Returns slot index or -1 if none ready. Oldest first. */
int  ab_find_drainable(anomaly_buffer_t *ab);

/* Recovery: on startup, mark all status=ACTIVE entries as READY
 * (with current file size as total_bytes) since they didn't close cleanly. */
void ab_recover_after_restart(anomaly_buffer_t *ab);

/* Delete segment file and free the slot */
int  ab_delete_segment(anomaly_buffer_t *ab, int slot);

/* Disk usage helper */
int  ab_disk_pct(anomaly_buffer_t *ab, double *out_pct);

/* Backlog management: total un-uploaded bytes across all segments.
 * ab_total_backlog_bytes returns the sum. ab_enforce_backlog_cap is a
 * PRESSURE CHECK ONLY: un-acked anomaly segments are never deleted (data
 * retention policy — captured anomaly evidence is protected; the disk
 * safeguard pauses new writes instead). Returns 1 if backlog > max_bytes,
 * 0 otherwise. Segments are deleted only after AnyLog acks them. */
uint64_t ab_total_backlog_bytes(anomaly_buffer_t *ab);
int      ab_enforce_backlog_cap(anomaly_buffer_t *ab, uint64_t max_bytes);

/* ── Segment writer — one active segment at a time ─────────────────── */
typedef struct {
    FILE    *fp;
    char     full_path[512];
    int      slot;
    char     anomaly_id[17];
    uint64_t bytes_written;
    uint64_t last_fsync_us;
    uint64_t fsync_interval_us;
} segment_writer_t;

/* Open a new segment file. Allocates a slot, creates the file. */
int  segwriter_open(segment_writer_t *w, anomaly_buffer_t *ab,
                    const char *anomaly_id, uint64_t start_us);
int  segwriter_append(segment_writer_t *w, anomaly_buffer_t *ab,
                      const segment_sample_t *sample);
int  segwriter_append_array(segment_writer_t *w, anomaly_buffer_t *ab,
                            const segment_sample_t *samples, size_t n);
int  segwriter_close(segment_writer_t *w, anomaly_buffer_t *ab,
                     uint64_t end_us);

/* ── Segment reader — used by drain thread ─────────────────────────── */
typedef struct {
    FILE    *fp;
    char     full_path[512];
    int      slot;
    uint64_t bytes_read;
    uint64_t total_bytes;
} segment_reader_t;

int  segreader_open(segment_reader_t *r, anomaly_buffer_t *ab, int slot);
ssize_t segreader_read(segment_reader_t *r, segment_sample_t *out, size_t max_n);
void segreader_close(segment_reader_t *r);

#endif
