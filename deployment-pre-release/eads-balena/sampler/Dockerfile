FROM balenalib/raspberrypi3-64-debian:bullseye-run

WORKDIR /app

# Install all dependencies in one layer
RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libc6-dev \
    libck-dev \
    libcurl4-openssl-dev \
    curl \
    util-linux \
    && rm -rf /var/lib/apt/lists/*

# Build sampling binary
COPY sampling/src/ ./sampling/src/
RUN cd sampling/src && \
    export LIBRARY_PATH=/usr/lib/aarch64-linux-gnu:$LIBRARY_PATH && \
    make clean && \
    make SAMPLING_ADC=MCP3202 && \
    cp sampling /app/sampling_bin && \
    cd /app && rm -rf sampling/src

# Build C ingestor (Phase 1+2+3: detection + SD buffer + heartbeat)
COPY eads_ingestor.c \
     anomaly_detector.c anomaly_detector.h \
     anomaly_buffer.c   anomaly_buffer.h \
     pretrigger_ring.c  pretrigger_ring.h \
     ./
RUN gcc -O3 -march=armv8-a -mtune=cortex-a53 \
        -o eads_ingestor \
        eads_ingestor.c anomaly_detector.c \
        anomaly_buffer.c pretrigger_ring.c \
        -lcurl -lpthread -lm && \
    strip eads_ingestor && \
    rm *.c *.h

# Create socket directory and anomaly buffer mount point
RUN mkdir -p /var/sampling /var/eads-anomaly

COPY start.sh .
RUN chmod +x start.sh

CMD ["./start.sh"]
