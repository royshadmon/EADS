# EADS - Electrical Anomaly Detection System

Real-time power grid sensor data streaming platform using AnyLog for distributed edge data storage.

---

## Quick Start

### Prerequisites

- Docker and Docker Compose
- Python 3.9+
- PowerGridSense dataset ([download from Kaggle](https://www.kaggle.com/datasets/ziya07/powergridsense-dataset))
- A running multi-node AnyLog cluster (master, 3 operators, query node) with PostgreSQL and MongoDB containers

### 1. Make sure you have an instance of multi-node AnyLog running

Once the AnyLog cluster is up (master, operator1–3, query), create the `eads` database on each PostgreSQL container and connect it inside each operator.

```bash
# Create the eads database on each Postgres instance
docker exec postgres1 psql -U demo -d template1 -c "CREATE DATABASE eads;"
docker exec postgres2 psql -U demo -d template1 -c "CREATE DATABASE eads;"
docker exec postgres3 psql -U demo -d template1 -c "CREATE DATABASE eads;"
```

Then attach to each operator and connect the database (detach with Ctrl+P, Ctrl+Q after each):

```bash
docker attach operator1
# At the AL operator1 +> prompt:
connect dbms eads where type=psql and user=demo and password=passwd and ip=127.0.0.1 and port=5432

docker attach operator2
# At the AL operator2 +> prompt:
connect dbms eads where type=psql and user=demo and password=passwd and ip=127.0.0.1 and port=5433

docker attach operator3
# At the AL operator3 +> prompt:
connect dbms eads where type=psql and user=demo and password=passwd and ip=127.0.0.1 and port=5434
```

### 2. Start Data Generator

```bash
cd data-generator
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

# Stream at 8 kHz (8000 rows/sec)
python eads_data_generator.py stream \
  --csv power_system_multiclass_anomaly_data.csv \
  --conn 127.0.0.1:32149 \
  --batch-size 400 \
  --rate 20 \
  --mode file
```

### 3. Start Outage Simulator

The outage proxies sit between generators and AnyLog, allowing you to simulate per-operator power outages.

```bash
cd outage-simulator
docker compose up -d --build
```

Verify all three proxies:

```bash
curl http://127.0.0.1:9001/status
curl http://127.0.0.1:9002/status
curl http://127.0.0.1:9003/status
```

### 4. Start FastAPI Generators

The containerized generators stream data through the outage proxies into AnyLog operators.

```bash
cd fastapi-generator
docker compose up -d --build
```

Verify streaming:

```bash
curl http://127.0.0.1:8001/status
curl http://127.0.0.1:8002/status
curl http://127.0.0.1:8003/status
```

### 5. Start Grafana

If this is your first time, create the `.env` file in the `grafana/` directory and input the following information:

```bash
GRAFANA_PORT=3000
GF_ADMIN_USER=admin
GF_ADMIN_PASSWORD=admin

PG1_HOST=host.docker.internal
PG1_PORT=5432
PG1_DB=eads
PG1_USER=demo
PG1_PASSWORD=passwd

PG2_HOST=host.docker.internal
PG2_PORT=5433
PG2_DB=eads
PG2_USER=demo
PG2_PASSWORD=passwd

PG3_HOST=host.docker.internal
PG3_PORT=5434
PG3_DB=eads
PG3_USER=demo
PG3_PASSWORD=passwd
```

> **Note:** `host.docker.internal` allows the Grafana container to reach localhost services on macOS/Windows. On Linux, use the actual host IP or add `extra_hosts` to the compose file.

Then start Grafana:

```bash
docker compose up -d
```

Open http://localhost:3000 and log in with `admin` / `admin`. Use the **operator_ds** dropdown to switch between operator datasources.

If Grafana shows "password authentication failed", add a trust rule to each Postgres:

```bash
docker exec postgres1 sh -c "echo 'host all all 0.0.0.0/0 trust' >> /var/lib/postgresql/data/pg_hba.conf"
docker exec postgres2 sh -c "echo 'host all all 0.0.0.0/0 trust' >> /var/lib/postgresql/data/pg_hba.conf"
docker exec postgres3 sh -c "echo 'host all all 0.0.0.0/0 trust' >> /var/lib/postgresql/data/pg_hba.conf"
docker restart postgres1 postgres2 postgres3
```

### 6. Run Voltage Predictor

```bash
cd voltage-predictor
pip install -r requirements.txt
python3 eads_voltage_predictor.py run --conn 127.0.0.1:32149
```

### 7. Verify Data

```bash
# Watch row count
watch -n 2 'docker exec postgres1 psql -U demo -d eads -c "SELECT COUNT(*) FROM voltage_calibrated;"'
```

---

## Components

### Data Generator
Streams PowerGridSense dataset to AnyLog operators.
- **Location:** `data-generator/`
- **Documentation:** See [data-generator/README.md](data-generator/README.md) for detailed usage

### FastAPI Service
Containerized data generators that stream through outage proxies to AnyLog operators. Runs 3 generators for multi-node setups.
- **Location:** `fastapi-generator/`
- **Documentation:** See [fastapi-generator/README.md](fastapi-generator/README.md) for detailed usage

### Outage Proxy
Intercepts data between generators and AnyLog operators to simulate per-operator power outages in real time.
- **Location:** `outage-proxy/`
- **Documentation:** See [outage-proxy/README.md](outage-proxy/README.md) for detailed usage

#### Quickstart
```bash
# Build and start
docker compose up --build -d

# Check all proxies are healthy
./outage_cli.sh all status
```

#### CLI Commands
```
./outage_cli.sh <1|2|3|all> <start|stop|status|health>

./outage_cli.sh 1 start       # trigger spike → outage on operator 1
./outage_cli.sh 1 stop        # end outage, resume normal forwarding
./outage_cli.sh all status    # check all proxies
```

### Grafana Dashboard
Multi-node visualization dashboard for monitoring all AnyLog operators.
- **Location:** `grafana/`

### Voltage Predictor
Predicts Future Voltages from recent readings and sends the predictions to Anylog
- **Location:** `voltage-predictor/`
- **Documentation:** See [voltage-predictor/README.md](voltage-predictor/README.md) for detailed usage

---

## Running Multiple Operators

Run separate generator instances for each operator:

```bash
# Terminal 1
python eads_data_generator.py stream --csv <dataset> --conn node1:32149 --batch-size 400 --rate 20

# Terminal 2
python eads_data_generator.py stream --csv <dataset> --conn node2:32149 --batch-size 400 --rate 20

# Terminal 3
python eads_data_generator.py stream --csv <dataset> --conn node3:32149 --batch-size 400 --rate 20
```

---

## Troubleshooting

**Data generator won't start:**
- Ensure dataset CSV is in `data-generator/` directory
- Check AnyLog is running: `docker ps | grep anylog`

**No data in database:**
- Check partitioned tables: `docker exec postgres1 psql -U demo -d eads -c "\dt"`
- Verify operator is running: `curl -s "http://127.0.0.1:32149" -H "command: get processes" -H "User-Agent: AnyLog/1.23"`

---

## Shutdown

Stop components in reverse order:

```bash
# Stop Voltage Predictor (Ctrl+C in its terminal)

# Stop FastAPI Generators
cd fastapi-generator
docker compose down

# Stop Outage Proxies
cd outage-simulator
docker compose down

# Stop Grafana
cd grafana
docker compose down
```