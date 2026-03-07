"""
EADS Voltage Predictor - Linear Regression Baseline
Predicts sensor voltage 5 seconds into the future using lag features.

Workflow:
  1. TRAIN  — fetch historical data from AnyLog, build lag features, fit model
  2. INFER  — every 0.5s, query recent readings, predict t+5s, PUT back to AnyLog

Predictions are written to: eads.voltage_predictions

Usage:
    # Train on last 24 hours of data, then run inference loop
    python eads_voltage_predictor.py run --conn 127.0.0.1:32149

    # Train only (saves model to disk)
    python eads_voltage_predictor.py train --conn 127.0.0.1:32149

    # Inference only (loads saved model)
    python eads_voltage_predictor.py infer --conn 127.0.0.1:32149

    # Override training window and lag window
    python eads_voltage_predictor.py run --conn 127.0.0.1:32149 --train-hours 48 --lags 10
"""

import argparse
import json
import pickle
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd
import requests
from sklearn.linear_model import LinearRegression
from sklearn.metrics import mean_absolute_error, r2_score
from sklearn.preprocessing import StandardScaler

# ── Configuration ──────────────────────────────────────────────────────────────

DBMS          = "eads"
SOURCE_TABLE  = "grid_readings"
PRED_TABLE    = "voltage_predictions"
HORIZON_SECS  = 5        # predict voltage this many seconds ahead
DEFAULT_LAGS  = 10       # number of lag readings used as features
INFER_HZ      = 2.0      # runs every 0.5 seconds
MODEL_PATH    = Path("eads_voltage_model.pkl")

# ── AnyLog REST helpers ────────────────────────────────────────────────────────

def anylog_query(conn: str, sql: str, auth: tuple = ()) -> Optional[pd.DataFrame]:
    """
    Run a SQL query against AnyLog via REST GET.
    Returns a DataFrame or None on failure.

    AnyLog command header format:
        sql <dbms> format = json and stat = false "<SELECT ...>"
    Note: spaces around '=' are required, SQL wrapped in double quotes.
    """
    url = f"http://{conn}"
    # Escape any double quotes inside the SQL itself
    sql_escaped = sql.replace('"', "'")
    headers = {
        "command": f'sql {DBMS} format = json and stat = false "{sql_escaped}"',
        "User-Agent": "AnyLog/1.23",
        "destination": "network",   # route to operator nodes
    }
    try:
        r = requests.get(url, headers=headers, auth=auth or None, timeout=60)
    except requests.RequestException as e:
        print(f"[Query] Connection error: {e}")
        print(f"[Query] Make sure AnyLog REST server is running at {conn}")
        return None

    if r.status_code != 200:
        print(f"[Query] HTTP {r.status_code}: {r.text[:400]}")
        return None

    raw = r.text.strip()
    if not raw:
        print(f"[Query] Empty response from AnyLog")
        return pd.DataFrame()

    try:
        data = r.json()
        # AnyLog may wrap rows under a 'Query' key
        if isinstance(data, dict):
            data = data.get("Query", data.get("query", []))
        if not data:
            return pd.DataFrame()
        return pd.DataFrame(data)
    except Exception as e:
        print(f"[Query] Failed to parse response: {e}\n  Raw: {raw[:400]}")
        return None


def anylog_put(conn: str, payload: list[dict], auth: tuple = ()) -> bool:
    """
    Write prediction rows to AnyLog via REST PUT.
    """
    headers = {
        "type": "json",
        "dbms": DBMS,
        "table": PRED_TABLE,
        "mode": "file",
        "Content-Type": "text/plain",
    }
    url = f"http://{conn}"
    try:
        r = requests.put(url, headers=headers, auth=auth or None,
                         data=json.dumps(payload), timeout=30)
    except requests.RequestException as e:
        print(f"[PUT] Failed: {e}")
        return False

    if r.status_code != 200:
        print(f"[PUT] HTTP {r.status_code}: {r.text[:200]}")
        return False

    return True

# ── Feature Engineering ────────────────────────────────────────────────────────

def build_lag_features(df: pd.DataFrame, n_lags: int) -> pd.DataFrame:
    """
    For each sensor_id, create n_lags lag columns of voltage
    and a target column = voltage shifted back by HORIZON_SECS rows.

    Assumes df is sorted by (sensor_id, timestamp).
    One row ≈ one second of data per sensor (matches 1 Hz streaming).
    """
    df = df.copy()
    df["timestamp"] = pd.to_datetime(df["timestamp"], utc=True, errors="coerce")
    df = df.sort_values(["sensor_id", "timestamp"]).reset_index(drop=True)

    feature_frames = []

    for sensor_id, grp in df.groupby("sensor_id"):
        grp = grp.reset_index(drop=True)
        grp["voltage"] = pd.to_numeric(grp["voltage"], errors="coerce")

        # Lag features: voltage at t-1, t-2, ..., t-n_lags
        for lag in range(1, n_lags + 1):
            grp[f"lag_{lag}"] = grp["voltage"].shift(lag)

        # Target: voltage n steps ahead (HORIZON_SECS rows forward)
        grp["target_voltage"] = grp["voltage"].shift(-HORIZON_SECS)

        feature_frames.append(grp)

    result = pd.concat(feature_frames, ignore_index=True)

    # Drop rows with NaN from shifting
    lag_cols = [f"lag_{i}" for i in range(1, n_lags + 1)]
    result = result.dropna(subset=lag_cols + ["target_voltage", "voltage"])

    return result


