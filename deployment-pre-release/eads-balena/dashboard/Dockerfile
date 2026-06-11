# EADS local dashboard — single static C binary serving node health on :8080.
# Same base image as sampler/gpsd/chrony so the base layers are SHARED on the
# device: the marginal disk cost of this container is just the binary +
# libraspberrypi-bin. vcgencmd gives core voltage + undervoltage/throttle
# flags (needs /dev/vchiq, passed through in docker-compose).
FROM balenalib/raspberrypi3-64-debian:bullseye-run

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev libraspberrypi-bin \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY dashboard.c .
RUN gcc -O2 -Wall -Wextra -o dashboard dashboard.c && \
    strip dashboard && rm dashboard.c && \
    apt-get purge -y gcc libc6-dev && apt-get autoremove -y

COPY start.sh .
RUN chmod +x start.sh
CMD ["./start.sh"]
