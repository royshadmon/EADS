"""
EADS Data Generator — FastAPI wrapper.

Wraps the existing eads_data_generator.py module in a containerised
FastAPI service. One container = one AnyLog operator.

The background task calls the same generate_batch / rest_put / rest_post /
grpc functions from the original script, configured via env vars.
"""

import asyncio
import logging
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.config import settings

# Import the teammate's generator module (copied into the image as-is)
import eads_data_generator as gen

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(name)-20s  %(levelname)-8s  %(message)s",
)
logger = logging.getLogger("eads.fastapi")

# ---------------------------------------------------------------------------
# Shared state (exposed via /status)
# ---------------------------------------------------------------------------
state = {
    "running": False,
    "readings_sent": 0,
    "batches_sent": 0,
    "errors": 0,
    "last_reading": None,
    "effective_hz": 0.0,
}

_stream_task: asyncio.Task | None = None


# ---------------------------------------------------------------------------
# Override the SENSORS list so this container generates for ONE sensor
# ---------------------------------------------------------------------------
def _apply_sensor_config():
    """Replace the default SENSORS list with the single sensor from env."""
    gen.SENSORS = [
        {
            "sensor_id": settings.sensor_id,
            "lat": settings.sensor_lat,
            "lon": settings.sensor_lon,
            "rms_voltage": settings.sensor_rms_voltage,
        }
    ]
    gen.DBMS_NAME = settings.anylog_dbms
    gen.TABLE_NAME = settings.anylog_table


# ---------------------------------------------------------------------------
# Background streaming loop (mirrors stream_continuous from the CLI)
# ---------------------------------------------------------------------------
async def _stream_loop():
    """
    Continuously generate + push batches using the original module's
    rest_put / rest_post functions (synchronous), run in a thread so
    we don't block the FastAPI event loop.
    """
    conn, auth = gen.parse_conn(settings.anylog_conn)
    hz = settings.sample_rate_hz
    batch_size = settings.batch_size
    interval = batch_size / hz  # seconds between sends
    method = settings.method

    state["running"] = True
    start = time.monotonic()

    logger.info(
        "Streaming started  sensor=%s  method=%s  target=%s  hz=%d  batch=%d",
        settings.sensor_id, method, settings.anylog_conn, hz, batch_size,
    )

    loop = asyncio.get_running_loop()

    try:
        while True:
            t0 = time.monotonic()

            # Generate batch (CPU-bound but fast, fine in thread)
            batch = await loop.run_in_executor(
                None, gen.generate_batch, batch_size, hz
            )

            # Push via the original module's functions
            if method == "rest-put":
                ok = await loop.run_in_executor(
                    None, gen.rest_put, conn, auth, batch, settings.anylog_mode
                )
            elif method == "rest-post":
                ok = await loop.run_in_executor(
                    None, gen.rest_post, conn, auth, settings.mqtt_topic, batch
                )
            else:
                logger.error("Unsupported method for streaming: %s", method)
                break

            if ok:
                state["readings_sent"] += len(batch)
                state["batches_sent"] += 1
                state["last_reading"] = batch[-1]
            else:
                state["errors"] += 1

            # Update effective rate
            elapsed_total = time.monotonic() - start
            if elapsed_total > 0:
                state["effective_hz"] = round(
                    state["readings_sent"] / elapsed_total, 1
                )

            if state["batches_sent"] % 50 == 0 and state["batches_sent"]:
                logger.info(
                    "Progress: %d sent (%d batches, %d errors) — %.0f Hz effective",
                    state["readings_sent"],
                    state["batches_sent"],
                    state["errors"],
                    state["effective_hz"],
                )

            # Sleep to maintain target rate
            elapsed_batch = time.monotonic() - t0
            sleep_time = interval - elapsed_batch
            if sleep_time > 0:
                await asyncio.sleep(sleep_time)

    except asyncio.CancelledError:
        logger.info("Stream loop cancelled")
    finally:
        duration = time.monotonic() - start
        logger.info(
            "Stopped after %.1fs — %d readings, %.0f Hz effective",
            duration, state["readings_sent"],
            state["readings_sent"] / duration if duration else 0,
        )
        state["running"] = False


# ---------------------------------------------------------------------------
# gRPC mode (blocks, so we run it in a thread)
# ---------------------------------------------------------------------------
async def _grpc_loop():
    """Run the gRPC server from the original module."""
    state["running"] = True
    loop = asyncio.get_running_loop()
    logger.info("Starting gRPC server on port %d", settings.grpc_port)
    try:
        await loop.run_in_executor(
            None,
            gen.run_grpc_server,
            settings.grpc_port,
            settings.batch_size,
            settings.sample_rate_hz,
        )
    except asyncio.CancelledError:
        logger.info("gRPC loop cancelled")
    finally:
        state["running"] = False


# ---------------------------------------------------------------------------
# App lifespan
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _stream_task
    _apply_sensor_config()

    if settings.method == "grpc-serve":
        _stream_task = asyncio.create_task(_grpc_loop())
    else:
        _stream_task = asyncio.create_task(_stream_loop())

    yield

    _stream_task.cancel()
    try:
        await _stream_task
    except asyncio.CancelledError:
        pass


app = FastAPI(
    title="EADS Data Generator",
    description="Containerised wrapper around eads_data_generator.py",
    version="0.1.0",
    lifespan=lifespan,
)


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------
@app.get("/health")
async def health():
    return {"status": "ok", "streaming": state["running"]}


@app.get("/status")
async def status():
    return {
        "sensor_id": settings.sensor_id,
        "method": settings.method,
        "target": settings.anylog_conn,
        "sample_rate_hz": settings.sample_rate_hz,
        "batch_size": settings.batch_size,
        "readings_sent": state["readings_sent"],
        "batches_sent": state["batches_sent"],
        "errors": state["errors"],
        "effective_hz": state["effective_hz"],
        "last_reading": state["last_reading"],
    }
