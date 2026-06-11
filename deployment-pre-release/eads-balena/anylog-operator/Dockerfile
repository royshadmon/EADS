FROM bronsenuc/anylog-network:arm-updated

RUN python3 -m venv /opt/external-venv && \
    /opt/external-venv/bin/pip install --upgrade pip

COPY deploy_anylog.sh /app/deploy_anylog.sh
RUN chmod +x /app/deploy_anylog.sh
