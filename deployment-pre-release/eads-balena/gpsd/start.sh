#!/bin/bash
# EADS gpsd startup — Quectel GPS module on the HAT, over UART.
#
# FIXED 2026-07-24: gpsd was launched with `-n` but not `-N`. Lowercase -n
# only means "poll the receiver without waiting for a client to connect".
# It is uppercase -N that keeps gpsd in the FOREGROUND. Without it gpsd
# daemonises: it forks a background copy and the process we exec'd returns
# immediately. Balena sees the container's main process exit, restarts it,
# and the cycle repeats every couple of seconds forever — which is exactly
# the "Found GPS device ... Service exited" pattern in the logs.
#
# Two consequences of that bug worth knowing, because both look like
# hardware faults:
#   - publish_fix.sh is started in the background here, so it was killed
#     with the container ~1s later, every cycle. It never survived long
#     enough to complete a single 20s read, so /shared-nebula/gps was
#     never written and node_health rows carried no lat/lon.
#   - chrony's `refclock SHM 0` had nothing writing the segment, so GPS
#     time was never actually available and chrony silently used NTP.
set -u

GPS_DEV=""
echo "[gpsd] Waiting for GPS device..."
for _ in $(seq 1 30); do
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
    echo "[gpsd] No GPS device found after 30s, exiting."
    echo "[gpsd] On a Pi 3 the PL011 UART is wired to Bluetooth by default."
    echo "[gpsd] Free it with the balena host config vars:"
    echo "[gpsd]   BALENA_HOST_CONFIG_dtoverlay = disable-bt"
    echo "[gpsd]   BALENA_HOST_CONFIG_enable_uart = 1"
    exit 1
fi

echo "[gpsd] Found GPS device: $GPS_DEV"

# A stale control socket survives a container restart and makes gpsd refuse
# to bind. Cheap to clear, and it removes a confusing second failure mode.
rm -f /var/run/gpsd.sock 2>/dev/null || true

# Background publisher: writes "lat,lon,mode" to /shared-nebula/gps for the
# ingestor to stamp into node_health. Safe to start before gpsd is up — it
# retries on its own schedule and simply logs "no fix this cycle" until the
# receiver has one.
/publish_fix.sh &

# -N : stay in the foreground (THE FIX — see header)
# -n : begin polling immediately, don't wait for a client
# -G : listen on all interfaces so sibling containers can reach gpsd
# -S : control port
echo "[gpsd] starting gpsd in foreground on $GPS_DEV ..."
exec gpsd -N -n -G -S 2947 "$GPS_DEV"
