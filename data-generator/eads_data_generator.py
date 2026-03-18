"""
EADS Data Generator - Sprint 2
Streams PowerGridSense dataset to AnyLog via REST PUT.

Dataset: https://www.kaggle.com/datasets/ziya07/powergridsense-dataset

Fields in dataset:
    - Timestamp: Date and time of sensor reading
    - Sensor_ID: Unique identifier for the sensor
    - Voltage (V): Voltage level in volts
    - Current (A): Current in amperes
    - Power (kW): Active power in kilowatts
    - Frequency (Hz): System frequency
    - Power_Factor: Ratio of real to apparent power
    - Location: Geographic/logical area of sensor
    - Anomaly_Label: Multi-class anomaly indicator (0-4)

Usage:
    # Stream dataset continuously (loops when reaching end)
    python eads_data_generator.py stream --csv powergridsense.csv --conn 127.0.0.1:32149

    # Send one batch of data
    python eads_data_generator.py send --csv powergridsense.csv --conn 127.0.0.1:32149 --rows 100

    # Print sample from dataset
    python eads_data_generator.py sample --csv powergridsense.csv --rows 5
"""

import argparse
import csv
import json
import sys
import time
from datetime import datetime, timezone
from typing import Iterator

import requests

# Configuration

DBMS_NAME = "eads"
TABLE_NAME = "grid_readings"

# CSV Data Loading

