# EADS - Electrical Anomaly Detection System

Real-time power grid sensor data streaming platform using AnyLog for distributed edge data storage.

---

## Quick Start

### Prerequisites

- Docker and Docker Compose
- Python 3.9+
- PowerGridSense dataset ([download from Kaggle](https://www.kaggle.com/datasets/ziya07/powergridsense-dataset))

### 1. Make sure you have an instance of multi-node AnyLog running

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

### 4. Verify Data

```bash
# Watch row count
watch -n 2 'docker exec postgres1 psql -U demo -d eads -c "SELECT COUNT(*) FROM grid_readings;"'
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

**See [data-generator/README.md](data-generator/README.md) for detailed troubleshooting**

