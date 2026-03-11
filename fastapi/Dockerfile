FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy the original generator module
COPY eads_data_generator.py .

# Copy the dataset into /data (matches default CSV_PATH)
COPY power_system_multiclass_anomaly_data.csv /data/

# Copy the FastAPI wrapper
COPY app/ ./app/

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
