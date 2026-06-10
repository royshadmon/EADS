# EADS Data Generator

Streams calibrated 7680 Hz voltage data to AnyLog via REST PUT.

## Dataset

**eads_7680hz_ch1_voltage_calibrated.csv** — High-frequency single-channel voltage measurements captured at 7680 Hz.

### Fields (5 total)
- `timestamp_us` - Microsecond epoch timestamp from data acquisition hardware
- `raw_count` - Raw ADC integer count
- `adc_input_volts` - ADC input voltage (V)
- `adc_centered_volts` - ADC centered (zero-offset) voltage (V)
- `voltage_est` - Estimated AC voltage (V)

## Prerequisites

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Usage

### View Sample Data

```bash
python eads_data_generator.py sample --csv eads_7680hz_ch1_voltage_calibrated.csv --rows 5
```

### Send One Batch

```bash
python eads_data_generator.py send --csv eads_7680hz_ch1_voltage_calibrated.csv --conn 127.0.0.1:32149 --rows 100
```

### Stream Continuously at 8 kHz

Stream the dataset continuously at 8000 rows/sec (loops back to start when reaching end):

```bash
python eads_data_generator.py stream \
  --csv eads_7680hz_ch1_voltage_calibrated.csv \
  --conn 127.0.0.1:32149 \
  --batch-size 400 \
  --rate 20 \
  --mode file
```

**Parameters:**
- `--csv`: Path to CSV file (required)
- `--conn`: AnyLog REST endpoint (default: 127.0.0.1:32149)
- `--batch-size`: Rows per HTTP request (default: 100)
- `--rate`: Batches per second (default: 10)
- `--mode`: AnyLog mode — `file` for immediate write, `streaming` for buffered (default: file)
- `--use-dataset-time`: Use original `timestamp_us` values converted to ISO format instead of current time (default: use current time)

**Effective data rate:**
Rows/sec = batch_size × rate

Examples:
- 400 rows × 20 batches/sec = **8000 rows/sec (8 kHz)**
- 100 rows × 10 batches/sec = 1000 rows/sec (1 kHz)

**Timestamps:**
By default, the generator uses **current timestamps** (real-time mode) to simulate live sensor data. To replay the original acquisition timestamps (derived from `timestamp_us`), add the `--use-dataset-time` flag.

## AnyLog Setup

Data is written to the `voltage_calibrated` table in the `eads` database.

**Verify data via AnyLog REST API:**

```bash
# List tables
curl -s "http://127.0.0.1:32149" -H "command: get tables where dbms = eads" -H "User-Agent: AnyLog/1.23"

# Count rows
curl -s "http://127.0.0.1:32149" -H 'command: sql eads format = table "select count(*) from voltage_calibrated"' -H "User-Agent: AnyLog/1.23"
```

**Verify directly in PostgreSQL:**

```bash
# Check row count
docker exec postgres1 psql -U demo -d eads -c "SELECT COUNT(*) FROM voltage_calibrated;"

# View sample data
docker exec postgres1 psql -U demo -d eads -c "SELECT timestamp, voltage_est, adc_centered_volts FROM voltage_calibrated ORDER BY timestamp DESC LIMIT 10;"
```
