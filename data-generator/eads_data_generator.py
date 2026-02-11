"""
EADS Data Generator
Generates mock electrical sensor data and pushes it to AnyLog via REST (PUT/POST) or serves it via gRPC.

Voltage simulation:
    - 60 Hz AC sine wave: V(t) = V_peak * sin(2π * 60 * t)
    - V_peak = V_rms * √2 (e.g. 120V RMS → ~169.7V peak)
    - Small Gaussian noise added for realism

Data format:
    - timestamp: ISO 8601 UTC
    - voltage: float (V) — instantaneous AC voltage
    - sensor_id: string identifier
    - latitude / longitude: geo-coordinates

Usage:
    # REST PUT (direct to AnyLog operator)
    python eads_data_generator.py rest-put --conn 127.0.0.1:32149 --hz 1000

    # REST POST (topic-based, requires MQTT client on AnyLog node)
    python eads_data_generator.py rest-post --conn 127.0.0.1:32149 --topic eads-sensors --hz 1000

    # gRPC server (AnyLog connects to this as a client)
    python eads_data_generator.py grpc-serve --port 50051 --hz 1000

    # Print sample data without sending
    python eads_data_generator.py sample --rows 5
"""

import argparse
import datetime
import json
import math
import random
import sys
import time
from concurrent import futures

import requests

# gRPC imports - deferred to avoid hard failure if only using REST
try:
    import grpc
    from grpc_tools import protoc
    HAS_GRPC = True
except ImportError:
    HAS_GRPC = False


# ---------------------------------------------------------------------------
# Sensor configuration
# ---------------------------------------------------------------------------

AC_FREQ_HZ = 60  # US mains frequency

SENSORS = [
    {"sensor_id": "EADS-V-001", "lat": 32.7157, "lon": -117.1611, "rms_voltage": 120.0},
    {"sensor_id": "EADS-V-002", "lat": 32.7300, "lon": -117.1500, "rms_voltage": 240.0},
    {"sensor_id": "EADS-V-003", "lat": 32.7050, "lon": -117.1700, "rms_voltage": 480.0},
    {"sensor_id": "EADS-V-004", "lat": 32.7400, "lon": -117.1400, "rms_voltage": 120.0},
    {"sensor_id": "EADS-V-005", "lat": 32.7200, "lon": -117.1800, "rms_voltage": 240.0},
]

DBMS_NAME = "eads"
TABLE_NAME = "voltage_readings"


# ---------------------------------------------------------------------------
# Mock data generation — 60 Hz AC sine wave
# ---------------------------------------------------------------------------

def ac_voltage(sensor: dict, t_seconds: float) -> float:
    """Instantaneous AC voltage: V_peak * sin(2π * 60 * t) + noise."""
    v_peak = sensor["rms_voltage"] * math.sqrt(2)
    v = v_peak * math.sin(2 * math.pi * AC_FREQ_HZ * t_seconds)
    noise = random.gauss(0, sensor["rms_voltage"] * 0.005)  # ±0.5% noise
    return round(v + noise, 4)


def generate_reading(sensor: dict, ts: datetime.datetime, t_seconds: float) -> dict:
    """Generate a single sensor reading at a point on the AC sine wave."""
    return {
        "timestamp": ts.strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        "voltage": ac_voltage(sensor, t_seconds),
        "sensor_id": sensor["sensor_id"],
        "latitude": sensor["lat"],
        "longitude": sensor["lon"],
    }


def generate_batch(rows: int, sample_rate_hz: int = 1000) -> list[dict]:
    """Generate a batch of sensor readings at the given sample rate."""
    readings = []
    ts = datetime.datetime.now(datetime.timezone.utc)
    dt = datetime.timedelta(seconds=1.0 / sample_rate_hz)
    t_seconds = time.time()  # continuous time reference for sine wave
    for i in range(rows):
        sensor = SENSORS[i % len(SENSORS)]
        readings.append(generate_reading(sensor, ts, t_seconds))
        ts += dt
        t_seconds += 1.0 / sample_rate_hz
    return readings


# ---------------------------------------------------------------------------
# REST PUT - direct attribute-to-column mapping
# ---------------------------------------------------------------------------

