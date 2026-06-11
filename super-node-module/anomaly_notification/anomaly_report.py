import os
import smtplib
from dataclasses import dataclass
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from typing import Optional
from datetime import datetime

try:
    from anomaly_detection.anomaly_core import AnomalyType
except ImportError:
    from enum import Enum

    class AnomalyType(Enum):
        OUTAGE = "Outage"
        FREQUENCY = "Frequency Anomaly"
        VOLTAGE = "Voltage Anomaly"
        HARMONIC = "Harmonic Anomaly"

# ── Anomaly Report ─────────────────────────────────────────────────────────────

@dataclass
class AnomalyReport:
    # Classification
    anomaly_type:       AnomalyType
    failed_test:        str              # human-readable description of what check failed

    # Identity
    sensor_id:          str
    location:           str              # placeholder: sensor location / grid zone

    # Timing
    start_time:         datetime
    end_time:           Optional[datetime]  = None
    duration_secs:      Optional[float]     = None

    # Waveform context
    typical_waveform:   Optional[str]       = None  # placeholder: description or reference
    interval_data:      Optional[str]       = None  # placeholder: measurements during anomaly
    pre_interval_data:  Optional[str]       = None  # placeholder: measurements before anomaly
    post_interval_data: Optional[str]       = None  # placeholder: measurements after anomaly

    # Anomaly-specific metrics
    measured_voltage:   Optional[float]     = None  # V RMS
    expected_voltage:   Optional[float]     = None  # V RMS
    measured_frequency: Optional[float]     = None  # Hz
    expected_frequency: float               = 60.0  # Hz
    harmonic_content:   Optional[dict]      = None  # e.g. {"3rd": 0.12, "5th": 0.04}
    thd_percent:        Optional[float]     = None  # Total Harmonic Distortion %

# ── Email Sender ───────────────────────────────────────────────────────────────

EMAIL_CONFIG = {
    "smtp_host":  os.getenv("EMAIL_SMTP_HOST", "smtp.gmail.com"),
    "smtp_port":  int(os.getenv("EMAIL_SMTP_PORT", 587)),
    "username":   os.getenv("EMAIL_USERNAME", ""),
    "password":   os.getenv("EMAIL_PASSWORD", ""),
    "from_addr":  os.getenv("EMAIL_FROM", ""),
    "to_addrs":   [addr.strip() for addr in os.getenv("EMAIL_TO", "").split(",") if addr.strip()],
}

def send_anomaly_email(report: AnomalyReport):
    """
    Builds and sends a formatted plain-text alert email from an AnomalyReport.
    Fields that are None are displayed as [PLACEHOLDER] in the email body.

    Args:
        report: AnomalyReport containing all available information about the anomaly.
    """
    # Debug: log email config
    print(f"[Email] Config loaded: host={EMAIL_CONFIG['smtp_host']}, port={EMAIL_CONFIG['smtp_port']}, from={EMAIL_CONFIG['from_addr']}, to={EMAIL_CONFIG['to_addrs']}")
    
    status  = "OUTAGE DETECTED" if report.end_time is None else "OUTAGE RESOLVED"
    subject = (
        f"[EADS ALERT] {report.anomaly_type.value} — "
        f"Sensor {report.sensor_id} — "
        f"{report.start_time.strftime('%Y-%m-%d %H:%M:%S')} UTC"
    )

    def fmt(value, placeholder="[PLACEHOLDER]", suffix=""):
        return f"{value}{suffix}" if value is not None else placeholder

    body = f"""
EADS Anomaly Notification
{'=' * 60}

CLASSIFICATION
  Type         : {report.anomaly_type.value}
  Failed Test  : {report.failed_test}

IDENTITY
  Sensor ID    : {report.sensor_id}
  Location     : {fmt(report.location)}

TIMING
  Start Time   : {report.start_time.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]} UTC
  End Time     : {fmt(report.end_time.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3] + ' UTC' if report.end_time else None)}
  Duration     : {fmt(report.duration_secs, suffix='s')}

WAVEFORM
  Typical Waveform     : {fmt(report.typical_waveform)}
  Interval Data        : {fmt(report.interval_data)}
  Pre-Interval Data    : {fmt(report.pre_interval_data)}
  Post-Interval Data   : {fmt(report.post_interval_data)}

MEASUREMENTS
  Measured Voltage     : {fmt(report.measured_voltage, suffix=' V RMS')}
  Expected Voltage     : {fmt(report.expected_voltage, suffix=' V RMS')}
  Measured Frequency   : {fmt(report.measured_frequency, suffix=' Hz')}
  Expected Frequency   : {report.expected_frequency} Hz
  Harmonic Content     : {fmt(report.harmonic_content)}
  THD                  : {fmt(report.thd_percent, suffix=' %')}

{'=' * 60}
This is an automated alert from the EADS Voltage Predictor.
"""

    msg = MIMEMultipart()
    msg["Subject"] = subject
    msg["From"]    = EMAIL_CONFIG["from_addr"]
    msg["To"]      = ", ".join(EMAIL_CONFIG["to_addrs"])
    msg.attach(MIMEText(body, "plain"))

    try:
        print(f"[Email] Attempting to connect to {EMAIL_CONFIG['smtp_host']}:{EMAIL_CONFIG['smtp_port']}")
        with smtplib.SMTP(EMAIL_CONFIG["smtp_host"], EMAIL_CONFIG["smtp_port"]) as server:
            print(f"[Email] Connected, initiating TLS...")
            server.starttls()
            print(f"[Email] TLS started, logging in as {EMAIL_CONFIG['username']}")
            server.login(EMAIL_CONFIG["username"], EMAIL_CONFIG["password"])
            print(f"[Email] Login successful, sending mail...")
            server.sendmail(EMAIL_CONFIG["from_addr"], EMAIL_CONFIG["to_addrs"], msg.as_string())
            print(f"[Email] Alert sent for {report.anomaly_type.value} on sensor {report.sensor_id}")
    except Exception as e:
        print(f"[Email] Failed to send alert: {type(e).__name__}: {e}")