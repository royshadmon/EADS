# EADS

## Pre-requisites
```bash 
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

---

## Setup

### Step 1 — Add generator files

Add Svante's generator files to this root directory.
- `eads_data_generator.py`
- `power_system_multiclass_anomaly_data.csv`

### Step 2 — Configure Docker connections

Check which ports your containers are running on:

```bash
docker ps
```

Then open `docker-compose.yml` and update the `ANYLOG_CONN` variables for each generator to match the **REST ports** shown in the output above.

### Step 3 — Start the stack

```bash
make up
```

Then follow the logs to confirm everything is running:

```bash
make logs
```

---