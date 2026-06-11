#!/bin/bash
# EADS gpsd startup script
# Quectel GPS module on the HAT connects via UART

# Wait for UART device to be available
echo "[gpsd] Waiting for GPS device..."
for i in $(seq 1 30); do
    if [ -e /dev/ttyAMA0 ]; then
        GPS_DEV=/dev/ttyAMA0
        break
    elif [ -e /dev/ttyS0 ]; then
        GPS_DEV=/dev/ttyS0
        break
    fi
    sleep 1
done

if [ -z "$GPS_DEV" ]; then
    echo "[gpsd] No GPS device found, exiting"
    exit 1
fi

echo "[gpsd] Found GPS device: $GPS_DEV"

# Background: publish the fix for the fleet map + health rows.
/publish_fix.sh &

# Start gpsd
# -n: don't wait for client to connect before polling
# -G: listen on all interfaces
# -S: port (default 2947)
exec gpsd -n -G -S 2947 "$GPS_DEV"
