"""
outage_proxy.py
===============
A FastAPI proxy that sits between the EADS data generator and AnyLog.

Architecture
------------
  [Generator container] ──PUT──> [Outage Proxy :9001/:9002/:9003] ──PUT──> [AnyLog operator]

One proxy instance per operator.  The proxy target (AnyLog URL) is set
via the ANYLOG_URL environment variable.

Outage state machine
--------------------
  NORMAL  →  /sim-power-outage  →  SPIKE (3 s)  →  OUTAGE (zeros)
  OUTAGE  →  /end-power-outage  →  NORMAL

  NORMAL  : forwards real readings unchanged
  SPIKE   : overwrites with high voltage_est values for N seconds
  OUTAGE  : overwrites all readings with zeros

Control endpoints (call from terminal with curl or the CLI helper)
------------------------------------------------------------------
  POST /sim-power-outage          – start spike → outage sequence
  POST /end-power-outage          – return to normal immediately
  GET  /status                    – current state
  GET  /health                    – liveness probe

Forwarding endpoint
-------------------
  PUT  /                          – receives batches from the generator and
                                    forwards them to AnyLog (with optional overwrite)
"""

import json
import logging
import os
import random
import threading
import time

import httpx
from fastapi import FastAPI, BackgroundTasks, Header, Request
from typing import Optional

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(name)-24s  %(levelname)-8s  %(message)s",
)
logger = logging.getLogger("outage_proxy")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
# The real AnyLog operator this proxy forwards to.
# Set ANYLOG_URL in docker-compose or on the CLI; default keeps single-node usage working.
ANYLOG_URL: str = os.getenv("ANYLOG_URL", "http://127.0.0.1:32149")

# How many seconds the spike phase lasts before flipping to full outage.
SPIKE_DURATION_SEC: int = int(os.getenv("SPIKE_DURATION_SEC", "3"))

# Operator label used in log messages (purely cosmetic, set via env).
OPERATOR_LABEL: str = os.getenv("OPERATOR_LABEL", "operator-1")

# ---------------------------------------------------------------------------
# Outage state  (protected by _lock)
# ---------------------------------------------------------------------------
_outage_active: bool = False   # True  → overwrite with zeros
_spiking:       bool = False   # True  → overwrite with spike values
_lock = threading.Lock()

# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(
    title=f"Outage Proxy — {OPERATOR_LABEL}",
    description=(
        "Transparent proxy between the EADS data generator and an AnyLog operator.\n\n"
        "Supports simulated power-outage events with a voltage spike pre-cursor."
    ),
    version="1.0.0",
)

# ---------------------------------------------------------------------------
# Overwrite helpers
# ---------------------------------------------------------------------------

def _apply_spike(reading: dict) -> dict:
    """Overwrite a reading with an extreme high-voltage spike."""
    spike_voltage = round(random.uniform(250.0, 350.0), 4)
    adc_centered = spike_voltage / 281.793554  # reverse calibration
    adc_input = adc_centered + 1.692297
    return {
        **reading,
        "voltage_est":        spike_voltage,
        "adc_centered_volts": round(adc_centered, 6),
        "adc_input_volts":    round(adc_input, 6),
        "raw_count":          str(int(adc_input * 4096.0 / 3.3)),
    }


def _apply_outage(reading: dict) -> dict:
    """Overwrite a reading with zeros — simulates a complete power outage."""
    return {
        **reading,
        "voltage_est":        0.0,
        "adc_centered_volts": 0.0,
        "adc_input_volts":    0.0,
        "raw_count":          "0",
    }


def _current_mode() -> str:
    with _lock:
        if _spiking:
            return "spike"
        if _outage_active:
            return "outage"
        return "normal"

# ---------------------------------------------------------------------------
# Background timer: ends the spike phase and transitions to full outage
# ---------------------------------------------------------------------------

def _spike_timer() -> None:
    """Runs in a background thread; ends the spike phase after SPIKE_DURATION_SEC."""
    time.sleep(SPIKE_DURATION_SEC)
    with _lock:
        if _outage_active:          # only flip if the outage is still active
            global _spiking
            _spiking = False
            logger.info("[%s] Spike phase ended → outage (zeros) now active", OPERATOR_LABEL)

# ---------------------------------------------------------------------------
# Forwarding endpoint  (receives batches from the generator)
# ---------------------------------------------------------------------------

