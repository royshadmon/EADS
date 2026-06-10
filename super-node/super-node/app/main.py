import asyncio
import json
import logging
import os
import socket
from datetime import datetime, timedelta
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.staticfiles import StaticFiles
from starlette.responses import JSONResponse

from anomaly_notification.anomaly_report import AnomalyReport, EMAIL_CONFIG, send_anomaly_email, AnomalyType

logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")
logger = logging.getLogger("super-node")

ANYLOG_HOST = os.getenv("ANYLOG_HOST", "127.0.0.1")
ANYLOG_PORT = int(os.getenv("ANYLOG_PORT", "32149"))
ANYLOG_DBMS = os.getenv("ANYLOG_DBMS", "eads")
ANYLOG_URL = f"http://{ANYLOG_HOST}:{ANYLOG_PORT}"
MONITOR_INTERVAL_S = int(os.getenv("MONITOR_INTERVAL_S", "20"))
OFFLINE_WINDOW_S = int(os.getenv("OFFLINE_WINDOW_S", "120"))

EMAIL_CONFIG.update({
    "smtp_host": os.getenv("EMAIL_SMTP_HOST", EMAIL_CONFIG.get("smtp_host", "smtp.gmail.com")),
    "smtp_port": int(os.getenv("EMAIL_SMTP_PORT", EMAIL_CONFIG.get("smtp_port", 587))),
    "username": os.getenv("EMAIL_USERNAME", EMAIL_CONFIG.get("username", "")),
    "password": os.getenv("EMAIL_PASSWORD", EMAIL_CONFIG.get("password", "")),
    "from_addr": os.getenv("EMAIL_FROM", EMAIL_CONFIG.get("from_addr", "")),
    "to_addrs": [addr.strip() for addr in os.getenv("EMAIL_TO", ",".join(EMAIL_CONFIG.get("to_addrs", []))).split(",") if addr.strip()],
})

DEFAULT_NODES = [
    {"id": "n1", "name": "Pi-1", "sensorFilter": ""},
    {"id": "n2", "name": "Pi-2", "sensorFilter": ""},
    {"id": "n3", "name": "Pi-3", "sensorFilter": ""},
]

NODE_LIST = os.getenv("NODE_LIST")
if NODE_LIST:
    try:
        import json

        NODES = json.loads(NODE_LIST)
    except Exception as exc:
        logging.warning("Failed to parse NODE_LIST, using defaults: %s", exc)
        NODES = DEFAULT_NODES
else:
    NODES = DEFAULT_NODES

previous_status: Dict[str, bool] = {}

# Active alerts to prevent duplicate emails: {(node_id, anomaly_type): start_time}
_active_alerts: Dict[tuple, dict] = {}
_alerts_lock = asyncio.Lock()
app = FastAPI(title="EADS Super Node")


