# EADS Data Generator

Generates mock electrical sensor data (60 Hz AC sine wave) and pushes it to AnyLog via REST PUT, REST POST, or gRPC.

## Directory Structure

This repo should be part of a larger setup with AnyLog and PostgreSQL infrastructure:

```
parent-directory/
├── EADS/                    (this GitHub repo)
│   └── data-generator/      
├── docker-compose/          (AnyLog docker-compose - not in repo)
└── EdgeFL/                  (PostgreSQL setup - not in repo)
```

## Prerequisites

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Start PostgreSQL

Start the PostgreSQL container from the EdgeFL repo (cloned outside this repo, parallel to docker-compose):

```bash
cd ../../EdgeFL/EdgeLake/Postgres
make up NAME=postgres1 HOST_PORT=5432 VOLUME=pgdata1
```

Connect it to the AnyLog network (only needed once):

```bash
docker network connect docker-compose-files_default postgres1
```

## Start AnyLog

AnyLog runs via the [AnyLog docker-compose repo](https://github.com/AnyLog-co/docker-compose) (cloned outside this repo, parallel to EdgeFL):

```bash
make up ANYLOG_TYPE=anylog-standalone
```

Attach to the AnyLog CLI (in a separate terminal):

```bash
make attach ANYLOG_TYPE=anylog-standalone
```

## AnyLog Setup (required after every container restart)

The databases should auto-connect on startup. If they don't, manually connect in the AnyLog CLI:

```
connect dbms almgm where type = psql and ip = 172.18.0.3 and port = 5432 and user = demo and password = passwd
connect dbms blockchain where type = psql and ip = 172.18.0.3 and port = 5432 and user = demo and password = passwd
connect dbms eads where type = psql and ip = 172.18.0.3 and port = 5432 and user = demo and password = passwd
```

> **Note**: Replace `172.18.0.3` with your postgres1 container IP if different. Get it with: `docker inspect postgres1 -f '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}'`

Get the operator policy ID:

```
blockchain get operator
```

Start the operator (replace the policy ID if yours differs):

```
run operator where policy = 06f92b3660b7ff6ef8b7ba33488f0b31 and create_table = true and update_tsd_info = true and compress_json = true and compress_sql = true
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
sql eads format = table "select count(*) from voltage_readings"
sql eads format = table "select * from voltage_readings order by timestamp desc limit 10"
```

> **Note**: AnyLog creates partitioned tables (e.g., `par_voltage_readings_2026_02_01_d14_insert_timestamp`) for performance. If the main table query returns 0 rows, query the partition directly:
> ```
> get tables where dbms = eads
> sql eads format = table "select count(*) from par_voltage_readings_2026_02_01_d14_insert_timestamp"
> ```

**Verify directly in PostgreSQL:**

```bash
docker exec -it postgres1 psql -U demo -d eads -c "SELECT COUNT(*) FROM par_voltage_readings_2026_02_01_d14_insert_timestamp;"
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
get tables where dbms = eads
sql eads format = table "select count(*) from par_voltage_readings_2026_02_01_d14_insert_timestamp"
sql eads format = table "select * from par_voltage_readings_2026_02_01_d14_insert_timestamp order by timestamp desc limit 10"
```

> **Note**: Query the partitioned table (shown by `get tables`) for data verification.

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
- **PostgreSQL configuration:**
  - `DB_TYPE=psql` — use PostgreSQL instead of SQLite
  - `DB_USER=demo` — PostgreSQL username
  - `DB_PASSWD=passwd` — PostgreSQL password
  - `DB_IP=172.18.0.3` — postgres1 container IP on shared network
  - `DB_PORT=5432` — PostgreSQL port
  - `DEFAULT_DBMS=eads` — default database name

**`advance_configs.env`:**
- `NIC_TYPE=""` — empty string instead of `"lo"` (loopback binds only inside container)

**Docker Network Setup:**
- Both `postgres1` and `anylog-standalone` containers must be on the `docker-compose-files_default` network
- Connect postgres1 to the network: `docker network connect docker-compose-files_default postgres1`

---

## Test Summary

| Method | Status | Notes |
|--------|--------|-------|
| REST PUT | Working | Tested at 1kHz continuous streaming |
| REST POST | Working | Uses `run msg client` with `broker = rest` for topic mapping |
| gRPC | AnyLog Bug | `TypeError: get_one_value()` — Possible AnyLog internal issue |
| Sample | Working | Prints data to stdout, no network needed |
