# EADS Voltage Predictor

Real-time voltage forecasting for the PowerGridSense dataset, built on AnyLog as the data backbone.

---

## ML Model

- **Algorithm:** Scikit-learn `LinearRegression` with `StandardScaler` normalization
- **Task:** Predicts each sensor's voltage **5 seconds into the future**
- **Features:** 10 lag readings per sensor — `lag_1` (most recent voltage) through `lag_10` (10 readings back), forming a sliding window over recent signal history
- **Target:** `voltage` at `t + 5s`, derived by shifting the voltage series forward by 5 rows during training
- **Training data:** Fetched live from `eads.grid_readings` via AnyLog REST GET
- **Train/test split:** 80/20 chronological split — test set is always the most recent 20% of data
- **Evaluation metrics:** MAE and R² reported on both train and test sets at the end of every training run
- **Persistence:** Model and scaler saved together as `eads_voltage_model.pkl`, allowing inference to run independently of training
- **Inference rate:** Runs at 1 Hz — queries the last ~25 seconds of readings per sensor, builds the lag vector, and writes a prediction
- **Output clamp:** Predictions are clipped to `[0, 1000]` volts to discard physically implausible outputs

---

## Architecture

```
PowerGridSense CSV
       │
       ▼
eads_data_generator.py  ──── REST PUT ────►  AnyLog Operator1 (32149)
                                                      │
                                               eads.grid_readings
                                                      │
                                    ◄── REST GET ─────┘
                                    │
                          eads_voltage_predictor.py
                          (train → infer loop)
                                    │
                          ──── REST PUT ────►  AnyLog Operator1 (32149)
                                                      │
                                           eads.voltage_predictions
```

---

## Prerequisites

- Python 3.11+
- AnyLog operator1 running at `127.0.0.1:32149`
- PowerGridSense CSV: [kaggle.com/datasets/ziya07/powergridsense-dataset](https://www.kaggle.com/datasets/ziya07/powergridsense-dataset)

```bash
pip install scikit-learn pandas numpy requests
```

---

## Usage

### 1. Start the data generator
```bash
python eads_data_generator.py stream \
  --csv power_system_multiclass_anomaly_data.csv \
  --conn 127.0.0.1:32149
```

### 2. Train then run inference
```bash
python eads_voltage_predictor.py run \
  --conn 127.0.0.1:32149 \
  --train-hours 1
```

### 3. Train only (saves model to disk)
```bash
python eads_voltage_predictor.py train \
  --conn 127.0.0.1:32149 \
  --train-hours 1
```

### 4. Inference only (loads saved model)
```bash
python eads_voltage_predictor.py infer \
  --conn 127.0.0.1:32149
```

### CLI flags

| Flag | Default | Description |
|---|---|---|
| `--conn` | `127.0.0.1:32449` | AnyLog REST endpoint — override to `127.0.0.1:32149` |
| `--train-hours` | `24` | Hours of history to train on — `1` recommended |
| `--lags` | `10` | Number of lag features |

---

## AnyLog Setup

Raise the query volume limit on the operator1 CLI before training:

```
set query mode using max_volume = 100MB and timeout = 120 seconds
```

---

## Verifying Predictions

Check predictions are being written:
```
sql eads format = table "SELECT timestamp, sensor_id, predicted_voltage FROM voltage_predictions ORDER BY timestamp DESC LIMIT 10"
```

Compare a prediction to its actual reading for a specific sensor:
```
sql eads format = table "SELECT timestamp, sensor_id, predicted_voltage FROM voltage_predictions WHERE sensor_id = 'S5' ORDER BY timestamp DESC LIMIT 5"

sql eads format = table "SELECT timestamp, sensor_id, voltage FROM grid_readings WHERE sensor_id = 'S5' ORDER BY timestamp DESC LIMIT 5"
```

> The prediction `timestamp` is when the forecast was made — the matching actual reading will appear ~5 seconds later in `grid_readings`.

---

## Output Schema

Predictions are written to `eads.voltage_predictions`:

| Field | Type | Description |
|---|---|---|
| `timestamp` | timestamp | When the prediction was made |
| `sensor_id` | char(2) | Sensor identifier (e.g. `S5`) |
| `predicted_voltage` | decimal | Forecasted voltage in volts |
| `horizon_seconds` | int | Always `5` — seconds ahead predicted |
| `n_lags_used` | int | Number of lag features used |
| `model` | varchar | Always `linear_regression` |