def format_anylog_timestamp(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def anylog_headers(sql: str) -> Dict[str, str]:
    return {
        "User-Agent": "AnyLog/1.23",
        "command": f'sql {ANYLOG_DBMS} format=json "{sql}"',
        "destination": "network",
    }


def _build_http_request(method: str, path: str, headers: Dict[str, str], body: bytes = b"") -> bytes:
    lines = [f"{method} {path} HTTP/1.1"]
    for key, value in headers.items():
        lines.append(f"{key}: {value}")
    if body:
        lines.append(f"Content-Length: {len(body)}")
    lines.append("Connection: close")
    request = "\r\n".join(lines).encode("ascii") + b"\r\n\r\n"
    return request + body


def _parse_http_response(raw: bytes) -> tuple[int, str, Dict[str, str], bytes]:
    header_bytes, sep, body = raw.partition(b"\r\n\r\n")
    header_lines = header_bytes.split(b"\r\n")
    status_line = header_lines[0].decode("iso-8859-1", "replace")
    parts = status_line.split(" ", 2)
    status = int(parts[1]) if len(parts) > 1 else 200
    reason = parts[2] if len(parts) > 2 else ""
    headers = {}
    for line in header_lines[1:]:
        if b":" not in line:
            continue
        key, value = line.split(b":", 1)
        headers[key.decode("ascii").strip().lower()] = value.decode("ascii").strip()
    if headers.get("transfer-encoding", "").lower() == "chunked":
        if body.startswith((b"{", b"[")):
            return status, reason, headers, body
        cleaned = b""
        offset = 0
        while offset < len(body):
            line_end = body.find(b"\r\n", offset)
            if line_end == -1:
                break
            chunk_size = int(body[offset:line_end].split(b";", 1)[0].strip(), 16)
            offset = line_end + 2
            if chunk_size == 0:
                break
            cleaned += body[offset:offset + chunk_size]
            offset += chunk_size + 2
        return status, reason, headers, cleaned
    return status, reason, headers, body


def _raw_anylog_request(
    method: str,
    path: str,
    headers: Dict[str, str],
    body: bytes = b"",
    anylog_host: str = ANYLOG_HOST,
    anylog_port: int = ANYLOG_PORT,
) -> tuple[int, str, Dict[str, str], bytes]:
    request_headers = headers.copy()
    request_headers["Host"] = f"{anylog_host}:{anylog_port}"
    request_bytes = _build_http_request(method, path, request_headers, body)
    with socket.create_connection((anylog_host, anylog_port), timeout=10) as sock:
        sock.sendall(request_bytes)
        response = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
    return _parse_http_response(response)


async def query_anylog(sql: str) -> List[Dict[str, Any]]:
    status, reason, headers, body = await asyncio.to_thread(
        _raw_anylog_request,
        "GET",
        "/",
        anylog_headers(sql),
        b"",
    )
    if status >= 400:
        raise HTTPException(status_code=502, detail=f"AnyLog query failed: {status} {reason}")
    try:
        payload = json.loads(body.decode("utf-8", "replace"))
    except json.JSONDecodeError as exc:
        raise HTTPException(status_code=502, detail=f"Could not parse AnyLog response: {exc}")
    if isinstance(payload, dict) and "Query" in payload:
        return payload["Query"]
    if isinstance(payload, list):
        return payload
    return []


def render_sensor_clause(sensor_filter: str) -> str:
    if not sensor_filter:
        return ""
    escaped = sensor_filter.replace("'", "''")
    return f" AND sensor_id = '{escaped}'"


def make_alert_report(node: Dict[str, Any], start_time: datetime) -> AnomalyReport:
    return AnomalyReport(
        anomaly_type=AnomalyType.OUTAGE,
        failed_test=f"No data received from {node['name']} in the last {OFFLINE_WINDOW_S} seconds.",
        sensor_id=node["id"],
        location=node["name"],
        start_time=start_time,
        measured_voltage=0.0,
        expected_voltage=None,
        measured_frequency=None,
    )


async def check_node_offline(node: Dict[str, Any]) -> bool:
    """Check if a node is offline by querying operator status (not voltage_calibrated row count)."""
    try:
        destination = node.get("destination", "")
        status, reason, headers, body = await asyncio.to_thread(
            _raw_anylog_request,
            "GET",
            "/",
            {
                "User-Agent": "AnyLog/1.23",
                "command": "get status",
                "destination": destination,
            },
            b"",
            ANYLOG_HOST,
            ANYLOG_PORT,
        )
        if status >= 400:
            return True  # offline if status error
        response_text = body.decode("utf-8", errors="ignore")
        # Node is online if response contains "running"
        return "running" not in response_text.lower()
    except Exception as e:
        logger.warning("Failed to check node %s: %s", node.get("id"), e)
        return True  # treat errors as offline


async def monitor_loop() -> None:
    while True:
        for node in NODES:
            try:
                offline = await check_node_offline(node)
                previous = previous_status.get(node["id"], True)
                key = (node["id"], "outage")
                
                if offline and previous:
                    # Node went offline
                    logger.warning("Node offline detected: %s", node["name"])
                    
                    # Check dedup: only send if not already in _active_alerts
                    start_time = datetime.utcnow()
                    should_send = False
                    async with _alerts_lock:
                        if key not in _active_alerts:
                            _active_alerts[key] = {"start_time": start_time, "node_name": node["name"]}
                            should_send = True
                    
                    if should_send:
                        report = make_alert_report(node, start_time)
                        logger.info("Attempting to send email. Config: to_addrs=%s, username=%s, has_password=%s",
                                   EMAIL_CONFIG["to_addrs"], EMAIL_CONFIG["username"], bool(EMAIL_CONFIG["password"]))
                        if EMAIL_CONFIG["to_addrs"] and EMAIL_CONFIG["username"] and EMAIL_CONFIG["password"]:
                            try:
                                send_anomaly_email(report)
                            except Exception as e:
                                logger.error("Failed to send email for %s: %s", node["name"], e)
                        else:
                            logger.warning("Email alert skipped because email settings are not fully configured.")
                elif not offline and not previous:
                    # Node recovered
                    logger.info("Node recovered: %s", node["name"])
                    async with _alerts_lock:
                        _active_alerts.pop(key, None)
                        
                previous_status[node["id"]] = not offline
            except Exception as exc:
                logger.error("Failed to check node %s: %s", node["id"], exc)
        await asyncio.sleep(MONITOR_INTERVAL_S)


@app.post("/api/notify")
async def notify_anomaly(request: Request) -> JSONResponse:
    """Receive anomaly notifications from the dashboard and send a single email per anomaly.

    Payload: { node_id, node_name, anomaly_type='outage', description?, resolved: bool }
    """
    body = await request.json()
    node_id = body.get("node_id")
    node_name = body.get("node_name") or body.get("name") or node_id
    anomaly_type = body.get("anomaly_type", "outage")
    description = body.get("description", "")
    resolved = bool(body.get("resolved", False))

    key = (node_id, anomaly_type)
    logger.info("Notify request: node_id=%s node_name=%s anomaly_type=%s resolved=%s", node_id, node_name, anomaly_type, resolved)
    async with _alerts_lock:
        if resolved:
            # If resolved and active, clear the stored alert so a future outage can send again.
            _active_alerts.pop(key, None)
            logger.info("Notify resolved cleared: %s", key)
            return JSONResponse({"status": "resolved"})

        # Not resolved: create/send alert only if not already active
        if key in _active_alerts:
            logger.info("Notify duplicate skipped: %s", key)
            return JSONResponse({"status": "already_sent"})

        start_time = datetime.utcnow()
        _active_alerts[key] = {"start_time": start_time, "node_name": node_name}
        logger.info("Notify sending first alert: %s", key)

    # Build and send the email outside the lock
    report = AnomalyReport(
        anomaly_type=AnomalyType.OUTAGE,
        failed_test=description or f"No data received from {node_name}",
        sensor_id=node_id,
        location=node_name,
        start_time=start_time,
    )
    try:
        send_anomaly_email(report)
    except Exception as e:
        logger.error("Failed to send anomaly email: %s", e)

    return JSONResponse({"status": "sent"})


@app.on_event("startup")
async def startup_event() -> None:
    asyncio.create_task(monitor_loop())
    logger.info("Super Node monitoring %d nodes", len(NODES))


@app.api_route("/anylog", methods=["GET", "POST", "OPTIONS"])
async def anylog_proxy(request: Request) -> Response:
    if request.method == "OPTIONS":
        return Response(status_code=204, headers={
            "Access-Control-Allow-Origin": "*",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "User-Agent, command, destination, Content-Type",
            "Access-Control-Max-Age": "86400",
        })

    headers = {key: value for key, value in request.headers.items() if key.lower() in {"user-agent", "command", "destination", "content-type"}}
    body = await request.body()
    target_host = request.headers.get("x-anylog-host", ANYLOG_HOST)
    target_port = int(request.headers.get("x-anylog-port", str(ANYLOG_PORT)))
    status, reason, response_headers, response_body = await asyncio.to_thread(
        _raw_anylog_request,
        request.method,
        "/",
        headers,
        body,
        target_host,
        target_port,
    )
    response_headers = {k: v for k, v in response_headers.items() if k.lower() not in {"content-encoding", "transfer-encoding", "connection", "host"}}
    return Response(content=response_body, status_code=status, headers=response_headers)


@app.get("/api/nodes")
async def node_status() -> JSONResponse:
    result = []
    for node in NODES:
        result.append({
            "id": node["id"],
            "name": node["name"],
            "offline": not previous_status.get(node["id"], False),
            "sensorFilter": node.get("sensorFilter", ""),
        })
    return JSONResponse(result)


# Serve the dashboard static files at root (mounted after API routes to avoid shadowing)
app.mount("/", StaticFiles(directory="/app/user-dashboard", html=True), name="static")
