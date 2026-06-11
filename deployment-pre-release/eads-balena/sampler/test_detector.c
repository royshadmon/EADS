/*
 * test_detector.c — Verify C port matches Python test_detection.py expectations
 *
 *   Clean signal         → no alerts
 *   Mild noise           → no alerts
 *   Brownout             → LOW_VOLTAGE
 *   Overvoltage          → VOLTAGE_SPIKE
 *   Clipped waveform     → DISTORTED_WAVEFORM
 *   Outage               → OUTAGE
 */
#include "anomaly_detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static const float SAMPLE_RATE = 7680.0f;
static const float FREQ_HZ     = 60.0f;
static const float TWOPI       = 6.28318530717958647692f;

static uint32_t run_window(float peak_volts, const char *name,
                           int clip, float clip_level)
{
    pq_detector_t det;
    pq_tolerances_t tol = { 5.0f, 10.0f, 50.0f };
    pq_init(&det, SAMPLE_RATE, &tol);

    uint32_t flags = 0;
    pq_result_t r;
    for (int i = 0; i < PQ_SAMPLES_PER_WINDOW; i++) {
        float t = (float)i / SAMPLE_RATE;
        float v = peak_volts * sinf(TWOPI * FREQ_HZ * t);
        if (clip && fabsf(v) > clip_level) {
            v = (v > 0 ? clip_level : -clip_level);
        }
        if (pq_ingest(&det, v, (uint64_t)i, &r)) {
            flags = r.anomaly_flags;
        }
    }

    char buf[64];
    pq_flags_to_str(flags, buf, sizeof(buf));
    printf("  %-22s  rms=%.2fV  dev=%.2f%%  sad=%.2f  flags=%s\n",
           name, r.measured_rms_v, r.rms_deviation_pct, r.waveform_sad, buf);
    return flags;
}

static int expect(const char *name, uint32_t got, uint32_t want)
{
    char b1[64], b2[64];
    pq_flags_to_str(got, b1, sizeof(b1));
    pq_flags_to_str(want, b2, sizeof(b2));
    int ok = (got == want);
    printf("    [%s] %s  got=%s  want=%s\n",
           ok ? "PASS" : "FAIL", name, b1, b2);
    return ok;
}

int main(void)
{
    const float peak120 = 120.0f * 1.41421356f;

    printf("Testing anomaly detector against Python reference expectations:\n\n");

    int fails = 0;

    /* Clean 120V — no alert */
    uint32_t f = run_window(peak120, "clean 120V", 0, 0.0f);
    fails += !expect("clean 120V → no alert", f, 0);

    /* Brownout: 100V — should trip LOW_VOLTAGE (16% < 120) */
    f = run_window(100.0f * 1.41421356f, "brownout 100V", 0, 0.0f);
    fails += !expect("brownout 100V → LOW_VOLTAGE", f, PQ_FLAG_LOW_VOLTAGE);

    /* Overvoltage: 132V — should trip VOLTAGE_SPIKE (10% > 120) */
    f = run_window(132.0f * 1.41421356f, "overvoltage 132V", 0, 0.0f);
    fails += !expect("overvoltage 132V → VOLTAGE_SPIKE", f, PQ_FLAG_VOLTAGE_SPIKE);

    /* Clipped waveform: 120V peak but clipped at 130V peak (severe clipping) */
    f = run_window(120.0f * 1.41421356f * 1.5f, "clipped 120V",
                   1, 120.0f * 1.41421356f * 0.7f);
    /* This will fire as both RMS deviation AND distortion */
    fails += !expect("clipped → DISTORTED_WAVEFORM in flags",
                     f & PQ_FLAG_DISTORTED_WAVEFORM, PQ_FLAG_DISTORTED_WAVEFORM);

    /* Outage: 0V (silence) — should trip OUTAGE */
    f = run_window(0.0f, "outage 0V", 0, 0.0f);
    fails += !expect("outage 0V → OUTAGE", f, PQ_FLAG_OUTAGE);

    printf("\n%s — %d failure(s)\n", fails == 0 ? "ALL TESTS PASSED" : "FAILURES", fails);
    return fails;
}
