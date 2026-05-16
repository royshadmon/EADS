import numpy as np
from dataclasses import dataclass
from datetime import datetime
from typing import Optional
from anomaly_notification.anomaly_report import AnomalyType

# ── Sentinel ───────────────────────────────────────────────────────────────────
# Used to check whether 128 samples have been collected
_WINDOW_INCOMPLETE = object()

# ── Constants ──────────────────────────────────────────────────────────────────
# Nominal US Voltage and Frequency
STANDARD_RMS_V  = 120.0
NOMINAL_FREQ_HZ = 60.0

# ── Tolerances ─────────────────────────────────────────────────────────────────

@dataclass
class PowerQualityTolerances:
    rms_deviation_pct: float = 5.0    # % from 120V RMS before flagging
    outage_rms_v:      float = 10.0   # below this RMS = outage
    waveform_sad:      float = 50.0   # sum of absolute differences vs ideal sine

# ── Result ─────────────────────────────────────────────────────────────────────

@dataclass
class PowerQualityResult:
    sensor_id:         str
    window_start:      datetime

    # Level 1 - RMS Deviation (outages, brownouts, spikes)
    measured_rms_v:    float
    standard_rms_v:    float
    gain:              float
    rms_deviation_pct: float

    # Level 2 - Waveform Shape (SAD after gain correction)
    waveform_sad:      float
    waveform_sad_limit:float

    # Anomalies Detected and Tests failed
    anomaly_types:     list[AnomalyType] = None
    failed_tests:      list[str]         = None
    def __post_init__(self):
        if self.anomaly_types is None:
            self.anomaly_types = []
        if self.failed_tests is None:
            self.failed_tests = []

# ── Detector ───────────────────────────────────────────────────────────────────

class PowerQualityDetector:
    """
    Detects power quality anomalies using two checks per 128-sample window:
      Level 1 — RMS vs 120V standard  (outages, brownouts, spikes)
      Level 2 — Waveform shape via SAD (distortion, clipping, nonlinear loads)
    """

    def __init__(
        self,
        sensor_id:          str,
        sample_rate_hz:     float = 7680.0,
        standard_rms_v:     float = STANDARD_RMS_V,
        samples_per_window: int   = 128,
        tolerances:         Optional[PowerQualityTolerances] = None,
    ):
        self.sensor_id          = sensor_id
        self.sample_rate_hz     = sample_rate_hz
        self.standard_rms_v     = standard_rms_v
        self.samples_per_window = samples_per_window
        self.tolerances         = tolerances or PowerQualityTolerances()

        self._buffer:     list[float]    = []
        self._timestamps: list[datetime] = []
        self._ideal = self._generate_ideal()

    def _generate_ideal(self) -> np.ndarray:
        """ Generates a mathematically perfect 60Hz Sine Wave """
        t    = np.linspace(0, self.samples_per_window / self.sample_rate_hz,
                           self.samples_per_window, endpoint=False)
        peak = self.standard_rms_v * np.sqrt(2)
        return peak * np.sin(2 * np.pi * NOMINAL_FREQ_HZ * t)

    def update_tolerances(self, **kwargs):
        """ Updates the tolerances """
        for key, value in kwargs.items():
            if hasattr(self.tolerances, key):
                setattr(self.tolerances, key, value)
            else:
                raise ValueError(f"Unknown tolerance field: '{key}'")

    def ingest(self, voltage: float, timestamp: datetime):
        """ Fires the analysis once there are 128 samples in the buffer """
        self._buffer.append(voltage)
        self._timestamps.append(timestamp)

        if len(self._buffer) < self.samples_per_window:
            return _WINDOW_INCOMPLETE

        window    = np.array(self._buffer[:self.samples_per_window], dtype=float)
        win_start = self._timestamps[0]

        self._buffer     = self._buffer[self.samples_per_window:]
        self._timestamps = self._timestamps[self.samples_per_window:]

        return self._analyze(window, win_start)

    def _analyze(self, window: np.ndarray, win_start: datetime) -> PowerQualityResult:
        """ Performs the anomaly detection"""
        t             = self.tolerances
        anomaly_types = []
        failed_tests  = []

        # ── Level 1: RMS ───────────────────────────────────────────────────────
        measured_rms = float(np.sqrt(np.mean(window ** 2)))
        gain         = self.standard_rms_v / measured_rms if measured_rms > 0 else 0.0
        rms_dev_pct  = abs(measured_rms - self.standard_rms_v) / self.standard_rms_v * 100

        if measured_rms < t.outage_rms_v:
            anomaly_types.append(AnomalyType.OUTAGE)
            failed_tests.append(
                f"RMS={measured_rms:.2f}V below outage threshold {t.outage_rms_v}V"
            )
        elif rms_dev_pct > t.rms_deviation_pct:
            if measured_rms < self.standard_rms_v:
                anomaly_types.append(AnomalyType.LOW_VOLTAGE)
            else:
                anomaly_types.append(AnomalyType.VOLTAGE_SPIKE)
            failed_tests.append(
                f"RMS={measured_rms:.2f}V deviation={rms_dev_pct:.1f}% "
                f"(limit {t.rms_deviation_pct}%)"
            )

        # ── Level 2: Waveform shape ────────────────────────────────────────────
        if AnomalyType.OUTAGE not in anomaly_types:
            corrected = window * gain
            sad = float(np.sum(np.abs(corrected - self._ideal)))
            if sad > t.waveform_sad:
                anomaly_types.append(AnomalyType.DISTORTED_WAVEFORM)
                failed_tests.append(
                    f"Waveform SAD={sad:.2f} (limit {t.waveform_sad})"
                )
        else:
            sad = 0.0

        return PowerQualityResult(
            sensor_id          = self.sensor_id,
            window_start       = win_start,
            measured_rms_v     = measured_rms,
            standard_rms_v     = self.standard_rms_v,
            gain               = gain,
            rms_deviation_pct  = rms_dev_pct,
            waveform_sad       = sad,
            waveform_sad_limit = t.waveform_sad,
            anomaly_types      = anomaly_types,
            failed_tests       = failed_tests,
        )

# ── Stream Ingestion Entry Point ───────────────────────────────────────────────

_detectors: dict[str, PowerQualityDetector] = {}

def process_reading(
    sensor_id:      str,
    voltage:        float,
    timestamp:      datetime,
    tolerances:     Optional[PowerQualityTolerances] = None,
    sample_rate_hz: float = 7680.0,
):
    if sensor_id not in _detectors:
        _detectors[sensor_id] = PowerQualityDetector(
            sensor_id      = sensor_id,
            tolerances     = tolerances,
            sample_rate_hz = sample_rate_hz,
        )

    result = _detectors[sensor_id].ingest(voltage, timestamp)
    if result is _WINDOW_INCOMPLETE:
        return None
    return result if result.anomaly_types else None