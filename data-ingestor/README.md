# EADS Data Ingestor

Reads live ADC samples from the hardware sampling socket, applies voltage calibration, and streams to AnyLog via REST PUT.

## How It Works

The hardware sampling process (from `eads-software-main`) reads a MCP3208 12-bit ADC via SPI at ~8000 Hz and streams samples over a Unix domain socket at `/var/sampling/samples.sock`. Each line has the format:

```
timestamp_us,channel,raw_count
```

The ingestor connects to this socket, filters channel 1, applies fixed ADC calibration, batches the results, and sends them to AnyLog.

## Calibration

Calibration constants are derived from a reference hardware capture and hardcoded:

| Constant | Value | Description |
|---|---|---|
| `ADC_REF_VOLTS` | 3.3 V | ADC reference voltage |
| `ADC_COUNTS` | 4096 | 12-bit ADC range |
| `ADC_MID_VOLTS` | 1.692297 V | ADC midpoint (zero offset) |
| `ADC_SCALE` | 281.793554 | Scale factor to map to AC voltage |

Each sample produces these fields:
- `timestamp_us` — original microsecond timestamp from the hardware
- `raw_count` — raw 12-bit ADC value
- `adc_input_volts` — raw ADC voltage
- `adc_centered_volts` — zero-centered ADC voltage
- `voltage_est` — estimated AC voltage (V)

## Prerequisites

```bash
pip install -r requirements.txt
```

Ensure the hardware sampling process is running so the socket exists at `/var/sampling/samples.sock`.

## Usage

```bash
python3 eads_data_ingestor.py --conn 127.0.0.1:32149
```

**Parameters:**
- `--socket` — Unix socket path (default: `/var/sampling/samples.sock`)
- `--conn` — AnyLog REST endpoint (default: `127.0.0.1:32149`)
- `--batch-size` — Samples per HTTP request (default: 100)
- `--mode` — AnyLog ingestion mode: `file` for immediate write, `streaming` for buffered (default: `file`)

## AnyLog Setup

Data is written to the `voltage_calibrated` table in the `eads` database.

**Verify data via AnyLog REST API:**

```bash
# Count rows
curl -s "http://127.0.0.1:32149" -H 'command: sql eads format=json "select count(*) from voltage_calibrated"' -H "User-Agent: AnyLog/1.23"
```

**Verify directly in PostgreSQL:**

```bash
docker exec postgres1 psql -U demo -d eads -c "SELECT timestamp, voltage_est FROM voltage_calibrated ORDER BY timestamp DESC LIMIT 10;"
```
