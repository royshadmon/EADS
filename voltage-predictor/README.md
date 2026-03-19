# EADS Voltage Predictor

A real-time voltage forecasting service for the EADS pipeline.

This component trains a machine learning model on recent voltage readings stored in AnyLog, then continuously predicts future voltage values for each sensor and writes those predictions back into the system.

## Overview

The predictor uses a lag-based supervised learning approach:

- It queries recent voltage readings from `eads.grid_readings`
- It builds lag features from recent voltage history for each sensor
- It trains a `LinearRegression` model with standardized inputs
- It continuously performs inference on live data
- It writes predictions to `eads.voltage_predictions`

## Usage
```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```
Train on last 24 hours of data, then run inference loop:
```bash
python eads_voltage_predictor.py run --conn 127.0.0.1:32149
```
## Functional Flow

1. **Training**
   - Fetch historical voltage data from AnyLog
   - Build lag-based feature vectors
   - Train a regression model
   - Save the trained model and scaler to disk

2. **Inference**
   - Continuously query recent readings from AnyLog
   - Build the most recent lag vector for each sensor
   - Predict a future voltage value
   - Write predictions back to AnyLog

## Model Details

- **Model:** `LinearRegression`
- **Feature scaling:** `StandardScaler`
- **Input features:** most recent `n` lagged voltage readings
- **Default lags:** `100`
- **Prediction target:** voltage shifted forward by `HORIZON_SECS` rows
- **Configured horizon:** `5`
- **Inference loop rate:** `100 Hz`
- **Prediction output clamp:** `[0, 1000]`
