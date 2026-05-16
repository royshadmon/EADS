# Anomaly Detection

Anomaly detection for the EADS grid monitoring system. Analyzes voltage readings in 128-sample windows and detects anomalies using RMS comparison and waveform shape analysis.

---

## Package Structure

The `anomaly_detection` folder contains an `__init__.py` that re-exports the public API, so the detector can be imported directly from the package:

```python
from anomaly_detection import process_reading, PowerQualityTolerances
```

---

## cycle_detector.py

The core detection module. Exposes a single entry point — `process_reading` — that accepts one voltage sample at a time and handles everything else internally.

### How it works

Samples are accumulated in a per-sensor buffer. Once 128 samples are collected (one window, ~16.7ms at 7680 Hz), two checks run automatically:

**Level 1 — RMS Comparison**
Computes the RMS of the 128-sample window and compares it to the 120V standard. From this it derives a gain factor — the multiplier needed to bring the measured RMS exactly to 120V. Detects outages, brownouts, and overvoltage purely from amplitude.

**Level 2 — Waveform Shape**
Multiplies every sample by the Level 1 gain to normalize amplitude, then compares the result point-by-point against a mathematically perfect 120V 60Hz sine wave. The sum of absolute differences (SAD) measures how distorted the waveform shape is, independent of amplitude. Detects clipping, notching, and nonlinear load distortion.

Both levels run on every window. A result is returned whenever anomalies are detected; if no anomalies are present, `process_reading` returns `None`.

---

### Classes

#### `PowerQualityTolerances`
Configurable thresholds for both detection levels.

| Field | Default | Description |
|---|---|---|
| `rms_deviation_pct` | `5.0` | % deviation from 120V RMS before flagging low/high voltage |
| `outage_rms_v` | `10.0` | RMS below this value is classified as an outage |
| `waveform_sad` | `50.0` | SAD threshold above which waveform is flagged as distorted |

Tolerances can be updated at runtime without restarting the detector:
```python
detector.update_tolerances(rms_deviation_pct=8.0, waveform_sad=75.0)
```

#### `PowerQualityResult`
Returned by the detector for every completed window that contains anomalies. Contains all measurements from both levels.

| Field | Description |
|---|---|
| `sensor_id` | Sensor that produced this window |
| `window_start` | Timestamp of the first sample in the window |
| `measured_rms_v` | Measured RMS voltage in volts |
| `standard_rms_v` | Reference standard (120V) |
| `gain` | Scale factor applied to normalize amplitude |
| `rms_deviation_pct` | % deviation from standard RMS |
| `waveform_sad` | Sum of absolute differences vs ideal sine wave |
| `waveform_sad_limit` | SAD threshold used for this window |
| `anomaly_types` | List of `AnomalyType` values that failed |
| `failed_tests` | Human-readable description of each failure |

#### `PowerQualityDetector`
One instance per sensor. Manages the sample buffer and runs analysis. Created automatically by `process_reading` on first sight of a sensor ID — you do not need to instantiate this directly.

---

### Entry Point

```python
process_reading(
    sensor_id:      str,
    voltage:        float,
    timestamp:      datetime,
    tolerances:     Optional[PowerQualityTolerances] = None,
    sample_rate_hz: float = 7680.0,
)
```

Call this once per voltage sample per sensor. It manages detector lifecycle, accumulates samples, runs analysis when a window is complete, and returns a `PowerQualityResult` if any anomalies are detected, or `None` otherwise.

**Example:**
```python
from anomaly_detection import process_reading, PowerQualityTolerances
from datetime import datetime, timezone

tolerances = PowerQualityTolerances(
    rms_deviation_pct = 5.0,
    outage_rms_v      = 10.0,
    waveform_sad      = 50.0,
)

process_reading(
    sensor_id      = "sensor_01",
    voltage        = 168.3,
    timestamp      = datetime.now(timezone.utc),
    tolerances     = tolerances,
    sample_rate_hz = 7680.0,
)
```

---

### Anomaly Types Detected

| Type | Level | Cause |
|---|---|---|
| `OUTAGE` | 1 | RMS below `outage_rms_v` |
| `LOW_VOLTAGE` | 1 | RMS more than `rms_deviation_pct`% below 120V |
| `VOLTAGE_SPIKE` | 1 | RMS more than `rms_deviation_pct`% above 120V |
| `DISTORTED_WAVEFORM` | 2 | SAD exceeds `waveform_sad` threshold |

---

### Constants

| Name | Value | Description |
|---|---|---|
| `STANDARD_RMS_V` | `120.0` | US standard grid voltage in RMS volts |
| `NOMINAL_FREQ_HZ` | `60.0` | US standard grid frequency in Hz |

---

### Dependencies

- `numpy`
- `anomaly_notification.anomaly_report` — for `AnomalyType`, `AnomalyReport`, and `send_anomaly_email`

---

### Testing

```bash
PYTHONPATH=. python anomaly_detection/test_detection.py
```

Expected output:
- Clean signal → no alerts
- Mild noise → no alerts
- Brownout → `Low Voltage` alert
- Overvoltage → `Voltage Spike` alert
- Clipped waveform → `Distorted Waveform` alert
- Outage → `Outage` alert