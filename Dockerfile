FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy the original generator module + proto/gRPC stubs into /app
# so `import eads_data_generator` works from the FastAPI code
COPY eads_data_generator.py .
COPY sensor_data.proto .
COPY sensor_data_pb2.py .
COPY sensor_data_pb2_grpc.py .

# Copy the FastAPI wrapper
COPY app/ ./app/

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
