#!/bin/bash
# Nebula container startup for EADS — final (env-var config delivery).
#
# PRIMARY PATH (set by provisioning/provision_setenv.sh at enrollment):
#   NEBULA_CONFIG    — the COMPLETE nebula config.yml with CA cert, host
#                      cert, and host key inlined as PEM block scalars.
#                      Self-contained: no file paths, no SD-card injection.
#   EADS_NEBULA_IP   — this Pi's overlay IP (e.g. 10.42.0.2). Published to
#                      /shared-nebula/ip for the AnyLog operator to advertise.
#
# LEGACY FALLBACK (pre-setenv enrollments; still works, will be retired):
#   NEBULA_CA_CRT / NEBULA_HOST_CRT / NEBULA_HOST_KEY  — separate PEMs;
#   config is generated here from a template.
#
# Why env vars and not SD-card files: Balena containers cannot bind-mount
# /mnt/boot and there is no bootfs feature label (verified against the
# authoritative label list). Device env vars are cached on-device by the
# supervisor — they survive reboots, data purges, and cloud outages; only
# the one-time initial `balena env set` needs connectivity.

set -e

NEB_DIR=/etc/nebula
mkdir -p "$NEB_DIR" /shared-nebula
NEB_IP=""

if [ -n "$NEBULA_CONFIG" ]; then
    # ── Path 1: full inlined config delivered via env (manual override) ──
    echo "[Nebula] Using NEBULA_CONFIG (inlined-PEM config from enrollment)"
    printf '%s\n' "$NEBULA_CONFIG" > "$NEB_DIR/config.yml"
    chmod 600 "$NEB_DIR/config.yml"

    # Overlay IP: prefer the explicit enrollment var; otherwise parse it out
    # of the inlined host cert so there is never a second source of truth.
    if [ -n "$EADS_NEBULA_IP" ]; then
        NEB_IP="$EADS_NEBULA_IP"
    else
        # Extract the host cert PEM block (the one labeled NEBULA CERTIFICATE
        # under the `cert:` key) and ask nebula-cert for its IP.
        awk '/^  cert: \|/{f=1;next} f{if(sub(/^    /,"")){print}else{f=0}}' \
            "$NEB_DIR/config.yml" > /tmp/host.crt || true
        if [ -s /tmp/host.crt ] && /usr/local/bin/nebula-cert print -path /tmp/host.crt > /tmp/cert-info 2>&1; then
            NEB_IP=$(grep -oE '10\.[0-9]+\.[0-9]+\.[0-9]+/[0-9]+' /tmp/cert-info | head -1 | cut -d/ -f1)
        fi
        rm -f /tmp/host.crt /tmp/cert-info
    fi

elif [ -s /shared-nebula/enrolled.yml ]; then
    # ── Path 2: identity cached from a previous auto-enrollment ─────────
    # Use cached identity from the shared volume.
    echo "[Nebula] Using cached enrolled identity"
    cp /shared-nebula/enrolled.yml "$NEB_DIR/config.yml"
    chmod 600 "$NEB_DIR/config.yml"
    NEB_IP="$(cat /shared-nebula/ip 2>/dev/null || true)"

elif [ -n "$EADS_ENROLL_TOKEN" ]; then
    # ── Path 3: PLUG-AND-PLAY auto-enrollment ───────────────────────────
    # First boot in the field: no identity anywhere. Phone home to the
    # master's enrollment service over pinned TLS, present the fleet
    # token + our hardware UUID, receive a pre-generated Nebula identity.
    # The fleet token is a FLEET-level Balena var (set once, ever) — no
    # per-device Balena interaction is needed.
    ENROLL_URL="${EADS_ENROLL_URL:-https://eads-master.duckdns.org:8443/enroll}"
    UUID="${BALENA_DEVICE_UUID:-}"
    [ -z "$UUID" ] && UUID="$(cat /etc/machine-id 2>/dev/null || true)"
    [ -z "$UUID" ] && UUID="$(hostname)"

    CURL_ARGS=(-fsS --cacert /app/enroll-ca.crt -X POST "$ENROLL_URL"
               -H "Content-Type: application/json"
               --data "{\"token\":\"$EADS_ENROLL_TOKEN\",\"uuid\":\"$UUID\"}")
    # Diagnostic override (e.g. testing against a local server):
    [ -n "$EADS_ENROLL_RESOLVE" ] && CURL_ARGS+=(--resolve "$EADS_ENROLL_RESOLVE")

    echo "[Nebula] No identity yet — enrolling at $ENROLL_URL (uuid $UUID)"
    until curl "${CURL_ARGS[@]}" -o /tmp/enroll.json; do
        echo "[Nebula] Enrollment failed (master unreachable / bad token?). Retrying in 30s..."
        sleep 30
    done
    NODE_NAME="$(jq -r '.node_name' /tmp/enroll.json)"
    NEB_IP="$(jq -r '.nebula_ip' /tmp/enroll.json)"
    jq -r '.config' /tmp/enroll.json > "$NEB_DIR/config.yml"
    rm -f /tmp/enroll.json
    chmod 600 "$NEB_DIR/config.yml"
    if ! grep -qE -- "-----BEGIN NEBULA CERTIFICATE( V2)?-----" "$NEB_DIR/config.yml"; then
        echo "[Nebula] FATAL: enrollment response did not contain a valid config" >&2
        exit 1
    fi
    # Cache for every future boot + publish identity for sibling containers.
    cp "$NEB_DIR/config.yml" /shared-nebula/enrolled.yml
    echo "$NODE_NAME" > /shared-nebula/node_name
    echo "[Nebula] Enrolled as $NODE_NAME ($NEB_IP)"

