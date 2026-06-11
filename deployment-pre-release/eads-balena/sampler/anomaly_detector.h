/*
 * anomaly_detector.h — EADS power quality anomaly detection
 *
 * C port of anomaly_detection/cycle_detector.py
 * Detects anomalies in 128-sample windows (one AC cycle at 60Hz/7680Hz):
 *   Level 1: RMS deviation from 120V standard  → OUTAGE, LOW_VOLTAGE, VOLTAGE_SPIKE
 *   Level 2: SAD vs ideal sine wave (after gain) → DISTORTED_WAVEFORM
 */

#ifndef ANOMALY_DETECTOR_H
#define ANOMALY_DETECTOR_H

#include <stdint.h>
#include <stddef.h>

#define PQ_SAMPLES_PER_WINDOW  128
#define PQ_STANDARD_RMS_V      120.0f
#define PQ_NOMINAL_FREQ_HZ     60.0f
#define PQ_DEFAULT_SAMPLE_RATE 7680.0f

/* Anomaly flags (bitmask — multiple can be set on one result) */
#define PQ_FLAG_OUTAGE             (1u << 0)
#define PQ_FLAG_LOW_VOLTAGE        (1u << 1)
#define PQ_FLAG_VOLTAGE_SPIKE      (1u << 2)
#define PQ_FLAG_DISTORTED_WAVEFORM (1u << 3)

typedef struct {
    float rms_deviation_pct;   /* % from 120V before flagging       (default 5.0) */
    float outage_rms_v;        /* below this RMS = outage           (default 10.0)*/
    float waveform_sad;        /* SAD threshold for distortion      (default 50.0)*/
} pq_tolerances_t;

typedef struct {
    uint64_t window_start_us;  /* timestamp of first sample in window */
    float    measured_rms_v;
    float    gain;             /* 120V / measured_rms                  */
    float    rms_deviation_pct;
    float    waveform_sad;
    float    waveform_sad_limit;
    uint32_t anomaly_flags;    /* PQ_FLAG_* bitmask, 0 = no anomaly    */
} pq_result_t;

typedef struct {
    float           sample_rate_hz;
    float           standard_rms_v;
    pq_tolerances_t tolerances;

    /* Sliding window buffer */
    float           window[PQ_SAMPLES_PER_WINDOW];
    uint64_t        first_sample_us;
    uint16_t        write_idx;

    /* Pre-computed ideal sine wave for SAD comparison */
    float           ideal[PQ_SAMPLES_PER_WINDOW];
} pq_detector_t;

/* Initialize detector with given sample rate and tolerances (NULL = defaults) */
void pq_init(pq_detector_t *det,
             float sample_rate_hz,
             const pq_tolerances_t *tol);

/* Feed one sample. Returns 1 if a window completed and out_result is filled,
 * 0 if still accumulating. */
int  pq_ingest(pq_detector_t *det,
               float voltage,
               uint64_t timestamp_us,
               pq_result_t *out_result);

/* Convert flag bitmask to human-readable string (comma-separated) */
const char *pq_flags_to_str(uint32_t flags, char *buf, size_t buflen);

#endif
