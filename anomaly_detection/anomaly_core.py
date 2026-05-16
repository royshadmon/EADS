from enum import Enum

# ── Anomaly Types ──────────────────────────────────────────────────────────────
# Shared vocabulary for all detectors and the notification system.
# Both anomaly_detection and anomaly_notification import from here — neither
# depends on the other.

class AnomalyType(Enum):
    OUTAGE              = "Outage"
    LOW_VOLTAGE         = "Low Voltage"
    VOLTAGE_SPIKE       = "Voltage Spike"
    DISTORTED_WAVEFORM  = "Distorted Waveform / Deviation from Ideal"
    FREQUENCY_DEVIATION = "Frequency Deviation from 60 Hz"
    HIGH_HARMONIC       = "High Harmonic Content"