def load_dataset(csv_path: str) -> list[dict]:
    """Load the PowerGridSense CSV dataset into memory."""
    try:
        with open(csv_path, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            data = list(reader)
            print(f"[Dataset] Loaded {len(data)} rows from {csv_path}")
            if data:
                print(f"[Dataset] Fields: {', '.join(data[0].keys())}")
            return data
    except FileNotFoundError:
        print(f"[Error] CSV file not found: {csv_path}")
        print("\nDownload the PowerGridSense dataset from:")
        print("https://www.kaggle.com/datasets/ziya07/powergridsense-dataset")
        sys.exit(1)
    except Exception as e:
        print(f"[Error] Failed to load CSV: {e}")
        sys.exit(1)


def dataset_iterator(data: list[dict]) -> Iterator[dict]:
    """Infinite iterator that loops through the dataset."""
    if not data:
        raise ValueError("Dataset is empty")

    while True:
        for row in data:
            yield row


def format_reading(row: dict, use_real_time: bool = True,
                   timestamp_override: str = None) -> dict:
    """
    Convert CSV row to JSON format for AnyLog.
    Preserves all fields from the dataset.

    Args:
        row: CSV row dict
        use_real_time: If True, use current timestamp instead of dataset timestamp
        timestamp_override: Optional timestamp string to use (overrides use_real_time)
    """
    # Determine timestamp to use
    if timestamp_override:
        timestamp = timestamp_override
    elif use_real_time:
        # Use current time in ISO format
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    else:
        # Use timestamp from dataset
        timestamp = row.get("Timestamp", "")

    return {
        "timestamp": timestamp,
        "sensor_id": row.get("Sensor_ID", ""),
        "voltage": row.get("Voltage (V)", ""),
        "current": row.get("Current (A)", ""),
        "power": row.get("Power (kW)", ""),
        "frequency": row.get("Frequency (Hz)", ""),
        "power_factor": row.get("Power_Factor", ""),
        "location": row.get("Location", ""),
        "anomaly_label": row.get("Anomaly_Label", ""),
    }

# REST PUT - Send data to AnyLog

def rest_put(conn: str, auth: tuple, payload: list[dict],
             mode: str = "file") -> bool:
    """
    Push data to AnyLog via REST PUT.

    Args:
        conn: AnyLog endpoint (host:port)
        auth: Optional (user, password) tuple
        payload: List of readings to send
        mode: "file" for immediate write, "streaming" for buffered

    Returns:
        True if successful, False otherwise
    """
    headers = {
        "type": "json",
        "dbms": DBMS_NAME,
        "table": TABLE_NAME,
        "mode": mode,
        "Content-Type": "text/plain",
    }
    url = f"http://{conn}"

    try:
        r = requests.put(url, auth=auth or None, timeout=30,
                        headers=headers, data=json.dumps(payload))
    except requests.RequestException as e:
        print(f"[REST PUT] Failed to send to {conn}: {e}")
        return False

    if r.status_code != 200:
        print(f"[REST PUT] Server returned {r.status_code}: {r.text}")
        return False

    return True

# Streaming Modes

def stream_continuous(csv_path: str, conn: str, auth: tuple,
                     batch_size: int = 100, rate_hz: float = 10.0,
                     mode: str = "file", use_real_time: bool = True):
    """
    Continuously stream dataset in batches.
    Loops back to start when reaching end of dataset.

    Args:
        csv_path: Path to PowerGridSense CSV file
        conn: AnyLog endpoint
        auth: Optional authentication
        batch_size: Number of rows per batch
        rate_hz: Target batches per second (not rows per second)
        mode: AnyLog ingestion mode ("file" or "streaming")
        use_real_time: Use current timestamps instead of dataset timestamps
    """
    data = load_dataset(csv_path)
    iterator = dataset_iterator(data)

    batch_interval = 1.0 / rate_hz  # seconds between batches
    rows_per_sec = batch_size * rate_hz

    print(f"\n[Stream] Configuration:")
    print(f"  Dataset: {csv_path} ({len(data)} rows)")
    print(f"  Batch size: {batch_size} rows")
    print(f"  Batch rate: {rate_hz} batches/sec")
    print(f"  Effective: ~{rows_per_sec:.1f} rows/sec")
    print(f"  Mode: {mode}")
    print(f"  Timestamps: {'Real-time (current)' if use_real_time else 'Dataset (historical)'}")
    print(f"  Target: {conn}")
    print(f"\n[Stream] Press Ctrl+C to stop\n")

    total_sent = 0
    batches_sent = 0
    start = time.monotonic()

    try:
        while True:
            t0 = time.monotonic()

            # Collect batch
            batch = []
            for _ in range(batch_size):
                row = next(iterator)
                batch.append(format_reading(row, use_real_time=use_real_time))

            # Send to AnyLog
            ok = rest_put(conn, auth, batch, mode)

            if ok:
                total_sent += len(batch)
                batches_sent += 1
                if batches_sent % 10 == 0:  # Progress every 10 batches
                    elapsed = time.monotonic() - start
                    actual_rate = total_sent / elapsed if elapsed > 0 else 0
                    print(f"[Stream] Sent {total_sent:,} rows in {batches_sent} batches "
                          f"({actual_rate:.1f} rows/sec)")

            # Rate limiting
            elapsed = time.monotonic() - t0
            sleep_time = batch_interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        duration = time.monotonic() - start
        actual_rate = total_sent / duration if duration > 0 else 0
        print(f"\n[Stream] Stopped after {duration:.1f}s")
        print(f"[Stream] Sent {total_sent:,} rows in {batches_sent} batches")
        print(f"[Stream] Effective rate: {actual_rate:.1f} rows/sec")


def send_batch(csv_path: str, conn: str, auth: tuple,
               rows: int = 100, mode: str = "file", use_real_time: bool = True):
    """
    Send a single batch of rows from the dataset.

    Args:
        csv_path: Path to PowerGridSense CSV file
        conn: AnyLog endpoint
        auth: Optional authentication
        rows: Number of rows to send
        mode: AnyLog ingestion mode
        use_real_time: Use current timestamps instead of dataset timestamps
    """
    data = load_dataset(csv_path)

    # Take first N rows (or all if fewer than N)
    batch_data = data[:rows]
    batch = [format_reading(row, use_real_time=use_real_time) for row in batch_data]

    print(f"\n[Send] Sending {len(batch)} rows to {conn}...")
    print(f"[Send] Timestamps: {'Real-time (current)' if use_real_time else 'Dataset (historical)'}")
    ok = rest_put(conn, auth, batch, mode)

    if ok:
        print(f"[Send] Success — sent {len(batch)} rows")
    else:
        print(f"[Send] Failed to send data")
        sys.exit(1)


def print_sample(csv_path: str, rows: int = 5, use_real_time: bool = True):
    """Print sample rows from the dataset as JSON."""
    data = load_dataset(csv_path)
    sample_data = data[:rows]
    sample = [format_reading(row, use_real_time=use_real_time) for row in sample_data]
    print(f"# Timestamps: {'Real-time (current)' if use_real_time else 'Dataset (historical)'}")
    print(json.dumps(sample, indent=2))


# CLI

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
        description="EADS Data Generator — Stream PowerGridSense dataset to AnyLog"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # stream
    p_stream = sub.add_parser("stream",
                             help="Stream dataset continuously (loops at end)")
    p_stream.add_argument("--csv", required=True,
                         help="Path to PowerGridSense CSV file")
    p_stream.add_argument("--conn", default="127.0.0.1:32149",
                         help="AnyLog REST endpoint (host:port or user:pass@host:port)")
    p_stream.add_argument("--batch-size", type=int, default=100,
                         help="Number of rows per batch (default: 100)")
    p_stream.add_argument("--rate", type=float, default=10.0,
                         help="Batches per second (default: 10)")
    p_stream.add_argument("--mode", choices=["streaming", "file"], default="file",
                         help="AnyLog ingestion mode (default: file)")
    p_stream.add_argument("--use-dataset-time", action="store_true",
                         help="Use timestamps from dataset instead of current time (default: use current time)")

    # send
    p_send = sub.add_parser("send",
                           help="Send a single batch of rows")
    p_send.add_argument("--csv", required=True,
                       help="Path to PowerGridSense CSV file")
    p_send.add_argument("--conn", default="127.0.0.1:32149",
                       help="AnyLog REST endpoint (host:port or user:pass@host:port)")
    p_send.add_argument("--rows", type=int, default=100,
                       help="Number of rows to send (default: 100)")
    p_send.add_argument("--mode", choices=["streaming", "file"], default="file",
                       help="AnyLog ingestion mode (default: file)")
    p_send.add_argument("--use-dataset-time", action="store_true",
                       help="Use timestamps from dataset instead of current time (default: use current time)")

    # sample
    p_sample = sub.add_parser("sample",
                             help="Print sample rows as JSON")
    p_sample.add_argument("--csv", required=True,
                         help="Path to PowerGridSense CSV file")
    p_sample.add_argument("--rows", type=int, default=5,
                         help="Number of rows to print (default: 5)")
    p_sample.add_argument("--use-dataset-time", action="store_true",
                         help="Use timestamps from dataset instead of current time (default: use current time)")

    args = parser.parse_args()

    if args.command == "stream":
        conn, auth = parse_conn(args.conn)
        use_real_time = not args.use_dataset_time  # Invert the flag
        stream_continuous(args.csv, conn, auth, args.batch_size,
                        args.rate, args.mode, use_real_time)

    elif args.command == "send":
        conn, auth = parse_conn(args.conn)
        use_real_time = not args.use_dataset_time  # Invert the flag
        send_batch(args.csv, conn, auth, args.rows, args.mode, use_real_time)

    elif args.command == "sample":
        use_real_time = not args.use_dataset_time  # Invert the flag
        print_sample(args.csv, args.rows, use_real_time)


if __name__ == "__main__":
    main()
