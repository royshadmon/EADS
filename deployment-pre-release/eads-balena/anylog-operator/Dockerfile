FROM peardotdash/anylog-network-arm:2.0.2606
USER root

RUN python3 -m venv /opt/external-venv && \
    /opt/external-venv/bin/pip install --upgrade pip
RUN sed -i 's/set license where activation_key = !activation_key/set license where activation_key = !license_key/' /app/deployment-scripts/node-deployment/policies/license_policy.al

COPY deploy_anylog.sh /app/deploy_anylog.sh
RUN chmod +x /app/deploy_anylog.sh
