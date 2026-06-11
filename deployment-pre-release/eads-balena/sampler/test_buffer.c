/* Smoke test for anomaly_buffer + pretrigger_ring
 * Exercises: init, write segment, close, recover, read back, delete. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "anomaly_buffer.h"
#include "pretrigger_ring.h"

int main(void) {
    system("rm -rf /tmp/eads_test_buf");
    anomaly_buffer_t ab;
    if (!ab_init(&ab, "/tmp/eads_test_buf")) {
        fprintf(stderr, "ab_init failed\n");
        return 1;
    }

    /* Test 1: pretrigger ring */
    printf("Test 1: pretrigger ring\n");
    pretrigger_ring_t pr;
    pretrigger_init(&pr);
    for (int i = 0; i < 100; i++) {
        segment_sample_t s = {0};
        s.ts_us = 1000 + i;
        s.voltage_est = (float)i;
        pretrigger_push(&pr, &s);
    }
    segment_sample_t snap[PRETRIGGER_CAPACITY];
    size_t got = pretrigger_snapshot(&pr, snap, PRETRIGGER_CAPACITY);
    assert(got == 100);
    assert(snap[0].voltage_est == 0);
    assert(snap[99].voltage_est == 99);
    printf("  PASS: 100 samples, in order\n");

    /* Test 2: ring overflow */
    for (int i = 0; i < PRETRIGGER_CAPACITY * 2; i++) {
        segment_sample_t s = {0};
        s.ts_us = 9000 + i;
        s.voltage_est = (float)i;
        pretrigger_push(&pr, &s);
    }
    got = pretrigger_snapshot(&pr, snap, PRETRIGGER_CAPACITY);
    assert(got == PRETRIGGER_CAPACITY);
    /* Snapshot oldest sample's voltage_est should be at index (2*CAP - CAP) = CAP */
    assert(snap[PRETRIGGER_CAPACITY - 1].voltage_est ==
           (float)(PRETRIGGER_CAPACITY * 2 - 1));
    printf("  PASS: ring wraps correctly, latest sample preserved\n");

    /* Test 3: segment write + close */
    printf("Test 2: segment writer\n");
    segment_writer_t w;
    char anom_id[16];
    memcpy(anom_id, "abc1234567890def", 16);
    int r = segwriter_open(&w, &ab, anom_id, 1000000);
    assert(r == 0);
    for (int i = 0; i < 5000; i++) {
        segment_sample_t s = {0};
        s.ts_us = 1000000 + i * 130;
        s.voltage_est = 120.0f + (float)(i % 100);
        memcpy(s.anomaly_id, anom_id, 16);
        s.is_anomaly = 1;
        assert(segwriter_append(&w, &ab, &s) == 0);
    }
    r = segwriter_close(&w, &ab, 1000000 + 5000 * 130);
    assert(r == 0);
    printf("  PASS: wrote+closed 5000 samples\n");

    /* Test 4: find drainable */
    int slot = ab_find_drainable(&ab);
    assert(slot >= 0);
    printf("  PASS: found drainable slot=%d\n", slot);

    /* Test 5: read back */
    printf("Test 3: segment reader\n");
    segment_reader_t reader;
    r = segreader_open(&reader, &ab, slot);
    assert(r == 0);
    segment_sample_t buf[1000];
    size_t total_read = 0;
    while (1) {
        ssize_t n = segreader_read(&reader, buf, 1000);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            assert(buf[i].is_anomaly == 1);
            assert(memcmp(buf[i].anomaly_id, anom_id, 16) == 0);
        }
        total_read += (size_t)n;
    }
    segreader_close(&reader);
    assert(total_read == 5000);
    printf("  PASS: read back %zu samples\n", total_read);

    /* Test 6: delete */
    r = ab_delete_segment(&ab, slot);
    assert(r == 0);
    printf("  PASS: segment deleted\n");

    /* Test 7: recovery — create ACTIVE entry, close+reopen */
    printf("Test 4: recovery\n");
    memcpy(anom_id, "deadbeefcafebabe", 16);
    r = segwriter_open(&w, &ab, anom_id, 2000000);
    assert(r == 0);
    for (int i = 0; i < 100; i++) {
        segment_sample_t s = {0};
        s.ts_us = 2000000 + i;
        s.is_anomaly = 1;
        memcpy(s.anomaly_id, anom_id, 16);
        segwriter_append(&w, &ab, &s);
    }
    /* Force flush WITHOUT calling segwriter_close — simulates crash */
    fflush(w.fp);
    fclose(w.fp);
    w.fp = NULL;
    /* slot is left ACTIVE in index */
    ab_close(&ab);

    /* Reopen — recovery should mark ACTIVE as READY */
    if (!ab_init(&ab, "/tmp/eads_test_buf")) return 2;
    ab_recover_after_restart(&ab);
    slot = ab_find_drainable(&ab);
    assert(slot >= 0);
    segment_entry_t e = ab_get_entry(&ab, slot);
    printf("  PASS: recovered slot=%d  total_bytes=%llu  status=%u\n",
           slot, (unsigned long long)e.total_bytes, e.status);
    assert(e.status == SEG_READY);
    assert(e.total_bytes == 100 * sizeof(segment_sample_t));
    ab_delete_segment(&ab, slot);

    ab_close(&ab);
    system("rm -rf /tmp/eads_test_buf");
    printf("\nALL TESTS PASSED\n");
    return 0;
}