def get_feature_cols(n_lags: int) -> list[str]:
    return [f"lag_{i}" for i in range(1, n_lags + 1)]

# ── Training ───────────────────────────────────────────────────────────────────

def train(conn: str, auth: tuple, train_hours: int, n_lags: int) -> tuple:
    """
    Fetch historical data, build features, train LinearRegression.
    Returns (model, scaler, n_lags) or raises on failure.
    """
    print(f"\n{'='*60}")
    print(f"[Train] Fetching last {train_hours}h of data from AnyLog...")
    print(f"[Train] Source: {DBMS}.{SOURCE_TABLE}")

    sql = (
        f"SELECT timestamp, sensor_id, voltage "
        f"FROM {SOURCE_TABLE} "
        f"WHERE timestamp >= NOW() - {train_hours} hours "
        f"ORDER BY sensor_id, timestamp "
        f"LIMIT 10000"
    )

    df = anylog_query(conn, sql, auth)
    if df is None:
        raise RuntimeError("Failed to query AnyLog for training data.")
    if df.empty:
        raise RuntimeError(
            "No training data returned. "
            "Make sure eads_data_generator.py has been running first."
        )

    print(f"[Train] Retrieved {len(df):,} rows across "
          f"{df['sensor_id'].nunique() if 'sensor_id' in df.columns else '?'} sensors")

    # Build features
    print(f"[Train] Building lag features (n_lags={n_lags}, horizon={HORIZON_SECS}s)...")
    featured = build_lag_features(df, n_lags)
    feature_cols = get_feature_cols(n_lags)

    print(f"[Train] Feature matrix: {len(featured):,} rows × {len(feature_cols)} features")

    X = featured[feature_cols].values
    y = featured["target_voltage"].values

    # Train/test split (last 20% = test)
    split = int(len(X) * 0.8)
    X_train, X_test = X[:split], X[split:]
    y_train, y_test = y[:split], y[split:]

    # Scale
    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X_train)
    X_test_s  = scaler.transform(X_test)

    # Fit model
    print("[Train] Fitting LinearRegression...")
    model = LinearRegression()
    model.fit(X_train_s, y_train)

    # Evaluate
    y_pred_train = model.predict(X_train_s)
    y_pred_test  = model.predict(X_test_s)

    print(f"\n[Train] ── Results ──────────────────────────────")
    print(f"[Train]   Train MAE : {mean_absolute_error(y_train, y_pred_train):.4f} V")
    print(f"[Train]   Test  MAE : {mean_absolute_error(y_test,  y_pred_test):.4f} V")
    print(f"[Train]   Train R²  : {r2_score(y_train, y_pred_train):.4f}")
    print(f"[Train]   Test  R²  : {r2_score(y_test,  y_pred_test):.4f}")
    print(f"[Train] ────────────────────────────────────────\n")

    # Save model bundle
    bundle = {"model": model, "scaler": scaler, "n_lags": n_lags}
    with open(MODEL_PATH, "wb") as f:
        pickle.dump(bundle, f)
    print(f"[Train] Model saved to {MODEL_PATH}")

    return model, scaler, n_lags


def load_model() -> tuple:
    """Load saved model bundle from disk."""
    if not MODEL_PATH.exists():
        raise FileNotFoundError(
            f"No saved model found at {MODEL_PATH}. "
            "Run with 'train' or 'run' command first."
        )
    with open(MODEL_PATH, "rb") as f:
        bundle = pickle.load(f)
    print(f"[Model] Loaded from {MODEL_PATH} (n_lags={bundle['n_lags']})")
    return bundle["model"], bundle["scaler"], bundle["n_lags"]

# ── Inference Loop ─────────────────────────────────────────────────────────────

