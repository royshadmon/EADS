# EADS Data Generator - Sprint 2

Streams the PowerGridSense dataset to AnyLog via REST PUT.

## Dataset

**PowerGridSense**: Real-time smart power grid measurements with anomaly detection.

Download from: [https://www.kaggle.com/datasets/ziya07/powergridsense-dataset](https://www.kaggle.com/datasets/ziya07/powergridsense-dataset)

### Fields (9 total)
- `Timestamp` - Date and time of sensor reading
- `Sensor_ID` - Unique sensor identifier
- `Voltage (V)` - Voltage level in volts
- `Current (A)` - Current in amperes
- `Power (kW)` - Active power in kilowatts
- `Frequency (Hz)` - System frequency
- `Power_Factor` - Ratio of real to apparent power
- `Location` - Geographic/logical area
- `Anomaly_Label` - Multi-class label (0-4: Normal, Voltage, Frequency, Power Factor, Combined)

## Prerequisites

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Usage

### View Sample Data

```bash
python eads_data_generator.py sample --csv power_system_multiclass_anomaly_data.csv --rows 5
```

### Send One Batch

```bash
python eads_data_generator.py send --csv power_system_multiclass_anomaly_data.csv --conn 127.0.0.1:32149 --rows 100
```

### Stream Continuously at 8 kHz

Stream the dataset continuously at 8000 rows/sec (loops back to start when reaching end):

```bash
python eads_data_generator.py stream \
  --csv power_system_multiclass_anomaly_data.csv \
  --conn 127.0.0.1:32149 \
  --batch-size 400 \
  --rate 20 \
  --mode file
```

**Parameters:**
- `--csv`: Path to PowerGridSense CSV file (required)
- `--conn`: AnyLog REST endpoint (default: 127.0.0.1:32149)
- `--batch-size`: Rows per HTTP request (default: 100)
- `--rate`: Batches per second (default: 10)
- `--mode`: AnyLog mode - `file` for immediate write, `streaming` for buffered (default: file)

**Effective data rate:**
Rows/sec = batch_size × rate

Examples:
- 400 rows × 20 batches/sec = **8000 rows/sec (8 kHz)**
- 100 rows × 10 batches/sec = 1000 rows/sec (1 kHz)
- 200 rows × 40 batches/sec = 8000 rows/sec (8 kHz)

## AnyLog Setup

The data is sent to the `grid_readings` table in the `eads` database.

**Verify data via AnyLog REST API:**

```bash
# List tables
curl -s "http://127.0.0.1:32149" -H "command: get tables where dbms = eads" -H "User-Agent: AnyLog/1.23"

# Count rows
curl -s "http://127.0.0.1:32149" -H 'command: sql eads format = table "select count(*) from grid_readings"' -H "User-Agent: AnyLog/1.23"
```

**Verify directly in PostgreSQL:**

```bash
# Note: Data goes into partitioned tables (par_grid_readings_*)
docker exec postgres1 psql -U demo -d eads -c "SELECT COUNT(*) FROM par_grid_readings_2026_02_01_d14_insert_timestamp;"

# View sample data
docker exec postgres1 psql -U demo -d eads -c "SELECT timestamp, sensor_id, voltage, current, power, frequency FROM par_grid_readings_2026_02_01_d14_insert_timestamp ORDER BY timestamp DESC LIMIT 10;"
```

## Changes from Sprint 1

- Removed mock AC sine wave generation
- Removed gRPC support
- Removed REST POST/MQTT topic mapping
- Added real PowerGridSense dataset (10,000 rows from Kaggle)
- Added continuous streaming with infinite loop
- Included all 9 dataset fields (timestamp, sensor_id, voltage, current, power, frequency, power_factor, location, anomaly_label)
- Simplified to REST PUT only
- Changed table name from `voltage_readings` to `grid_readings`
- Configured for 8 kHz streaming (8000 rows/sec)
