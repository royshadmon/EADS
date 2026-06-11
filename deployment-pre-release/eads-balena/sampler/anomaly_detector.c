/*
 * anomaly_detector.c — implementation
 *
 * Direct C port of cycle_detector.py. Numerical results should match the
 * Python implementation to ~6 decimal places (float vs double precision).
 */

#include "anomaly_detector.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static const pq_tolerances_t PQ_DEFAULT_TOLERANCES = {
    .rms_deviation_pct = 5.0f,
    .outage_rms_v      = 10.0f,
    .waveform_sad      = 50.0f,
};

void pq_init(pq_detector_t *det, float sample_rate_hz,
             const pq_tolerances_t *tol)
{
    memset(det, 0, sizeof(*det));
    det->sample_rate_hz = sample_rate_hz > 0 ? sample_rate_hz
                                             : PQ_DEFAULT_SAMPLE_RATE;
    det->standard_rms_v = PQ_STANDARD_RMS_V;
    det->tolerances     = tol ? *tol : PQ_DEFAULT_TOLERANCES;
    det->write_idx      = 0;

    /* Pre-compute ideal 120V 60Hz sine wave once.
     * Python: peak * sin(2*pi*60*t)  where t = linspace(0, N/fs, N, endpoint=False)
     */
    const float peak = det->standard_rms_v * 1.41421356237f;  /* sqrt(2) */
    const float dt   = 1.0f / det->sample_rate_hz;
    const float w    = 2.0f * 3.14159265358979323846f * PQ_NOMINAL_FREQ_HZ;
    for (int i = 0; i < PQ_SAMPLES_PER_WINDOW; i++) {
        det->ideal[i] = peak * sinf(w * (float)i * dt);
    }
}

int pq_ingest(pq_detector_t *det, float voltage, uint64_t timestamp_us,
              pq_result_t *out_result)
{
    /* Capture timestamp of the first sample in this window */
    if (det->write_idx == 0) {
        det->first_sample_us = timestamp_us;
    }

    det->window[det->write_idx++] = voltage;

    if (det->write_idx < PQ_SAMPLES_PER_WINDOW) {
        return 0;  /* still accumulating */
    }

    /* ── Window complete — run analysis ──────────────────────────────── */
    pq_result_t r = {0};
    r.window_start_us    = det->first_sample_us;
    r.waveform_sad_limit = det->tolerances.waveform_sad;

    /* Level 1: RMS */
    double sum_sq = 0.0;
    for (int i = 0; i < PQ_SAMPLES_PER_WINDOW; i++) {
        sum_sq += (double)det->window[i] * (double)det->window[i];
    }
    r.measured_rms_v = (float)sqrt(sum_sq / PQ_SAMPLES_PER_WINDOW);
    r.gain           = r.measured_rms_v > 0
                         ? det->standard_rms_v / r.measured_rms_v
                         : 0.0f;
    r.rms_deviation_pct = fabsf(r.measured_rms_v - det->standard_rms_v)
                          / det->standard_rms_v * 100.0f;

    if (r.measured_rms_v < det->tolerances.outage_rms_v) {
        r.anomaly_flags |= PQ_FLAG_OUTAGE;
    } else if (r.rms_deviation_pct > det->tolerances.rms_deviation_pct) {
        if (r.measured_rms_v < det->standard_rms_v) {
            r.anomaly_flags |= PQ_FLAG_LOW_VOLTAGE;
        } else {
            r.anomaly_flags |= PQ_FLAG_VOLTAGE_SPIKE;
        }
    }

    /* Level 2: Waveform SAD (skip if outage — signal is essentially zero) */
    if (!(r.anomaly_flags & PQ_FLAG_OUTAGE)) {
        double sad = 0.0;
        for (int i = 0; i < PQ_SAMPLES_PER_WINDOW; i++) {
            float corrected = det->window[i] * r.gain;
            sad += fabs((double)corrected - (double)det->ideal[i]);
        }
        r.waveform_sad = (float)sad;
        if (r.waveform_sad > det->tolerances.waveform_sad) {
            r.anomaly_flags |= PQ_FLAG_DISTORTED_WAVEFORM;
        }
    }

    /* Reset for next window */
    det->write_idx = 0;

    *out_result = r;
    return 1;
}

const char *pq_flags_to_str(uint32_t flags, char *buf, size_t buflen)
{
    if (flags == 0) {
        snprintf(buf, buflen, "none");
        return buf;
    }
    buf[0] = '\0';
    size_t off = 0;
    #define APPEND(s) do {                                       \
        size_t n = snprintf(buf + off, buflen - off,             \
                            off ? ",%s" : "%s", s);              \
        if (n > 0 && off + n < buflen) off += n;                 \
    } while (0)
    if (flags & PQ_FLAG_OUTAGE)             APPEND("OUTAGE");
    if (flags & PQ_FLAG_LOW_VOLTAGE)        APPEND("LOW_VOLTAGE");
    if (flags & PQ_FLAG_VOLTAGE_SPIKE)      APPEND("VOLTAGE_SPIKE");
    if (flags & PQ_FLAG_DISTORTED_WAVEFORM) APPEND("DISTORTED_WAVEFORM");
    #undef APPEND
    return buf;
}
