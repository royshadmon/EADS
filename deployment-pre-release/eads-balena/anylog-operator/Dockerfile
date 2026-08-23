FROM peardotdash/anylog-network-arm:2.0.2606

RUN python3 -m venv /opt/external-venv && \
    /opt/external-venv/bin/pip install --upgrade pip

COPY deploy_anylog.sh /app/deploy_anylog.sh
RUN chmod +x /app/deploy_anylog.sh
