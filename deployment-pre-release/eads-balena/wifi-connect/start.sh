#!/bin/bash
# Connectivity sentinel: if the device can reach the internet, sleep and
# re-check. If not (new home, changed WiFi password), raise the portal.

check() {
    # NetworkManager full connectivity, with a curl fallback.
    state=$(curl -sf -m 5 --unix-socket /var/run/dbus/system_bus_socket x 2>/dev/null; true)
    nmcli -t -f CONNECTIVITY general 2>/dev/null | grep -q full && return 0
    curl -sf -m 8 https://api.balena-cloud.com/ping >/dev/null 2>&1 && return 0
    return 1
}

echo "[wifi-connect] sentinel started"
sleep 25   # give an existing connection time to come up after boot
while true; do
    if check; then
        sleep 60
        continue
    fi
    echo "[wifi-connect] no connectivity — raising EADS-Setup portal"
    /usr/local/bin/wifi-connect \
        --portal-ssid "EADS-Setup" \
        --ui-directory /usr/local/share/wifi-connect/ui \
        || true
    echo "[wifi-connect] portal closed; re-checking in 20s"
    sleep 20
done
