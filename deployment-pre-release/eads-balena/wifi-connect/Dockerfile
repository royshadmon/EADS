# WiFi captive-portal onboarding (balena wifi-connect).
# END-USER FLOW: if the Pi has no internet ~30s after boot, it raises a
# WiFi access point named "EADS-Setup". The participant connects to it with
# their phone, a captive portal opens, they pick their home WiFi and type
# its password, the Pi joins it, the AP disappears. No app, no account,
# no Balena dashboard, no SSH. (Ethernet, or WiFi baked at flash time,
# bypasses this entirely — the portal only appears when there's no link.)
#
# FIXED 2026-07-23: the previous version tried to extract a "ui" folder from
# the BINARY tarball, which does not contain one — verified: that archive
# holds exactly one file, the `wifi-connect` executable. The extraction was
# suffixed with `|| true`, so the failure was silent and every image shipped
# with an empty UI directory. The access point came up serving nothing and
# the participant's phone showed a dead page at 192.168.42.1.
#
# The web UI is published as a SEPARATE release asset, wifi-connect-ui.tar.gz,
# whose contents live at the archive ROOT (./index.html, ./static/...) rather
# than under a ui/ prefix. Hence: extract it INTO the ui directory.
#
# No `|| true` anywhere below. If an asset moves or a release is retracted,
# the build must fail here — loudly, on your laptop — rather than producing
# an image that looks fine until it is in someone else's house.
FROM balenalib/raspberrypi3-64-debian:bullseye-run

ARG WFC_VERSION=4.11.84
ARG WFC_BASE=https://github.com/balena-os/wifi-connect/releases/download

RUN apt-get update && apt-get install -y --no-install-recommends \
        dnsmasq wireless-tools ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# --- binary ---------------------------------------------------------------
RUN curl -fsSL "${WFC_BASE}/v${WFC_VERSION}/wifi-connect-aarch64-unknown-linux-gnu.tar.gz" \
        -o /tmp/wfc.tgz \
    && tar -xzf /tmp/wfc.tgz -C /usr/local/bin wifi-connect \
    && chmod +x /usr/local/bin/wifi-connect \
    && rm /tmp/wfc.tgz \
    && /usr/local/bin/wifi-connect --version

# --- web UI (separate asset; contents are at the archive root) -------------
RUN mkdir -p /usr/local/share/wifi-connect/ui \
    && curl -fsSL "${WFC_BASE}/v${WFC_VERSION}/wifi-connect-ui.tar.gz" \
        -o /tmp/wfc-ui.tgz \
    && tar -xzf /tmp/wfc-ui.tgz -C /usr/local/share/wifi-connect/ui \
    && rm /tmp/wfc-ui.tgz \
    # Build-time proof the portal has something to serve. Without index.html
    # wifi-connect starts, broadcasts the AP, and returns nothing.
    && test -f /usr/local/share/wifi-connect/ui/index.html \
    && echo "[build] portal UI OK: $(ls /usr/local/share/wifi-connect/ui | tr '\n' ' ')"

COPY start.sh /start.sh
RUN chmod +x /start.sh
CMD ["/start.sh"]
