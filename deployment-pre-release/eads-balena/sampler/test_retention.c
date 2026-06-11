/* test_retention.c — verifies the anomaly-protection retention policy.
 *
 * 1. Creates segments totalling well over a tiny soft cap, all SEG_READY
 *    (captured but un-acked anomaly data).
 * 2. Runs ab_enforce_backlog_cap with the tiny cap.
 * 3. Asserts: return value signals pressure, AND every segment + its file
 *    still exists (nothing purged).
 * 4. Sanity: the normal drain-complete path (ab_delete_segment after ack)
 *    still deletes, so this isn't "deletion is broken everywhere".
 *
 * Build: gcc -O2 -Wall -Wextra -o test_retention test_retention.c \
 *            anomaly_buffer.c -lpthread
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "anomaly_buffer.h"

#define TESTDIR "/tmp/ab_retention_test"

static int file_exists(const char *dir, const char *fn) {
    char p[512];
    snprintf(p, sizeof(p), "%s/segments/%s", dir, fn);
    struct stat st;
    return stat(p, &st) == 0;
}

int main(void) {
    if (system("rm -rf " TESTDIR) != 0) {}
    mkdir(TESTDIR, 0755);

    anomaly_buffer_t ab;
    assert(ab_init(&ab, TESTDIR));

    /* Create 4 segments x 1 MB of "anomaly data", all READY (un-acked). */
    enum { NSEG = 4, SEGSZ = 1024 * 1024 };
    char fnames[NSEG][64];
    char *blob = calloc(1, SEGSZ);
    for (int i = 0; i < NSEG; i++) {
        char aid[32];
        snprintf(aid, sizeof(aid), "anom%04d", i);
        int slot = ab_alloc_slot(&ab, aid, 1000000ULL * i,
                                 fnames[i], sizeof(fnames[i]));
        assert(slot >= 0);
        char path[512];
        snprintf(path, sizeof(path), "%s/segments/%s", TESTDIR, fnames[i]);
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(blob, 1, SEGSZ, f) == SEGSZ);
        fclose(f);
        ab_set_total_bytes(&ab, slot, SEGSZ);
        ab_set_status(&ab, slot, SEG_READY);     /* captured, NOT uploaded */
        /* stagger creation times so "oldest" is well-defined */
        ab.index->entries[slot].created_unix = 1000 + i;
    }
    free(blob);

    uint64_t backlog = ab_total_backlog_bytes(&ab);
    printf("backlog before cap check: %.1f MB\n", backlog / 1048576.0);
    assert(backlog == (uint64_t)NSEG * SEGSZ);

    /* Soft cap of 1 MB — backlog is 4x over it. */
    int over = ab_enforce_backlog_cap(&ab, 1 * 1024 * 1024);
    assert(over == 1 && "cap check must report pressure");

    /* THE policy assertion: nothing was deleted. */
    int ready = 0;
    for (int i = 0; i < MAX_SEGMENTS; i++)
        if (ab.index->entries[i].status == SEG_READY) ready++;
    printf("READY segments after cap check: %d (expected %d)\n", ready, NSEG);
    assert(ready == NSEG && "protected segments must survive the cap");
    for (int i = 0; i < NSEG; i++)
        assert(file_exists(TESTDIR, fnames[i]) &&
               "segment files must survive the cap");
    assert(ab_total_backlog_bytes(&ab) == (uint64_t)NSEG * SEGSZ);

    /* Run it again — idempotent, still nothing deleted. */
    assert(ab_enforce_backlog_cap(&ab, 1 * 1024 * 1024) == 1);
    assert(ab_total_backlog_bytes(&ab) == (uint64_t)NSEG * SEGSZ);

    /* Under the cap → no pressure reported. */
    assert(ab_enforce_backlog_cap(&ab, 100ULL * 1024 * 1024) == 0);

    /* Normal ack path still deletes: simulate drain completion of slot 0. */
    int slot0 = -1;
    for (int i = 0; i < MAX_SEGMENTS; i++)
        if (ab.index->entries[i].status == SEG_READY &&
            strcmp(ab.index->entries[i].filename, fnames[0]) == 0) slot0 = i;
    assert(slot0 >= 0);
    ab_set_uploaded(&ab, slot0, SEGSZ);          /* fully acked by AnyLog */
    assert(ab_delete_segment(&ab, slot0) == 0);
    assert(!file_exists(TESTDIR, fnames[0]));
    assert(ab_total_backlog_bytes(&ab) == (uint64_t)(NSEG - 1) * SEGSZ);
    printf("post-ack deletion still works (backlog now %.1f MB)\n",
           ab_total_backlog_bytes(&ab) / 1048576.0);

    ab_close(&ab);
    printf("ALL RETENTION POLICY TESTS PASSED\n");
    return 0;
}
