#include "pretrigger_ring.h"
#include <string.h>

void pretrigger_init(pretrigger_ring_t *r) {
    memset(r->buf, 0, sizeof(r->buf));
    r->total_written = 0;
    pthread_mutex_init(&r->snap_mutex, NULL);
}

void pretrigger_push(pretrigger_ring_t *r, const segment_sample_t *s) {
    /* Protect snapshot state while adding a sample. */
    pthread_mutex_lock(&r->snap_mutex);
    size_t idx = r->total_written % PRETRIGGER_CAPACITY;
    r->buf[idx] = *s;
    r->total_written++;
    pthread_mutex_unlock(&r->snap_mutex);
}

size_t pretrigger_snapshot(pretrigger_ring_t *r,
                           segment_sample_t *out, size_t cap) {
    pthread_mutex_lock(&r->snap_mutex);
    uint64_t total = r->total_written;
    size_t count = (total < PRETRIGGER_CAPACITY)
                   ? (size_t)total
                   : PRETRIGGER_CAPACITY;
    if (count > cap) count = cap;
    if (total < PRETRIGGER_CAPACITY) {
        /* Ring not yet full — samples start at index 0 */
        memcpy(out, r->buf, count * sizeof(segment_sample_t));
    } else {
        /* Ring is full — oldest is at (total % CAPACITY) */
        size_t start = total % PRETRIGGER_CAPACITY;
        size_t tail  = PRETRIGGER_CAPACITY - start;
        memcpy(out, &r->buf[start], tail * sizeof(segment_sample_t));
        memcpy(out + tail, r->buf, start * sizeof(segment_sample_t));
    }
    pthread_mutex_unlock(&r->snap_mutex);
    return count;
}
