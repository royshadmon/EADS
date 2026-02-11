# EADS Data Generator

Generates mock electrical sensor data (60 Hz AC sine wave) and pushes it to AnyLog via REST PUT, REST POST, or gRPC.

## Prerequisites

From the repo root (`EADS/`):

```bash
cd data-generator
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Start AnyLog

AnyLog runs via the [AnyLog docker-compose repo](https://github.com/AnyLog-co/docker-compose) (cloned separately, not part of this repo). From that repo's root:

```bash
make up ANYLOG_TYPE=anylog-standalone
```

Attach to the AnyLog CLI (in a separate terminal):

```bash
make attach ANYLOG_TYPE=anylog-standalone
```

## AnyLog Setup (required after every container restart)

Run these commands in the AnyLog CLI:

```
connect dbms almgm where type = sqlite
connect dbms blockchain where type = sqlite
connect dbms eads where type = sqlite
```

Get the operator policy ID:

```
blockchain get operator
```

Start the operator (replace the policy ID if yours differs):

```
run operator where policy = c6daa7a3063b24da81b30465271f7545 and create_table = true and update_tsd_info = true and compress_json = true and compress_sql = true
```

Verify everything is running:

```
get processes
get databases
```

---

## Test 1: REST PUT

No additional AnyLog setup needed beyond the base setup above.

**Terminal:**

```bash
# One-shot (1000 readings)
python eads_data_generator.py rest-put --conn 127.0.0.1:32149 --rows 1000 --hz 1000

# Continuous streaming at 1kHz
python eads_data_generator.py rest-put --conn 127.0.0.1:32149 --rows 1000 --hz 1000 --continuous

# Use file mode for immediate writes (no 60s buffer wait)
python eads_data_generator.py rest-put --conn 127.0.0.1:32149 --rows 10 --hz 1000 --mode file
```

**Verify in AnyLog CLI:**

```
sql eads format = table "select * from voltage_readings order by timestamp desc limit 10"
```

Note: In streaming mode (default), data buffers for up to 60 seconds or 10KB before flushing. Use `--mode file` for immediate writes, or check buffer status with `get streaming`.

**Result:** Working. Tested at 1kHz continuous (~1006 Hz effective rate).

---

## Test 2: REST POST (topic-based with MQTT mapping)

Requires an additional `run msg client` command on AnyLog before sending data.

**AnyLog CLI** (run this AFTER the base setup):

```
run msg client where broker = rest and user-agent = anylog and topic = (name = eads-sensors and dbms = "bring [dbms]" and table = "bring [table]" and column.timestamp.timestamp = "bring [timestamp]" and column.voltage.float = "bring [voltage]" and column.sensor_id.str = "bring [sensor_id]" and column.latitude.float = "bring [latitude]" and column.longitude.float = "bring [longitude]")
```

> **Important:** The command is `run msg client`, NOT `run mqtt client`. The latter gives "Unrecognized Command".

**Terminal:**

```bash
# One-shot (10 readings)
python eads_data_generator.py rest-post --conn 127.0.0.1:32149 --topic eads-sensors --rows 10

# Continuous streaming at 1kHz
python eads_data_generator.py rest-post --conn 127.0.0.1:32149 --topic eads-sensors --rows 1000 --hz 1000 --continuous
```

**Verify in AnyLog CLI:**

```
sql eads format = table "select * from voltage_readings order by timestamp desc limit 10"
```

**Result:** Working. Data ingested successfully via topic-based mapping.

---

## Test 3: gRPC (potetntial AnyLog bug - fails)

Our script runs a gRPC server. AnyLog connects to it as a client.

**Terminal 1 — Start gRPC server:**

```bash
python eads_data_generator.py grpc-serve --port 50055 --hz 1000
```

**AnyLog CLI — Connect to gRPC server:**

```
run grpc client where name = eads_sensors and ip = host.docker.internal and port = 50055 and grpc_dir = /tmp and proto = sensor_data and function = GetSensorData and request = SensorRequest and response = SensorDataResponse and service = SensorService and dbms = eads and table = voltage_readings
```

**Check error log:**

```
get error log
```

**Result:** Fails with AnyLog internal bug:

```
TypeError: get_one_value() takes 2 positional arguments but 3 were given
```

This is a potential bug in AnyLog's `run grpc client` handler. The gRPC server itself works correctly (verified with a standalone Python gRPC client). The bug crashes AnyLog and may make the container unresponsive, requiring a restart from the repo root:

```bash
# From the AnyLog docker-compose repo
make down ANYLOG_TYPE=anylog-standalone
make up ANYLOG_TYPE=anylog-standalone
```

---

## Test 4: Sample Data (no network)

```bash
# Print 5 sample readings
python eads_data_generator.py sample --rows 5

# Print 20 readings at 8kHz sample rate
python eads_data_generator.py sample --rows 20 --hz 8000
```

---

## Useful AnyLog CLI Commands

```
get connections          # Show TCP/REST/broker endpoints
get processes            # Show running processes (operator, streamer, etc.)
get databases            # Show connected databases
get streaming            # Show streaming buffer status
get error log            # Show recent errors
get operator             # Show operator stats (rows inserted, etc.)
blockchain get operator  # Show operator policy (includes policy ID)

sql eads format = table "select count(*) from voltage_readings"
sql eads format = table "select * from voltage_readings order by timestamp desc limit 10"
```

---

## AnyLog Config Changes (already applied)

These changes in the AnyLog docker-compose config files were required for Docker on macOS. The files are in the [AnyLog docker-compose repo](https://github.com/AnyLog-co/docker-compose) under `docker-makefiles/anylog-standalone/`:

**`base_configs.env`:**
- `TCP_BIND=false` — allows Docker port mapping to work
- `REST_BIND=false` — allows Docker port mapping to work

**`advance_configs.env`:**
- `NIC_TYPE=""` — empty string instead of `"lo"` (loopback binds only inside container)

---

## Test Summary

| Method | Status | Notes |
|--------|--------|-------|
| REST PUT | Working | Tested at 1kHz continuous streaming |
| REST POST | Working | Uses `run msg client` with `broker = rest` for topic mapping |
| gRPC | AnyLog Bug | `TypeError: get_one_value()` — Possible AnyLog internal issue |
| Sample | Working | Prints data to stdout, no network needed |

