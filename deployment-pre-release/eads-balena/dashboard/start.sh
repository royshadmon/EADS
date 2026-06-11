#!/bin/bash
# EADS dashboard launcher. Port 8080 on the host network — reachable at
# http://<pi-lan-ip>:8080 from any browser on the same network, and at
# http://10.42.0.<N>:8080 over the Nebula overlay if the firewall allows it.
echo "[dashboard] EADS local dashboard starting on :${DASHBOARD_PORT:-8080}"
exec /app/dashboard "${DASHBOARD_PORT:-8080}"
