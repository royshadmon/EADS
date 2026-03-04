.PHONY: build up down logs status

# Build all generator images
build:
	docker compose build

# Start all generators
up:
	docker compose up -d --build

# Stop all generators
down:
	docker compose down

# Tail logs for all generators
logs:
	docker compose logs -f

# Check health/status of each running generator
status:
	@echo "--- Generator 1 ---" && curl -s http://localhost:8001/status | python3 -m json.tool 2>/dev/null || echo "not reachable"
	@echo "--- Generator 2 ---" && curl -s http://localhost:8002/status | python3 -m json.tool 2>/dev/null || echo "not reachable"
	@echo "--- Generator 3 ---" && curl -s http://localhost:8003/status | python3 -m json.tool 2>/dev/null || echo "not reachable"

# ---- Run locally (no Docker) ----
# Override env vars as needed, e.g.:
#   ANYLOG_CONN=127.0.0.1:32149 CSV_PATH=./power_system_multiclass_anomaly_data.csv make run-local
run-local:
	PYTHONPATH=. uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
