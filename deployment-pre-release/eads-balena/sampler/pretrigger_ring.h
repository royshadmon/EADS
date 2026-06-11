/*
 * pretrigger_ring.h — 2-second RAM circular buffer
 *
 * Reader always pushes samples here regardless of mode.
 * On NORMAL→ANOMALY transition, contents get flushed to SD segment file
 * to provide forensic context of what was happening just before the trigger.
 *
 * Single-writer (reader thread), single-reader (transition handler).
 * Lockless writes since only one thread writes; flush takes a snapshot.
 */

#ifndef PRETRIGGER_RING_H
#define PRETRIGGER_RING_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "anomaly_buffer.h"

#define PRETRIGGER_SECONDS  2
#define PRETRIGGER_RATE     7680
#define PRETRIGGER_CAPACITY (PRETRIGGER_SECONDS * PRETRIGGER_RATE)  /* 15360 */

typedef struct {
    segment_sample_t buf[PRETRIGGER_CAPACITY];
    volatile uint64_t total_written;   /* monotonic, wraps via modulo */
    pthread_mutex_t   snap_mutex;      /* held during snapshot copy */
} pretrigger_ring_t;

void pretrigger_init(pretrigger_ring_t *r);

/* Append one sample. Lockless write — reader thread only. */
void pretrigger_push(pretrigger_ring_t *r, const segment_sample_t *s);

/* Snapshot ring contents in chronological order.
 * Writes up to PRETRIGGER_CAPACITY samples to out[].
 * Returns count written (may be < capacity if ring isn't full yet). */
size_t pretrigger_snapshot(pretrigger_ring_t *r,
                           segment_sample_t *out, size_t cap);

#endif
