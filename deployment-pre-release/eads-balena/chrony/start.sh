#!/bin/bash
echo "[chrony] Starting time synchronization..."

# Wait for gpsd to be ready
echo "[chrony] Waiting for gpsd..."
sleep 5

# Start chronyd in foreground
exec chronyd -d -f /etc/chrony/chrony.conf
