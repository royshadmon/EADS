FROM python:3.12-slim

WORKDIR /app

COPY super-node/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt

# Copy the super-node app and shared repo components from the build context (repo root)
COPY super-node/app ./app
COPY anomaly_notification ./anomaly_notification
COPY user-dashboard ./user-dashboard

EXPOSE 8080

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
