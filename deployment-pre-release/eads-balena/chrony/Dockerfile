FROM balenalib/raspberrypi3-64-debian:bullseye-run

RUN apt-get update && apt-get install -y \
    chrony \
    && rm -rf /var/lib/apt/lists/*

COPY chrony.conf /etc/chrony/chrony.conf
COPY start.sh /start.sh
RUN chmod +x /start.sh

CMD ["/start.sh"]
