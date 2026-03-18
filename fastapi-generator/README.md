# EADS FastAPI Data Generator

Containerized FastAPI wrapper around the data generator that streams PowerGridSense data through the outage proxies to AnyLog operators.

## Architecture

```
Generator ──PUT──> Outage Proxy ──PUT──> AnyLog Operator
(8001)            (9001)                (32149)
(8002)            (9002)                (32249)
(8003)            (9003)                (32349)
```

---

## Prerequisites

- Docker and Docker Compose
- Outage proxies running (see `../outage-simulator/`)

The data generator module (`eads_data_generator.py`) and dataset (`power_system_multiclass_anomaly_data.csv`) are automatically included from `../data-generator/` during the Docker build.

---

## Quick Start

### Step 1 — Start the Outage Proxies

```bash
cd ../outage-simulator
docker compose up -d
```

Verify proxies are running:
```bash
curl -s http://localhost:9001/status | python3 -m json.tool
curl -s http://localhost:9002/status | python3 -m json.tool
curl -s http://localhost:9003/status | python3 -m json.tool
```

### Step 2 — Start the FastAPI Generators

```bash
cd ../fastapi-generator
make up
```

### Step 3 — Verify Streaming

```bash
make status
```

Or check individual generators:
```bash
curl -s http://localhost:8001/status | python3 -m json.tool
```

---

## Configuration

Environment variables (set in `docker-compose.yml`):

| Variable | Default | Description |
|----------|---------|-------------|
| `ANYLOG_CONN` | `127.0.0.1:9001` | Proxy host:port |
| `ANYLOG_MODE` | `file` | `file` (immediate) or `streaming` (buffered) |
| `CSV_PATH` | `/data/power_system_multiclass_anomaly_data.csv` | Dataset path inside container |
| `BATCH_SIZE` | `100` | Rows per HTTP request |
| `RATE_HZ` | `10` | Batches per second |

**Effective throughput:** `BATCH_SIZE × RATE_HZ` = 1000 rows/sec per generator

---

## Makefile Commands

| Command | Description |
|---------|-------------|
| `make up` | Build and start all generators |
| `make down` | Stop all generators |
| `make logs` | Tail logs for all generators |
| `make status` | Check health/status of each generator |
| `make build` | Build images without starting |

---

## Running Locally (No Docker)

```bash
# Create venv
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

# Run with PYTHONPATH pointing to data-generator
PYTHONPATH=../data-generator \
ANYLOG_CONN=127.0.0.1:9001 \
CSV_PATH=../data-generator/power_system_multiclass_anomaly_data.csv \
make run-local
```

---

## API Endpoints

Each generator exposes:

| Endpoint | Description |
|----------|-------------|
| `GET /health` | Health check (`{"status": "ok", "streaming": true}`) |
| `GET /status` | Full status with rows sent, errors, effective rate |

---

## Simulating Outages

Use the outage CLI to test fault tolerance:

```bash
cd ../outage-simulator

# Start outage on operator 1
./outage_cli.sh 1 start

# Check status
./outage_cli.sh all status

# Stop outage
./outage_cli.sh 1 stop
```

See `../outage-simulator/OUTAGE_PROXY_README.md` for details.

---

## Troubleshooting

**Generators can't connect:**
- Ensure outage proxies are running: `docker ps | grep proxy`
- Check proxy ports match `ANYLOG_CONN` in docker-compose.yml

**High error count in /status:**
- Check if AnyLog operators are running
- Verify proxy → AnyLog connectivity