def rest_put(conn: str, auth: tuple, payload: list[dict],
             mode: str = "streaming") -> bool:
    """
    Push data to AnyLog via REST PUT.
    Headers carry dbms/table; JSON body carries the data.
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

    print(f"[REST PUT] Success — sent {len(payload)} readings to {conn}")
    return True


# ---------------------------------------------------------------------------
# REST POST - topic-based mapping (requires MQTT client on AnyLog node)
# ---------------------------------------------------------------------------

def rest_post(conn: str, auth: tuple, topic: str,
              payload: list[dict]) -> bool:
    """
    Push data to AnyLog via REST POST.
    Requires an MQTT client with broker=rest configured on the receiving node.
    The payload includes dbms/table so the MQTT mapping can extract them.
    """
    # Embed dbms and table into each row for topic-based extraction
    enriched = []
    for row in payload:
        enriched.append({**row, "dbms": DBMS_NAME, "table": TABLE_NAME})

    headers = {
        "command": "data",
        "topic": topic,
        "User-Agent": "AnyLog/1.23",
        "Content-Type": "text/plain",
    }
    url = f"http://{conn}"
    try:
        r = requests.post(url, auth=auth or None, timeout=30,
                          headers=headers, data=json.dumps(enriched))
    except requests.RequestException as e:
        print(f"[REST POST] Failed to send to {conn}: {e}")
        return False

    if r.status_code != 200:
        print(f"[REST POST] Server returned {r.status_code}: {r.text}")
        return False

    print(f"[REST POST] Success — sent {len(payload)} readings to topic '{topic}' on {conn}")
    return True


# ---------------------------------------------------------------------------
# gRPC server - AnyLog connects here as a client
# ---------------------------------------------------------------------------

def _ensure_grpc_stubs():
    """Compile proto and import generated modules at runtime."""
    if not HAS_GRPC:
        print("grpcio / grpcio-tools not installed. "
              "Install with: pip install grpcio grpcio-tools")
        sys.exit(1)

    import importlib
    import os
    proto_dir = os.path.dirname(os.path.abspath(__file__))
    proto_file = os.path.join(proto_dir, "sensor_data.proto")
    pb2_path = os.path.join(proto_dir, "sensor_data_pb2.py")

    # Compile proto if stubs don't exist yet
    if not os.path.exists(pb2_path):
        print("[gRPC] Compiling sensor_data.proto ...")
        result = protoc.main([
            "grpc_tools.protoc",
            f"-I{proto_dir}",
            f"--python_out={proto_dir}",
            f"--grpc_python_out={proto_dir}",
            proto_file,
        ])
        if result != 0:
            print("[gRPC] Proto compilation failed")
            sys.exit(1)

    # Import the generated modules
    spec_pb2 = importlib.util.spec_from_file_location(
        "sensor_data_pb2", os.path.join(proto_dir, "sensor_data_pb2.py"))
    pb2 = importlib.util.module_from_spec(spec_pb2)
    spec_pb2.loader.exec_module(pb2)

    spec_grpc = importlib.util.spec_from_file_location(
        "sensor_data_pb2_grpc",
        os.path.join(proto_dir, "sensor_data_pb2_grpc.py"))
    pb2_grpc = importlib.util.module_from_spec(spec_grpc)
    spec_grpc.loader.exec_module(pb2_grpc)

    return pb2, pb2_grpc


def run_grpc_server(port: int, rows: int, sample_rate_hz: int):
    """
    Start a gRPC server that serves sensor data.

    AnyLog connects to this server using:
        run grpc client where name = eads_sensors and ip = <this_host>
            and port = <port> and grpc_dir = <dir_with_proto>
            and proto = sensor_data and function = GetSensorData
            and request = SensorRequest and response = SensorDataResponse
            and service = SensorService
    """
    pb2, pb2_grpc = _ensure_grpc_stubs()

    class SensorServiceServicer(pb2_grpc.SensorServiceServicer):
        def GetSensorData(self, request, context):
            count = request.count if request.count > 0 else rows
            batch = generate_batch(count, sample_rate_hz)
            serialized = [json.dumps(r) for r in batch]
            return pb2.SensorDataResponse(serialized_data=serialized)

        def StreamSensorData(self, request, context):
            count = request.count if request.count > 0 else rows
            ts = datetime.datetime.now(datetime.timezone.utc)
            dt = datetime.timedelta(seconds=1.0 / sample_rate_hz)
            t_seconds = time.time()
            for i in range(count):
                sensor = SENSORS[i % len(SENSORS)]
                reading = generate_reading(sensor, ts, t_seconds)
                yield pb2.SensorReading(
                    timestamp=reading["timestamp"],
                    voltage=reading["voltage"],
                    sensor_id=reading["sensor_id"],
                    latitude=reading["latitude"],
                    longitude=reading["longitude"],
                )
                ts += dt
                t_seconds += 1.0 / sample_rate_hz

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    pb2_grpc.add_SensorServiceServicer_to_server(SensorServiceServicer(), server)
    server.add_insecure_port(f"0.0.0.0:{port}")
    server.start()
    print(f"[gRPC] Server listening on 0.0.0.0:{port}")
    print(f"[gRPC] Serving {rows} readings/request at {sample_rate_hz} Hz")
    print(f"[gRPC] Press Ctrl+C to stop")
    print()
    print("[gRPC] To connect AnyLog to this server, run on the AnyLog node:")
    print(f"  run grpc client where name = eads_sensors and ip = <THIS_HOST_IP> "
          f"and port = {port} \\")
    print(f"    and grpc_dir = <PATH_TO_PROTO_DIR> and proto = sensor_data \\")
    print(f"    and function = GetSensorData and request = SensorRequest \\")
    print(f"    and response = SensorDataResponse and service = SensorService \\")
    print(f"    and dbms = {DBMS_NAME} and table = {TABLE_NAME}")

    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        print("\n[gRPC] Shutting down...")
        server.stop(grace=2)


# ---------------------------------------------------------------------------
# Continuous streaming mode (REST)
# ---------------------------------------------------------------------------

def stream_continuous(conn: str, auth: tuple, method: str, topic: str,
                      sample_rate_hz: int, batch_size: int, mode: str = "streaming"):
    """
    Continuously generate and send batches to sustain the target sample rate.
    Batch size controls how many readings per HTTP request.
    """
    interval = batch_size / sample_rate_hz  # seconds between sends
    print(f"[Stream] Target: {sample_rate_hz} Hz ({batch_size} readings/batch, "
          f"1 batch every {interval:.3f}s)")
    print(f"[Stream] Method: {method.upper()} to {conn}")
    print("[Stream] Press Ctrl+C to stop\n")
    total_sent = 0
    batches_sent = 0
    start = time.monotonic()
    try:
        while True:
            t0 = time.monotonic()
            batch = generate_batch(batch_size, sample_rate_hz)
            if method == "put":
                ok = rest_put(conn, auth, batch, mode)
            else:
                ok = rest_post(conn, auth, topic, batch)
            if ok:
                total_sent += len(batch)
                batches_sent += 1
            elapsed = time.monotonic() - t0
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)
    except KeyboardInterrupt:
        duration = time.monotonic() - start
        actual_rate = total_sent / duration if duration > 0 else 0
        print(f"\n[Stream] Stopped after {duration:.1f}s")
        print(f"[Stream] Sent {total_sent} readings in {batches_sent} batches")
        print(f"[Stream] Effective rate: {actual_rate:.0f} Hz")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

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
        description="EADS Data Generator — push mock sensor data to AnyLog")
    sub = parser.add_subparsers(dest="command", required=True)

    # -- rest-put --
    p_put = sub.add_parser("rest-put", help="Push data via REST PUT")
    p_put.add_argument("--conn", default="127.0.0.1:32149",
                       help="AnyLog REST endpoint (host:port or user:pass@host:port)")
    p_put.add_argument("--rows", type=int, default=1000,
                       help="Number of readings to generate (one-shot) or batch size (continuous)")
    p_put.add_argument("--hz", type=int, default=1000,
                       help="Sample rate in Hz (default: 1000)")
    p_put.add_argument("--mode", choices=["streaming", "file"], default="streaming",
                       help="AnyLog ingestion mode")
    p_put.add_argument("--continuous", action="store_true",
                       help="Stream continuously instead of one-shot")

    # -- rest-post --
    p_post = sub.add_parser("rest-post", help="Push data via REST POST (topic-based)")
    p_post.add_argument("--conn", default="127.0.0.1:32149",
                        help="AnyLog REST endpoint (host:port or user:pass@host:port)")
    p_post.add_argument("--rows", type=int, default=1000,
                        help="Number of readings to generate (one-shot) or batch size (continuous)")
    p_post.add_argument("--hz", type=int, default=1000,
                        help="Sample rate in Hz (default: 1000)")
    p_post.add_argument("--topic", default="eads-sensors",
                        help="MQTT topic name (must match AnyLog MQTT client config)")
    p_post.add_argument("--continuous", action="store_true",
                        help="Stream continuously instead of one-shot")

    # -- grpc-serve --
    p_grpc = sub.add_parser("grpc-serve",
                            help="Start gRPC server (AnyLog connects as client)")
    p_grpc.add_argument("--port", type=int, default=50051,
                        help="Port for gRPC server")
    p_grpc.add_argument("--rows", type=int, default=1000,
                        help="Readings per request")
    p_grpc.add_argument("--hz", type=int, default=1000,
                        help="Sample rate in Hz (default: 1000)")

    # -- sample --
    p_sample = sub.add_parser("sample", help="Print sample data (no network)")
    p_sample.add_argument("--rows", type=int, default=20,
                          help="Number of sample readings")
    p_sample.add_argument("--hz", type=int, default=1000,
                          help="Sample rate in Hz (default: 1000)")

    args = parser.parse_args()

    if args.command == "sample":
        batch = generate_batch(args.rows, args.hz)
        print(json.dumps(batch, indent=2))
        return

    if args.command == "rest-put":
        conn, auth = parse_conn(args.conn)
        if args.continuous:
            stream_continuous(conn, auth, "put", "", args.hz, args.rows,
                              args.mode)
        else:
            batch = generate_batch(args.rows, args.hz)
            rest_put(conn, auth, batch, args.mode)

    elif args.command == "rest-post":
        conn, auth = parse_conn(args.conn)
        if args.continuous:
            stream_continuous(conn, auth, "post", args.topic,
                              args.hz, args.rows)
        else:
            batch = generate_batch(args.rows, args.hz)
            rest_post(conn, auth, args.topic, batch)

    elif args.command == "grpc-serve":
        run_grpc_server(args.port, args.rows, args.hz)


if __name__ == "__main__":
    main()
