FROM python:3.11-slim

WORKDIR /app

COPY fastapi-generator/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy the generator module from the merged data-generator directory
COPY data-generator/eads_data_generator.py .

# Copy the dataset into /data (matches default CSV_PATH)
COPY data-generator/power_system_multiclass_anomaly_data.csv /data/

# Copy the FastAPI wrapper
COPY fastapi-generator/app/ ./app/

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
