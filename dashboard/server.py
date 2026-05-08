import statistics
from flask import Flask, jsonify, render_template
import requests

app = Flask(__name__)

ANYLOG_URL = "http://127.0.0.1:32149"
DBMS = "eads"
TABLE = "voltage_calibrated"
SAMPLE_RATE_HZ = 7680


def anylog_query(sql: str) -> list[dict]:
    headers = {
        "command": f'sql {DBMS} format=json "{sql}"',
        "User-Agent": "AnyLog/1.23",
    }
    r = requests.get(ANYLOG_URL, headers=headers, timeout=5)
    r.raise_for_status()
    return r.json().get("Query", [])


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/waveform")
def waveform():
    rows = anylog_query(
        f"SELECT timestamp, timestamp_us, voltage_est FROM {TABLE} "
        f"ORDER BY timestamp DESC LIMIT 1000"
    )
    rows.sort(key=lambda r: r["timestamp_us"])
    return jsonify(rows)


@app.route("/api/stats")
def stats():
    rows = anylog_query(
        f"SELECT timestamp_us, voltage_est, adc_centered_volts FROM {TABLE} "
        f"ORDER BY timestamp DESC LIMIT {SAMPLE_RATE_HZ}"
    )
    rows.sort(key=lambda r: r["timestamp_us"])
    if not rows:
        return jsonify({"error": "no data"})

    voltages = [float(r["voltage_est"]) for r in rows]
    centered = [float(r["adc_centered_volts"]) for r in rows]

    # Estimate AC frequency from zero crossings over the sample window
    crossings = sum(
        1 for i in range(1, len(centered)) if centered[i - 1] * centered[i] < 0
    )
    n_seconds = len(centered) / SAMPLE_RATE_HZ
    freq = crossings / (2 * n_seconds) if n_seconds > 0 else 0

    return jsonify({
        "current": round(voltages[-1], 2),
        "min": round(min(voltages), 2),
        "max": round(max(voltages), 2),
        "avg": round(statistics.mean(voltages), 2),
        "frequency_hz": round(freq, 1),
    })


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8050, debug=False)
