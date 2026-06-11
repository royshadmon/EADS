FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app.py .
# /pool  — identity pool exported from the MacBook (provision_pool.sh)
# /data  — assignments, registry, alert state (persistent volume)
# /tls   — enroll.crt + enroll.key
CMD ["python", "app.py"]
