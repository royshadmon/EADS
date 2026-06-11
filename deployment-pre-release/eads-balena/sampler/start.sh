#!/bin/bash
# EADS Combined Sampler + Ingestor (Phase 1+2+3+4) — hardened release
#
# Stability features:
#  - sampling_bin auto-restart loop (DMA assertion recovery)
#  - ingestor auto-restart loop (any crash recovery)
#  - RT scheduling enabled via SYS_NICE cap (set in docker-compose)
#  - stall watchdog inside ingestor (_exit 42 if pipe dies → restart)
#  - backlog cap enforced inside ingestor (no unbounded SD growth)

echo "[EADS] === Phase 1+2+3+4 hardened start ==="

# Emergency backlog purge: if a prior run left a massive backlog that would
# choke startup, clear it. Threshold 800MB — well above the 500MB soft cap.
SEG_DIR=/var/eads-anomaly/segments
if [ -d "$SEG_DIR" ]; then
    BYTES=$(du -sb "$SEG_DIR" 2>/dev/null | awk '{print $1}')
    if [ -n "$BYTES" ] && [ "$BYTES" -gt 838860800 ]; then
        echo "[EADS] Emergency: backlog ${BYTES} bytes > 800MB, purging segments"
        rm -f "$SEG_DIR"/* /var/eads-anomaly/index.bin
    fi
fi

start_sampler_loop() {
    # Pin sampling_bin to core 2 with real-time priority. Its DMA copy loop
    # is HARD real-time (overrun = assertion abort). Core 2 is reserved for
    # it alone; the ingestor's threads live on core 3 and AnyLog on cores 0,1.
    # Prefer chrt/taskset when available.
    local LAUNCH="/app/sampling_bin"
    if command -v chrt >/dev/null 2>&1 && command -v taskset >/dev/null 2>&1; then
        LAUNCH="chrt -f 60 taskset -c 2 /app/sampling_bin"
        echo "[EADS] sampling_bin will run RT(FIFO 60) pinned to core 2"
    elif command -v taskset >/dev/null 2>&1; then
        LAUNCH="taskset -c 2 /app/sampling_bin"
        echo "[EADS] sampling_bin will run pinned to core 2 (no RT: chrt missing)"
    else
        echo "[EADS] WARNING: taskset/chrt missing; sampling_bin not isolated"
    fi
    while true; do
        echo "[EADS] Starting sampling binary..."
        $LAUNCH
        echo "[EADS] sampling_bin exited code $?, restarting in 5s..."
        sleep 5
    done
}
start_sampler_loop &

echo "[EADS] Waiting for AnyLog to be ready..."
until curl -sf -H "command: get status" -H "User-Agent: AnyLog/1.23" http://127.0.0.1:32149 > /dev/null 2>&1; do
    echo "[EADS] AnyLog not ready yet, retrying in 10 seconds..."
    sleep 10
done

echo "[EADS] AnyLog is ready, starting C ingestor..."
while true; do
    /app/eads_ingestor \
        --socket /var/sampling/samples.sock \
        --conn 127.0.0.1:32149 \
        --buffer-path /var/eads-anomaly \
        --drain-batch 4000 \
        --sample-rate 7680 \
        --disk-path / \
        --disk-pause-pct 85 \
        --disk-resume-pct 75 \
        --rms-pct 10.0 \
        --sad-limit 30000.0
    echo "[EADS] Ingestor exited code $?, restarting in 5 seconds..."
    sleep 5
done
