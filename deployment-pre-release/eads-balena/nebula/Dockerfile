# Nebula overlay network client for EADS Pi nodes.
# Joins the EADS mesh so the operator can be reached from anywhere via its
# stable overlay IP (10.42.0.X), regardless of physical network/NAT.
FROM debian:bookworm-slim

# Architecture is auto-detected by Balena builders for the target Pi
ARG NEBULA_VERSION=1.10.3

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl iproute2 jq \
    && rm -rf /var/lib/apt/lists/*

# Install the Nebula binary. Balena builds for arm64 on Pi 3 A+.
RUN ARCH=$(uname -m) && \
    case "$ARCH" in \
        aarch64|arm64) NEBARCH=arm64 ;; \
        armv7l|armhf)  NEBARCH=arm-7 ;; \
        x86_64)        NEBARCH=amd64 ;; \
        *) echo "Unsupported arch: $ARCH"; exit 1 ;; \
    esac && \
    curl -fsSL "https://github.com/slackhq/nebula/releases/download/v${NEBULA_VERSION}/nebula-linux-${NEBARCH}.tar.gz" \
        -o /tmp/nebula.tgz && \
    tar -xzf /tmp/nebula.tgz -C /usr/local/bin && \
    chmod +x /usr/local/bin/nebula /usr/local/bin/nebula-cert && \
    rm /tmp/nebula.tgz

# Pinned TLS cert of the master's enrollment service (auto-enroll path).
COPY enroll-ca.crt /app/enroll-ca.crt
COPY start.sh /start.sh
RUN chmod +x /start.sh

CMD ["/start.sh"]
