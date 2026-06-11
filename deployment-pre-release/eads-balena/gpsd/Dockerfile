FROM balenalib/raspberrypi3-64-debian:bullseye-run

# gpsd-clients is back ON PURPOSE (it was trimmed earlier as debug-only):
# gpspipe is now load-bearing — the publish loop uses it to read one TPV
# fix every 5 minutes and write coordinates for the fleet's outage map.
RUN apt-get update && apt-get install -y --no-install-recommends \
    gpsd \
    gpsd-clients \
    && rm -rf /var/lib/apt/lists/*

COPY start.sh /start.sh
COPY publish_fix.sh /publish_fix.sh
RUN chmod +x /start.sh /publish_fix.sh

CMD ["/start.sh"]
