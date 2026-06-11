#!/bin/bash
# Publishes the GPS fix to /shared-nebula/gps as "lat,lon,fix_mode" so the
# ingestor can stamp health rows and the fleet map auto-locates this box.
# Runs forever; one read every PUBLISH_SEC (default 300 — coordinates of a
# stationary box don't change, this is not a tracker).
PUBLISH_SEC="${GPS_PUBLISH_SEC:-300}"
OUT=/shared-nebula/gps

extract_fix() {
    # Read up to 25 sentences for up to 20s; print "lat,lon,mode" for the
    # first TPV with a 2D/3D fix. gpspipe emits one JSON object per line.
    timeout 20 gpspipe -w -n 25 2>/dev/null | while IFS= read -r line; do
        case "$line" in
            *'"class":"TPV"'*'"mode":2'*|*'"class":"TPV"'*'"mode":3'*) ;;
            *) continue ;;
        esac
        lat=$(printf '%s' "$line" | sed -n 's/.*"lat":\(-\{0,1\}[0-9.]*\).*/\1/p')
        lon=$(printf '%s' "$line" | sed -n 's/.*"lon":\(-\{0,1\}[0-9.]*\).*/\1/p')
        mode=$(printf '%s' "$line" | sed -n 's/.*"mode":\([23]\).*/\1/p')
        if [ -n "$lat" ] && [ -n "$lon" ]; then
            printf '%s,%s,%s\n' "$lat" "$lon" "$mode"
            return 0
        fi
    done
    return 1
}

echo "[gps-publish] loop started (every ${PUBLISH_SEC}s)"
while true; do
    if FIX=$(extract_fix) && [ -n "$FIX" ]; then
        printf '%s\n' "$FIX" > "${OUT}.tmp" && mv "${OUT}.tmp" "$OUT"
        echo "[gps-publish] fix: $FIX"
    else
        echo "[gps-publish] no fix this cycle (indoors / cold start?)"
    fi
    sleep "$PUBLISH_SEC"
done
