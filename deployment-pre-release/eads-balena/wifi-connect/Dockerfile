# WiFi captive-portal onboarding (balena wifi-connect).
# END-USER FLOW: if the Pi has no internet ~30s after boot, it raises a
# WiFi access point named "EADS-Setup". The participant connects to it with
# their phone, a captive portal opens, they pick their home WiFi and type
# its password, the Pi joins it, the AP disappears. No app, no account,
# no Balena dashboard, no SSH. (Ethernet, or WiFi baked at flash time,
# bypasses this entirely — the portal only appears when there's no link.)
FROM balenalib/raspberrypi3-64-debian:bullseye-run

ARG WFC_VERSION=4.11.84

RUN apt-get update && apt-get install -y --no-install-recommends \
        dnsmasq wireless-tools ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL "https://github.com/balena-os/wifi-connect/releases/download/v${WFC_VERSION}/wifi-connect-aarch64-unknown-linux-gnu.tar.gz" \
        -o /tmp/wfc.tgz \
    && tar -xzf /tmp/wfc.tgz -C /usr/local/bin wifi-connect \
    && mkdir -p /usr/local/share/wifi-connect \
    && tar -xzf /tmp/wfc.tgz -C /usr/local/share/wifi-connect ui || true \
    && rm /tmp/wfc.tgz

COPY start.sh /start.sh
RUN chmod +x /start.sh
CMD ["/start.sh"]
