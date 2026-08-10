#!/bin/bash
# ---------------------------------------------------------------------------
# Connectivity sentinel for EADS field units.
#
# Raises the "EADS-Setup" captive portal ONLY when the device is genuinely
# offline — e.g. a new home, or the WiFi password changed. If the device can
# reach the internet, this must never touch wlan0.
#
# WHY THIS IS CAREFUL:
# The portal takes over wlan0 to broadcast an access point. If it fires on a
# false negative, it destroys a working connection and the node is stranded
# until a human walks over to it. In a fleet living in other people's houses,
# that is the single most expensive failure mode there is. So:
#   - a single failed check is NOT enough; we require several in a row
#   - the check tries multiple independent endpoints before giving up
#   - the portal has an activity timeout, so if it fires by mistake the
#     device recovers on its own instead of waiting for a person
# ---------------------------------------------------------------------------
set -uo pipefail

# Consecutive failed checks required before raising the portal.
FAIL_THRESHOLD="${WIFI_FAIL_THRESHOLD:-4}"
# Seconds between checks while things look healthy.
CHECK_INTERVAL="${WIFI_CHECK_INTERVAL:-60}"
# Seconds between checks once we've started failing (retry faster).
RETRY_INTERVAL="${WIFI_RETRY_INTERVAL:-20}"
# Give an existing connection time to come up after boot before judging it.
BOOT_GRACE="${WIFI_BOOT_GRACE:-90}"
# If nobody uses the portal within this many seconds, tear it down and
# re-check. Prevents a mistaken portal from stranding the node forever.
PORTAL_TIMEOUT="${WIFI_PORTAL_TIMEOUT:-600}"

UI_DIR="/usr/local/share/wifi-connect/ui"

log() { echo "[wifi-connect] $*"; }

# --- connectivity check ----------------------------------------------------
# Returns 0 if ANY signal says we're online. Deliberately generous: the cost
# of a false "offline" (destroying a good connection) is far higher than the
# cost of a false "online" (we simply check again in a minute).
check() {
    # 1. NetworkManager's own verdict. Accept "full"; do NOT treat anything
    #    else as proof of failure, since NM's check URL is often blocked or
    #    disabled on residential networks and then reports "none"/"unknown"
    #    on a perfectly good link.
    if nmcli -t -f CONNECTIVITY general 2>/dev/null | grep -q '^full$'; then
        return 0
    fi

    # 2. Independent reachability probes. Any one succeeding means online.
    #    Multiple providers so a single service outage can't strand us.
    for url in \
        https://api.balena-cloud.com/ping \
        https://1.1.1.1 \
        https://www.google.com/generate_204
    do
        if curl -sf -m 8 -o /dev/null "$url" 2>/dev/null; then
            return 0
        fi
    done

    # 3. Last resort: can we resolve DNS and reach the default gateway?
    #    Catches captive-portal-ish networks that still carry our traffic.
    gw=$(ip route show default 2>/dev/null | awk '/default/ {print $3; exit}')
    if [ -n "${gw:-}" ] && ping -c1 -W3 "$gw" >/dev/null 2>&1; then
        # Gateway reachable but no internet. This is a degraded network, not
        # a missing one — raising a setup portal would not help the user.
        log "gateway $gw reachable but no internet; treating as online (portal would not help)"
        return 0
    fi

    return 1
}

# --- preflight -------------------------------------------------------------
if [ ! -d "$UI_DIR" ] || [ -z "$(ls -A "$UI_DIR" 2>/dev/null)" ]; then
    log "WARNING: portal UI missing or empty at $UI_DIR"
    log "         The access point would come up serving nothing, and the"
    log "         participant's phone would show a dead page. Refusing to"
    log "         raise a broken portal; staying out of the way instead."
    UI_OK=0
else
    UI_OK=1
fi

log "sentinel started (threshold=${FAIL_THRESHOLD} checks, boot grace=${BOOT_GRACE}s, ui_ok=${UI_OK})"
sleep "$BOOT_GRACE"

fails=0
while true; do
    if check; then
        if [ "$fails" -gt 0 ]; then
            log "connectivity restored after $fails failed check(s)"
        fi
        fails=0
        sleep "$CHECK_INTERVAL"
        continue
    fi

    fails=$((fails + 1))
    log "connectivity check failed ($fails/$FAIL_THRESHOLD)"

    if [ "$fails" -lt "$FAIL_THRESHOLD" ]; then
        sleep "$RETRY_INTERVAL"
        continue
    fi

    if [ "$UI_OK" -ne 1 ]; then
        log "offline, but portal UI is missing — not raising AP. Re-checking in 60s."
        log "Fix: ensure the wifi-connect release tarball's ui/ folder is in the image."
        fails=0
        sleep 60
        continue
    fi

    log "confirmed offline after $FAIL_THRESHOLD consecutive checks — raising EADS-Setup portal"
    /usr/local/bin/wifi-connect \
        --portal-ssid "EADS-Setup" \
        --ui-directory "$UI_DIR" \
        --activity-timeout "$PORTAL_TIMEOUT" \
        || log "wifi-connect exited non-zero (timeout or error)"

    log "portal closed; re-checking in 20s"
    fails=0
    sleep 20
done
