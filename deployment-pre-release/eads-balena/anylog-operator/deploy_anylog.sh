#!/bin/bash
# deploy_anylog.sh — EADS AnyLog operator startup with STABLE DEVICE IDENTITY
#
# THE FIX: identity is resolved HERE, at container runtime, where
# $BALENA_DEVICE_UUID is a real environment variable provided by the Balena
# supervisor. We do NOT rely on docker-compose "${BALENA_DEVICE_UUID}"
# substitution — that happens at compose-render time when the var may be
# unset, which is why the node name showed up as the literal
# "operator-${BALENA_DEVICE_UUID}" before.
#
# We export NODE_NAME / MEMBER / CLUSTER_NAME (and a few others AnyLog reads
# from the environment) so the agent picks up the resolved values regardless
# of any defaults in the deployment scripts.
#
# Flash the SAME image to every SD card; Balena assigns each physical device a
# permanent UUID at first boot, so each operator self-assigns a stable, unique
# identity with zero per-card work.

# ── Resolve a stable device UUID at RUNTIME ─────────────────────────────
# BALENA_DEVICE_UUID is injected by the supervisor because the operator
# service sets the io.balena.features.balena-api label in docker-compose.
# (Plain ${VAR} interpolation in docker-compose does NOT work for BALENA_*,
# which is why a previous attempt produced operator-fallback.)
UUID="${BALENA_DEVICE_UUID:-}"
if [[ -z "$UUID" ]]; then
    UUID="${RESIN_DEVICE_UUID:-}"
fi
if [[ -z "$UUID" ]]; then
    # Last resort: machine-id, else the container hostname. The hostname is
    # the container ID — still UNIQUE per device, so two Pis never collide
    # even in this degraded path (unlike a hardcoded constant).
    UUID="$(cat /etc/machine-id 2>/dev/null)"
    [[ -z "$UUID" ]] && UUID="$(hostname)"
    echo "[Identity] WARNING: BALENA_DEVICE_UUID unset; using fallback id: $UUID"
    echo "[Identity] (check the io.balena.features.balena-api label is set)"
fi

SHORT="${UUID:0:12}"

# Stable identity, exported into the environment AnyLog reads.
export NODE_NAME="operator-${SHORT}"
HASH=$(printf '%s' "$UUID" | cksum | awk '{print $1}')
export MEMBER=$(( (HASH % 32000) + 1 ))
export CLUSTER_NAME="eads-cluster-${SHORT}"

# Guard: if NODE_NAME somehow still contains an unexpanded "${" (should be
# impossible now), fall back to the unique container hostname rather than
# registering a broken name on the master.
if [[ "$NODE_NAME" == *'${'* ]]; then
    echo "[Identity] ERROR: NODE_NAME malformed ('$NODE_NAME'); using hostname"
    export NODE_NAME="operator-$(hostname | cut -c1-12)"
fi

echo "[Identity] ============================================="
echo "[Identity] Device UUID : $UUID"
echo "[Identity] NODE_NAME   : $NODE_NAME"
echo "[Identity] MEMBER      : $MEMBER"
echo "[Identity] CLUSTER_NAME: $CLUSTER_NAME"
echo "[Identity] ============================================="

# ── Advertise our NEBULA OVERLAY IP (not the LAN IP) ────────────────────
# Each Pi joins the Nebula mesh (10.42.0.0/24) via its own signed cert. The
# nebula sidecar container writes our assigned overlay IP to a shared volume;
# we read it here and tell AnyLog to bind/advertise THAT address. This is
# what makes operators reachable from any querying node, regardless of
# physical network or NAT — the master dials the overlay IP, Nebula traverses
# NAT, traffic lands here.
export TCP_BIND=true
export REST_BIND=true

# Fast path: the enrollment env var (set by provision_setenv.sh) names our
# overlay IP directly — no waiting on the shared volume.
NEB_IP="${EADS_NEBULA_IP:-}"
if [ -z "$NEB_IP" ]; then
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        if [ -r /shared-nebula/ip ]; then
            NEB_IP="$(cat /shared-nebula/ip 2>/dev/null)"
            [ -n "$NEB_IP" ] && break
        fi
        echo "[Identity] Waiting for Nebula overlay IP (attempt $attempt/10)..."
        sleep 3
    done
fi

if [ -z "$NEB_IP" ]; then
    echo "[Identity] ============================================="
    echo "[Identity] FATAL: Nebula overlay IP not available after 30s."
    echo "[Identity] The nebula sidecar must publish /shared-nebula/ip"
    echo "[Identity] before the operator can register. Common causes:"
    echo "[Identity]   - NEBULA_CA_CRT/HOST_CRT/HOST_KEY env vars missing"
    echo "[Identity]   - nebula container failed to start (check its logs)"
    echo "[Identity]   - lighthouse unreachable (check master is up,"
    echo "[Identity]     UDP 4242 forwarded on its router)"
    echo "[Identity] Exiting so Balena restarts us; nebula may be ready next round."
    echo "[Identity] ============================================="
    exit 1
fi

# AnyLog reads EXTERNAL_IP / OVERLAY_IP from env on some builds; set both to
# be safe. The authoritative bind comes from TCP_BIND=true + the address
# AnyLog detects on its interfaces — since our netns is the nebula sidecar's,
# nebula1 IS our primary interface, so AnyLog will naturally use it.
export EXTERNAL_IP="$NEB_IP"
export OVERLAY_IP="$NEB_IP"

echo "[Identity] NEBULA_IP   : $NEB_IP  (advertised to master; TCP_BIND=true)"
echo "[Identity] ============================================="

# ── Original deploy logic ────────────────────────────────────────────────
source /opt/external-venv/bin/activate && python3 -m pip install --upgrade pip --quiet

if [[ ! -d /app/deployment-scripts || ! "$(ls -A /app/deployment-scripts)" ]]; then
    echo "[Deploy] Cloning deployment scripts..."
    until git clone -b os-dev https://github.com/AnyLog-co/deployment-scripts /app/deployment-scripts; do
        echo "[Deploy] Clone failed, retrying in 10 seconds..."
        rm -rf /app/deployment-scripts
        sleep 10
    done
fi

export LOCAL_SCRIPTS=/app/deployment-scripts
/app/anylog_agent process /app/deployment-scripts/node-deployment/main.al