@app.put("/")
async def receive_and_forward(
    request: Request,
    type_header: Optional[str] = Header(None, alias="type"),
    dbms:        Optional[str] = Header(None),
    table:       Optional[str] = Header(None),
    mode:        Optional[str] = Header(None),
):
    """
    Accepts a JSON batch from the EADS generator, optionally overwrites
    the readings depending on the current outage state, then forwards the
    (possibly modified) batch to the configured AnyLog operator.
    """
    # 1. Parse incoming batch
    body = await request.body()
    try:
        batch = json.loads(body)
    except json.JSONDecodeError as exc:
        logger.warning("[%s] Received malformed JSON: %s", OPERATOR_LABEL, exc)
        return {"status": "error", "detail": "invalid JSON"}, 400

    if isinstance(batch, dict):
        batch = [batch]

    # 2. Apply overwrite if needed
    with _lock:
        spiking = _spiking
        active  = _outage_active

    if spiking:
        batch = [_apply_spike(r) for r in batch]
    elif active:
        batch = [_apply_outage(r) for r in batch]

    # 3. Rebuild forwarding headers from what the generator sent
    forward_headers: dict[str, str] = {"Content-Type": "text/plain"}
    if type_header: forward_headers["type"]  = type_header
    if dbms:        forward_headers["dbms"]  = dbms
    if table:       forward_headers["table"] = table
    if mode:        forward_headers["mode"]  = mode

    # 4. Forward to AnyLog
    try:
        resp = httpx.put(
            ANYLOG_URL,
            content=json.dumps(batch),
            headers=forward_headers,
            timeout=5.0,
        )
        anylog_status = resp.status_code
    except httpx.RequestError as exc:
        logger.error("[%s] Failed to reach AnyLog at %s: %s", OPERATOR_LABEL, ANYLOG_URL, exc)
        anylog_status = None

    current_mode = "spike" if spiking else ("outage" if active else "normal")

    logger.debug(
        "[%s] Forwarded %d readings  mode=%s  anylog_status=%s",
        OPERATOR_LABEL, len(batch), current_mode, anylog_status,
    )

    return {
        "status":        "forwarded",
        "operator":      OPERATOR_LABEL,
        "count":         len(batch),
        "proxy_mode":    current_mode,
        "anylog_status": anylog_status,
    }

# ---------------------------------------------------------------------------
# Outage control endpoints
# ---------------------------------------------------------------------------

@app.post(
    "/sim-power-outage",
    summary="Trigger a power outage (spike → zeros)",
    response_description="Outage started; spike phase lasts SPIKE_DURATION_SEC seconds.",
)
def sim_power_outage(background_tasks: BackgroundTasks):
    """
    Start a simulated power outage on **this operator only**.

    Phase 1 — Spike (default 3 s): readings are overwritten with extreme
    voltage_est values.

    Phase 2 — Outage: all readings are zeroed out
    until `POST /end-power-outage` is called.
    """
    global _outage_active, _spiking
    with _lock:
        if _outage_active:
            return {
                "status":   "already_active",
                "operator": OPERATOR_LABEL,
                "message":  "Outage is already running on this operator.",
            }
        _outage_active = True
        _spiking       = True

    background_tasks.add_task(_spike_timer)
    logger.info(
        "[%s] ⚡ Power outage triggered  spike=%ds → outage",
        OPERATOR_LABEL, SPIKE_DURATION_SEC,
    )
    return {
        "status":         "outage_started",
        "operator":       OPERATOR_LABEL,
        "spike_duration": SPIKE_DURATION_SEC,
        "message":        (
            f"Spike phase active for {SPIKE_DURATION_SEC}s, "
            "then zeroing all readings until /end-power-outage."
        ),
    }


@app.post(
    "/end-power-outage",
    summary="End the outage and return to normal forwarding",
)
def end_power_outage():
    """
    Deactivate the outage on this operator.  Real sensor data will be
    forwarded to AnyLog immediately after this call.
    """
    global _outage_active, _spiking
    with _lock:
        was_active     = _outage_active
        _outage_active = False
        _spiking       = False

    if not was_active:
        return {
            "status":   "not_active",
            "operator": OPERATOR_LABEL,
            "message":  "No outage was active on this operator.",
        }

    logger.info("[%s] ✅ Power outage ended — forwarding real data again", OPERATOR_LABEL)
    return {
        "status":   "outage_ended",
        "operator": OPERATOR_LABEL,
        "message":  "Overwriting deactivated. Forwarding real data.",
    }

# ---------------------------------------------------------------------------
# Status / health endpoints
# ---------------------------------------------------------------------------

@app.get("/status", summary="Current proxy + outage state")
def get_status():
    """Returns the current outage state and configuration for this proxy instance."""
    with _lock:
        spiking = _spiking
        active  = _outage_active

    mode = "spike" if spiking else ("outage" if active else "normal")

    return {
        "operator":        OPERATOR_LABEL,
        "proxy_mode":      mode,
        "outage_active":   active,
        "spiking":         spiking,
        "anylog_target":   ANYLOG_URL,
        "spike_duration":  SPIKE_DURATION_SEC,
    }


@app.get("/health", summary="Liveness probe")
def health():
    return {"status": "ok", "operator": OPERATOR_LABEL}