def infer_loop(conn: str, auth: tuple, model, scaler, n_lags: int):
    """
    Continuously query recent readings, predict voltage t+5s,
    and write predictions back to AnyLog every 0.5 seconds.

    Prediction timestamp = now + HORIZON_SECS so it aligns directly
    with the actual reading in grid_readings at that future timestamp.
    """
    feature_cols = get_feature_cols(n_lags)
    # Fetch a window of readings wide enough to build lag features
    window_secs = n_lags + HORIZON_SECS + 10  # extra buffer

    print(f"\n{'='*60}")
    print(f"[Infer] Starting inference loop ({INFER_HZ} Hz — every 0.5s)")
    print(f"[Infer] Reading from : {DBMS}.{SOURCE_TABLE}")
    print(f"[Infer] Writing to   : {DBMS}.{PRED_TABLE}")
    print(f"[Infer] Horizon      : {HORIZON_SECS} seconds ahead")
    print(f"[Infer] Press Ctrl+C to stop\n")

    total_predictions = 0
    total_errors = 0

    try:
        while True:
            t0 = time.monotonic()

            # ── 1. Fetch recent data ──────────────────────────────────────
            sql = (
                f"SELECT timestamp, sensor_id, voltage "
                f"FROM {SOURCE_TABLE} "
                f"WHERE timestamp >= NOW() - {window_secs} seconds "
                f"ORDER BY sensor_id, timestamp "
                f"LIMIT 10000"
            )
            df = anylog_query(conn, sql, auth)

            if df is None or df.empty:
                print(f"[Infer] No recent data — waiting...")
                time.sleep(1.0 / INFER_HZ)
                continue

            # ── 2. Build features ─────────────────────────────────────────
            df["timestamp"] = pd.to_datetime(df["timestamp"], utc=True, errors="coerce")
            df = df.sort_values(["sensor_id", "timestamp"])
            df["voltage"] = pd.to_numeric(df["voltage"], errors="coerce")

            predictions = []
            now_utc = datetime.now(timezone.utc)
            # Timestamp stored as now + 5s so it matches the actual reading
            # that will arrive in grid_readings at that future time
            predicted_time = now_utc + timedelta(seconds=HORIZON_SECS)

            for sensor_id, grp in df.groupby("sensor_id"):
                grp = grp.reset_index(drop=True)

                if len(grp) < n_lags + 1:
                    continue  # not enough history for this sensor yet

                # Use the most recent n_lags readings as features
                recent_voltages = grp["voltage"].dropna().values
                if len(recent_voltages) < n_lags:
                    continue

                lag_values = recent_voltages[-n_lags:][::-1]  # lag_1 = most recent
                X = scaler.transform([lag_values])
                predicted_v = float(model.predict(X)[0])

                # Clamp to physically plausible range (typical grid: 100–500 V)
                predicted_v = float(np.clip(predicted_v, 0.0, 1000.0))

                predictions.append({
                    "timestamp": predicted_time.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z",
                    "sensor_id": str(sensor_id).strip(),
                    "predicted_voltage": round(predicted_v, 4),
                    "horizon_seconds": HORIZON_SECS,
                    "n_lags_used": n_lags,
                    "model": "linear_regression",
                })

            # ── 3. Write predictions to AnyLog ────────────────────────────
            if predictions:
                ok = anylog_put(conn, predictions, auth)
                if ok:
                    total_predictions += len(predictions)
                    print(f"[Infer] {now_utc.strftime('%H:%M:%S')} — "
                          f"predicted {len(predictions)} sensor(s) | "
                          f"total: {total_predictions:,}")
                else:
                    total_errors += 1
                    if total_errors % 10 == 1:
                        print(f"[Infer] Warning: {total_errors} failed PUT(s) so far")
            else:
                print(f"[Infer] No predictions this cycle (insufficient lag data)")

            # ── 4. Rate limit ─────────────────────────────────────────────
            elapsed = time.monotonic() - t0
            sleep_t = (1.0 / INFER_HZ) - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)

    except KeyboardInterrupt:
        print(f"\n[Infer] Stopped.")
        print(f"[Infer] Total predictions written : {total_predictions:,}")
        print(f"[Infer] Total PUT failures        : {total_errors}")

# ── CLI ────────────────────────────────────────────────────────────────────────

def parse_conn(value: str) -> tuple[str, tuple]:
    """Parse 'user:pass@host:port' or 'host:port' into (conn, auth)."""
    if "@" in value:
        creds, conn = value.rsplit("@", 1)
        parts = creds.split(":", 1)
        auth = (parts[0], parts[1] if len(parts) > 1 else "")
    else:
        conn, auth = value, ()
    return conn, auth


def main():
    parser = argparse.ArgumentParser(
        description="EADS Voltage Predictor — Linear Regression Baseline"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # Shared args factory
    def add_common(p):
        p.add_argument("--conn", default="127.0.0.1:32149",
                       help="AnyLog REST endpoint (default: 127.0.0.1:32149)")
        p.add_argument("--lags", type=int, default=DEFAULT_LAGS,
                       help=f"Number of lag readings used as features (default: {DEFAULT_LAGS})")

    # run: train then infer
    p_run = sub.add_parser("run", help="Train model then start inference loop")
    add_common(p_run)
    p_run.add_argument("--train-hours", type=int, default=24,
                       help="Hours of historical data to train on (default: 24)")

    # train only
    p_train = sub.add_parser("train", help="Train and save model, then exit")
    add_common(p_train)
    p_train.add_argument("--train-hours", type=int, default=24,
                         help="Hours of historical data to train on (default: 24)")

    # infer only
    p_infer = sub.add_parser("infer", help="Load saved model and run inference loop")
    add_common(p_infer)

    args = parser.parse_args()
    conn, auth = parse_conn(args.conn)

    if args.command == "run":
        model, scaler, n_lags = train(conn, auth, args.train_hours, args.lags)
        infer_loop(conn, auth, model, scaler, n_lags)

    elif args.command == "train":
        train(conn, auth, args.train_hours, args.lags)

    elif args.command == "infer":
        model, scaler, n_lags = load_model()
        infer_loop(conn, auth, model, scaler, n_lags)


if __name__ == "__main__":
    main()