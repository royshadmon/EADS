#!/bin/bash
# Time synchronisation for EADS field units.
#
# Primary source is gpsd's SHM segment (GPS time, no internet needed);
# chrony.conf falls back to pool.ntp.org, so a unit with no GPS attached
# still gets correct time as long as it has a network path.
#
# FIXED 2026-07-23: chronyd was crash-looping with
#     "Fatal error : Another chronyd may already be running (pid=1)"
# The cause is a self-detection deadlock specific to containers. `exec`
# makes chronyd PID 1, so it writes "1" into /run/chrony/chronyd.pid. On
# the next start it reads that file, asks whether PID 1 is alive, and the
# answer is yes — because PID 1 is the process doing the asking. chronyd
# concludes a rival instance is running and aborts. It never once started,
# which is why these nodes have been keeping time with an undisciplined
# clock.
#
# Why that matters here and is not cosmetic: every voltage row carries
# timestamp_us, and the operator partitions on insert_timestamp. A Pi has
# no RTC, so it boots near the epoch until something corrects it. Rows
# written before a correction land in the wrong partition and cannot be
# aligned against other nodes — which defeats the point of a fleet that is
# supposed to correlate grid events across houses.
set -u

PIDFILE=/run/chrony/chronyd.pid

# The container is the only thing that could have written this file, and if
# we are running, nothing else in this namespace owns it. Clearing it is
# safe and is what breaks the loop.
if [ -e "$PIDFILE" ]; then
    echo "[chrony] clearing stale pidfile $PIDFILE (contained: $(cat "$PIDFILE" 2>/dev/null || echo '?'))"
    rm -f "$PIDFILE"
fi
mkdir -p "$(dirname "$PIDFILE")"

# gpsd may never appear (no GPS hardware on most units). That is fine:
# the SHM refclock simply never supplies samples and chrony uses NTP. Do
# not block on it — a missing GPS must not stop the clock being set.
echo "[chrony] giving gpsd a moment to publish its SHM segment (optional)..."
sleep 5

echo "[chrony] starting chronyd (GPS if present, NTP pool otherwise)..."
exec chronyd -d -f /etc/chrony/chrony.conf