elif [ -n "$NEBULA_CA_CRT" ]; then
    # ── Legacy: three separate PEM vars; generate config locally ────────
    echo "[Nebula] Using legacy 3-var credential delivery"
    LIGHTHOUSE_NEB_IP="${NEBULA_LIGHTHOUSE_IP:-10.42.0.1}"
    LIGHTHOUSE_PUBLIC="${NEBULA_LIGHTHOUSE_PUBLIC:-eads-master.duckdns.org:4242}"

    for var in NEBULA_CA_CRT NEBULA_HOST_CRT NEBULA_HOST_KEY; do
        if [ -z "${!var}" ]; then
            echo "[Nebula] FATAL: required env var $var is not set." >&2
            exit 1
        fi
    done
    printf '%s\n' "$NEBULA_CA_CRT"   > "$NEB_DIR/ca.crt"
    printf '%s\n' "$NEBULA_HOST_CRT" > "$NEB_DIR/host.crt"
    printf '%s\n' "$NEBULA_HOST_KEY" > "$NEB_DIR/host.key"
    chmod 600 "$NEB_DIR/host.key"

    if ! /usr/local/bin/nebula-cert print -path "$NEB_DIR/host.crt" >/tmp/cert-info 2>&1; then
        echo "[Nebula] FATAL: host cert failed to parse:"; cat /tmp/cert-info; exit 1
    fi
    NEB_IP=$(grep -oE '10\.[0-9]+\.[0-9]+\.[0-9]+/[0-9]+' /tmp/cert-info | head -1 | cut -d/ -f1)

    cat > "$NEB_DIR/config.yml" << CFG
pki:
  ca: $NEB_DIR/ca.crt
  cert: $NEB_DIR/host.crt
  key: $NEB_DIR/host.key

static_host_map:
  "$LIGHTHOUSE_NEB_IP": ["$LIGHTHOUSE_PUBLIC"]

lighthouse:
  am_lighthouse: false
  interval: 60
  hosts:
    - "$LIGHTHOUSE_NEB_IP"

listen:
  host: 0.0.0.0
  port: 0

punchy:
  punch: true
  respond: true

tun:
  disabled: false
  dev: nebula1
  drop_local_broadcast: false
  drop_multicast: false
  tx_queue: 500
  mtu: 1300

firewall:
  conntrack:
    tcp_timeout: 12m
    udp_timeout: 3m
    default_timeout: 10m
  outbound:
    - port: any
      proto: any
      host: any
  inbound:
    - port: any
      proto: icmp
      host: any
    - port: 32148
      proto: tcp
      host: any
    - port: 32149
      proto: tcp
      host: any
    - port: 32250
      proto: tcp
      host: any
CFG
else
    echo "[Nebula] FATAL: no credentials and no enrollment token." >&2
    echo "[Nebula] Either set the FLEET var EADS_ENROLL_TOKEN (plug-and-play" >&2
    echo "[Nebula] auto-enrollment, recommended), or enroll manually with" >&2
    echo "[Nebula]   provisioning/provision_setenv.sh <node> <device-uuid>" >&2
    exit 1
fi

if [ -z "$NEB_IP" ]; then
    echo "[Nebula] FATAL: could not determine overlay IP (set EADS_NEBULA_IP)" >&2
    exit 1
fi

# Publish to the shared volume so deploy_anylog.sh advertises this address
# (and so the cached-identity path can recover it on the next boot).
echo "$NEB_IP" > /shared-nebula/ip

echo "[Nebula] ============================================="
echo "[Nebula] Overlay IP : $NEB_IP"
echo "[Nebula] Config     : $NEB_DIR/config.yml"
echo "[Nebula] ============================================="

# Validate before launch — a malformed enrollment fails loudly here instead
# of crash-looping with a cryptic daemon error.
if ! /usr/local/bin/nebula -test -config "$NEB_DIR/config.yml"; then
    echo "[Nebula] FATAL: config failed validation (nebula -test)" >&2
    exit 1
fi

echo "[Nebula] starting daemon..."
exec /usr/local/bin/nebula -config "$NEB_DIR/config.yml"
