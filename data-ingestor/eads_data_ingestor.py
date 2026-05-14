"""
EADS Data Ingestor
Reads live ADC samples from the hardware sampling socket, applies calibration,
and streams to AnyLog via REST PUT.

Expects data from the Unix socket in format (no header):
    timestamp_us, channel, raw_count
Only channel 1 samples are used.

Usage:
    python eads_data_ingestor.py --conn 127.0.0.1:32149
    python eads_data_ingestor.py --socket /var/sampling/samples.sock --conn 127.0.0.1:32149
"""

import argparse
import json
import socket
import sys
import time
from datetime import datetime, timezone

import requests

# AnyLog configuration
DBMS_NAME = "eads"
TABLE_NAME = "voltage_calibrated"

# ADC constants
ADC_REF_VOLTS = 3.3
ADC_COUNTS = 4096.0

# Fixed calibration constants derived from reference capture
ADC_MID_VOLTS = 1.692297
ADC_SCALE = 281.793554

DEFAULT_SOCKET = "/var/sampling/samples.sock"


def calibrate_sample(raw_count: int) -> tuple[float, float, float]:
    """Apply fixed calibration constants to a single raw ADC count."""
    adc_input = raw_count * ADC_REF_VOLTS / ADC_COUNTS
    adc_centered = adc_input - ADC_MID_VOLTS
    voltage_est = adc_centered * ADC_SCALE
    return adc_input, adc_centered, voltage_est


def format_reading(timestamp_us: str, raw_count: int) -> dict:
    """Convert a raw sample to AnyLog JSON payload."""
    adc_input, adc_centered, voltage_est = calibrate_sample(raw_count)
    return {
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
        "timestamp_us": timestamp_us,
        "raw_count": str(raw_count),
        "adc_input_volts": f"{adc_input:.6f}",
        "adc_centered_volts": f"{adc_centered:.6f}",
        "voltage_est": f"{voltage_est:.6f}",
    }


def rest_put(conn: str, auth: tuple, payload: list[dict], mode: str = "file") -> bool:
    """Push a batch of readings to AnyLog via REST PUT."""
    headers = {
        "type": "json",
        "dbms": DBMS_NAME,
        "table": TABLE_NAME,
        "mode": mode,
        "Content-Type": "text/plain",
    }
    try:
        r = requests.put(f"http://{conn}", auth=auth or None, timeout=30,
                         headers=headers, data=json.dumps(payload))
    except requests.RequestException as e:
        print(f"[REST PUT] Failed: {e}")
        return False
    if r.status_code != 200:
        print(f"[REST PUT] Server returned {r.status_code}: {r.text}")
        return False
    return True


def socket_lines(socket_path: str):
    """Generator that yields lines from the Unix domain socket."""
    print(f"[Socket] Connecting to {socket_path}...")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(socket_path)
            print("[Socket] Connected")
            buf = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    print("[Socket] Connection closed by hardware")
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    yield line.decode("utf-8", errors="ignore").strip()
    except FileNotFoundError:
        print(f"[Error] Socket not found: {socket_path}")
        print("Ensure the hardware sampling process is running.")
        sys.exit(1)
    except ConnectionRefusedError:
        print(f"[Error] Connection refused on {socket_path}")
        print("Ensure the hardware sampling process is running.")
        sys.exit(1)


def ingest(socket_path: str, conn: str, auth: tuple,
           batch_size: int = 100, mode: str = "file"):
    """Read live samples from the hardware socket and stream to AnyLog."""
    print(f"\n[Ingestor] Configuration:")
    print(f"  Socket:     {socket_path}")
    print(f"  Batch size: {batch_size} samples")
    print(f"  Mode:       {mode}")
    print(f"  Target:     {conn}")
    print(f"\n[Ingestor] Press Ctrl+C to stop\n")

    total_sent = 0
    batches_sent = 0
    start = time.monotonic()
    batch = []

    try:
        for line in socket_lines(socket_path):
            parts = line.split(",")
            if len(parts) != 3:
                continue
            try:
                t, ch, raw = parts[0], int(parts[1]), int(parts[2])
            except ValueError:
                continue
            if ch != 1:
                continue

            batch.append(format_reading(t, raw))

            if len(batch) >= batch_size:
                if rest_put(conn, auth, batch, mode):
                    total_sent += len(batch)
                    batches_sent += 1
                    if batches_sent % 10 == 0:
                        elapsed = time.monotonic() - start
                        rate = total_sent / elapsed if elapsed > 0 else 0
                        print(f"[Ingestor] Sent {total_sent:,} rows in {batches_sent} batches "
                              f"({rate:.1f} rows/sec)")
                batch = []

    except KeyboardInterrupt:
        duration = time.monotonic() - start
        rate = total_sent / duration if duration > 0 else 0
        print(f"\n[Ingestor] Stopped after {duration:.1f}s")
        print(f"[Ingestor] Sent {total_sent:,} rows in {batches_sent} batches")
        print(f"[Ingestor] Effective rate: {rate:.1f} rows/sec")


def parse_conn(value: str) -> tuple[str, tuple]:
    """Parse 'user:pass@host:port' or 'host:port' into (conn, auth)."""
    if "@" in value:
        creds, conn = value.rsplit("@", 1)
        parts = creds.split(":", 1)
        auth = (parts[0], parts[1] if len(parts) > 1 else "")
    else:
        conn = value
        auth = ()
    return conn, auth


def main():
    parser = argparse.ArgumentParser(
        description="EADS Data Ingestor — Stream live ADC data to AnyLog"
    )
    parser.add_argument("--socket", default=DEFAULT_SOCKET,
                        help=f"Unix socket path (default: {DEFAULT_SOCKET})")
    parser.add_argument("--conn", default="127.0.0.1:32149",
                        help="AnyLog REST endpoint (default: 127.0.0.1:32149)")
    parser.add_argument("--batch-size", type=int, default=100,
                        help="Samples per batch (default: 100)")
    parser.add_argument("--mode", choices=["streaming", "file"], default="file",
                        help="AnyLog ingestion mode (default: file)")

    args = parser.parse_args()
    conn, auth = parse_conn(args.conn)
    ingest(args.socket, conn, auth, args.batch_size, args.mode)


if __name__ == "__main__":
    main()
