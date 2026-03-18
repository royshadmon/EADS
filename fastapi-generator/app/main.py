"""
EADS Data Generator — FastAPI wrapper

Wraps the updated eads_data_generator.py module in a containerised
FastAPI service. One container = one AnyLog operator.

The background task streams the PowerGridSense CSV dataset using the
original module's load_dataset / dataset_iterator / format_reading /
rest_put functions.
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
    "effective_rows_sec": 0.0,
    "last_reading": None,
    "dataset_rows": 0,
}

_stream_task: asyncio.Task | None = None


# ---------------------------------------------------------------------------
# Background streaming loop
# ---------------------------------------------------------------------------
async def _stream_loop():
    """
    Continuously stream the CSV dataset in batches using the
    module's functions.  Runs the synchronous rest_put in a thread pool
    so we don't block the FastAPI event loop.
    """
    conn, auth = gen.parse_conn(settings.anylog_conn)
    batch_size = settings.batch_size
    rate_hz = settings.rate_hz
    batch_interval = 1.0 / rate_hz
    mode = settings.anylog_mode

    # Load dataset (synchronous file I/O, run in thread)
    loop = asyncio.get_running_loop()
    data = await loop.run_in_executor(None, gen.load_dataset, settings.csv_path)
    state["dataset_rows"] = len(data)

    iterator = gen.dataset_iterator(data)
    state["running"] = True
    start = time.monotonic()

    effective_rate = batch_size * rate_hz
    logger.info(
        "Streaming started  csv=%s (%d rows)  target=%s  "
        "batch=%d  rate=%.1f batches/sec  effective=%.0f rows/sec  mode=%s",
        settings.csv_path,
        len(data),
        settings.anylog_conn,
        batch_size,
        rate_hz,
        effective_rate,
        mode,
    )

    try:
        while True:
            t0 = time.monotonic()

            # Collect batch from the infinite iterator
            batch = []
            for _ in range(batch_size):
                row = next(iterator)
                batch.append(gen.format_reading(row))

            # Push via the original module's rest_put (synchronous)
            ok = await loop.run_in_executor(
                None, gen.rest_put, conn, auth, batch, mode
            )

            if ok:
                state["readings_sent"] += len(batch)
                state["batches_sent"] += 1
                state["last_reading"] = batch[-1]
            else:
                state["errors"] += 1

            # Update effective rate
            elapsed_total = time.monotonic() - start
            if elapsed_total > 0:
                state["effective_rows_sec"] = round(
                    state["readings_sent"] / elapsed_total, 1
                )

            if state["batches_sent"] % 50 == 0 and state["batches_sent"]:
                logger.info(
                    "Progress: %d sent (%d batches, %d errors) — %.0f rows/sec",
                    state["readings_sent"],
                    state["batches_sent"],
                    state["errors"],
                    state["effective_rows_sec"],
                )

            # Rate limiting
            elapsed_batch = time.monotonic() - t0
            sleep_time = batch_interval - elapsed_batch
            if sleep_time > 0:
                await asyncio.sleep(sleep_time)

    except asyncio.CancelledError:
        logger.info("Stream loop cancelled")
    finally:
        duration = time.monotonic() - start
        logger.info(
            "Stopped after %.1fs — %d rows, %.0f rows/sec effective",
            duration,
            state["readings_sent"],
            state["readings_sent"] / duration if duration else 0,
        )
        state["running"] = False


# ---------------------------------------------------------------------------
# App lifespan
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _stream_task
    _stream_task = asyncio.create_task(_stream_loop())
    yield
    _stream_task.cancel()
    try:
        await _stream_task
    except asyncio.CancelledError:
        pass


app = FastAPI(
    title="EADS Data Generator",
    description="Containerised wrapper around eads_data_generator.py (Sprint 2)",
    version="0.2.0",
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
        "target": settings.anylog_conn,
        "csv_path": settings.csv_path,
        "dataset_rows": state["dataset_rows"],
        "batch_size": settings.batch_size,
        "rate_hz": settings.rate_hz,
        "mode": settings.anylog_mode,
        "readings_sent": state["readings_sent"],
        "batches_sent": state["batches_sent"],
        "errors": state["errors"],
        "effective_rows_sec": state["effective_rows_sec"],
        "last_reading": state["last_reading"],
    }
