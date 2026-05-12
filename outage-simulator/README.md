# EADS Outage Proxy

A lightweight FastAPI proxy that intercepts data flowing from the EADS
generators to AnyLog and lets you simulate per-operator power outages from
the terminal — without touching the generator or AnyLog configuration.

---

## Prerequisites

- Docker and Docker Compose installed
- This branch requires files from the fastapi branch. Before using this branch,
  merge from fastapi to ensure the following are present:

  ```bash
  git merge fastapi
  ```

  This will bring in:
  - `eads_data_generator.py`
  - `app/` (FastAPI wrapper)
  - `Dockerfile` (generator image)
  - `requirements.txt` (generator deps)
  - `power_system_multiclass_anomaly_data.csv`

- Three AnyLog operator nodes must already be running and reachable on:
  - `:32149` (operator 1)
  - `:32249` (operator 2)
  - `:32349` (operator 3)

---

## Architecture

```
                  ┌──────────────────────────────────────────┐
  Generator-1 ───►  Proxy :9001  ──────────────────────────►  AnyLog Op-1 :32149
  Generator-2 ───►  Proxy :9002  ──────────────────────────►  AnyLog Op-2 :32249
  Generator-3 ───►  Proxy :9003  ──────────────────────────►  AnyLog Op-3 :32349
                  └──────────────────────────────────────────┘
                         ▲
                   outage_cli.sh
                   (your terminal)
```

**The only change to the existing setup** is that each generator's
`ANYLOG_CONN` environment variable now points at the proxy instead of
AnyLog directly. Everything else is untouched.

---

## Outage State Machine

```
  NORMAL ──[ POST /sim-power-outage ]──► SPIKE (N seconds) ──► OUTAGE
  OUTAGE ──[ POST /end-power-outage ]──► NORMAL
```

| Phase      | Behaviour |
|------------|-----------|
| **NORMAL** | Real readings forwarded unchanged |
| **SPIKE**  | voltage_est overwritten with extreme values (250–350 V) |
| **OUTAGE** | All values zeroed out |

Outages are **per-operator** — triggering one on proxy-1 has no effect on
proxy-2 or proxy-3.

---

## Data Table Reference

The data generator writes calibrated voltage data to the `voltage_calibrated` table in AnyLog.

To query recent data:
```bash
# Recent voltage readings
curl -X GET http://127.0.0.1:32149 \
  -H "command: sql eads format=table \"select * from voltage_calibrated order by timestamp desc limit 10\"" \
  -H "User-Agent: AnyLog/1.23"
```

---

## Files Added / Changed

| File | Status | Description |
|------|--------|-------------|
| `outage_proxy.py` | **New** | The proxy FastAPI app |
| `Dockerfile.proxy` | **New** | Container image for the proxy |
| `requirements.proxy.txt` | **New** | Proxy-only dependencies |
| `outage_cli.sh` | **New** | Terminal control helper |
| `docker-compose.yml` | **Modified** | Adds proxy services; changes `ANYLOG_CONN` on generators |

---

## Quickstart

### 1 — Build and start everything

```bash
docker compose up --build -d
```

### 2 — Verify proxies are healthy

```bash
./outage_cli.sh all status
# or manually:
curl http://127.0.0.1:9001/status
curl http://127.0.0.1:9002/status
curl http://127.0.0.1:9003/status
```

### 3 — Trigger an outage on operator 1

```bash
./outage_cli.sh 1 start
```

The proxy will:
1. Immediately start overwriting readings with high voltage_est spike values
   for 3 seconds.
2. Then switch to zeroing all values.

### 4 — End the outage

```bash
./outage_cli.sh 1 stop
```

Real data flows to AnyLog again immediately.

---

## CLI Reference

```
Usage: ./outage_cli.sh <operator> <command>

Operators:
  1        proxy on port 9001  (AnyLog operator 1)
  2        proxy on port 9002  (AnyLog operator 2)
  3        proxy on port 9003  (AnyLog operator 3)
  all      applies command to all three

Commands:
  start    trigger spike → outage
  stop     end outage, resume normal forwarding
  status   show current proxy state
  health   liveness check

Examples:
  ./outage_cli.sh 1 start       # outage on operator 1
  ./outage_cli.sh 2 stop        # end outage on operator 2
  ./outage_cli.sh all status    # check all proxies
```

Make the script executable if needed:

```bash
chmod +x outage_cli.sh
```

---

## Direct curl Commands

If you prefer raw curl over the CLI helper:

```bash
# Start outage on operator 2
curl -X POST http://127.0.0.1:9002/sim-power-outage

# End outage on operator 2
curl -X POST http://127.0.0.1:9002/end-power-outage

# Check status
curl http://127.0.0.1:9002/status
```

---

## Environment Variables (per proxy container)

| Variable | Default | Description |
|----------|---------|-------------|
| `ANYLOG_URL` | `http://127.0.0.1:32149` | Target AnyLog operator URL |
| `OPERATOR_LABEL` | `operator-1` | Label used in logs |
| `SPIKE_DURATION_SEC` | `3` | Seconds in the spike phase before flipping to zeros |

---

## Running the Proxy Without Docker

```bash
pip install -r requirements.proxy.txt

# Operator 1
ANYLOG_URL=http://127.0.0.1:32149 OPERATOR_LABEL=operator-1 \
    uvicorn outage_proxy:app --port 9001

# Operator 2  (separate terminal)
ANYLOG_URL=http://127.0.0.1:32249 OPERATOR_LABEL=operator-2 \
    uvicorn outage_proxy:app --port 9002

# Operator 3  (separate terminal)
ANYLOG_URL=http://127.0.0.1:32349 OPERATOR_LABEL=operator-3 \
    uvicorn outage_proxy:app --port 9003
```

Then set `ANYLOG_CONN` on each generator to point at the matching proxy port
(`127.0.0.1:9001`, `127.0.0.1:9002`, `127.0.0.1:9003`).

---

## Interactive Docs

Each running proxy exposes Swagger UI at:

```
http://127.0.0.1:9001/docs
http://127.0.0.1:9002/docs
http://127.0.0.1:9003/docs
```