#!/bin/bash
# =============================================================================
# EADS Master Setup — run ON the Ubuntu VM from this directory.
# Idempotent: safe to re-run. Installs the Nebula lighthouse (native +
# systemd) and brings up the full Docker stack (AnyLog master, AnyLog query
# node, PostgreSQL, Grafana, Fleet service).
# =============================================================================
set -e

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()   { echo -e "${GREEN}[✓] $1${NC}"; }
warn()  { echo -e "${YELLOW}[!] $1${NC}"; }
error() { echo -e "${RED}[✗] $1${NC}"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
NEBULA_VERSION=1.9.5

# ── 0. Preflight ─────────────────────────────────────────────────────────
command -v docker >/dev/null || error "docker not installed"
[ -f "$HERE/.env" ] || error "Copy .env.example to .env and fill it in first"
# shellcheck disable=SC1091
set -a; . "$HERE/.env"; set +a
[ -n "$ANYLOG_LICENSE" ] || error ".env: ANYLOG_LICENSE is empty"
[ -n "$ENROLL_TOKEN" ]   || warn ".env: ENROLL_TOKEN empty — plug-and-play enrollment will be DISABLED until set (openssl rand -hex 24)"

# ── 1. Nebula lighthouse (native binary + systemd) ───────────────────────
if [ ! -x /usr/local/bin/nebula ]; then
    log "Installing Nebula $NEBULA_VERSION..."
    curl -fsSL "https://github.com/slackhq/nebula/releases/download/v${NEBULA_VERSION}/nebula-linux-amd64.tar.gz" \
        -o /tmp/nebula.tgz
    sudo tar -xzf /tmp/nebula.tgz -C /usr/local/bin nebula nebula-cert
    sudo chmod +x /usr/local/bin/nebula /usr/local/bin/nebula-cert
    rm /tmp/nebula.tgz
else
    log "Nebula binary present"
fi

sudo mkdir -p /etc/nebula
for f in ca.crt lighthouse.crt lighthouse.key; do
    if [ ! -f "/etc/nebula/$f" ]; then
        warn "/etc/nebula/$f missing."
        echo "  Generate on the MacBook (the CA key stays there):"
        echo "    cd provisioning && nebula-cert sign -name eads-lighthouse \\"
        echo "      -ip 10.42.0.1/24 -ca-crt ca.crt -ca-key ca.key \\"
        echo "      -out-crt lighthouse.crt -out-key lighthouse.key"
        echo "  Then: scp ca.crt lighthouse.crt lighthouse.key <this-vm>:/tmp/ "
        echo "        sudo mv /tmp/{ca.crt,lighthouse.crt,lighthouse.key} /etc/nebula/"
        error "lighthouse identity incomplete"
    fi
done
sudo cp "$HERE/nebula/lighthouse-config.yml" /etc/nebula/config.yml
sudo chmod 600 /etc/nebula/lighthouse.key /etc/nebula/config.yml
/usr/local/bin/nebula -test -config /etc/nebula/config.yml \
    || error "lighthouse config failed validation"
sudo cp "$HERE/nebula/nebula-lighthouse.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nebula-lighthouse
sleep 2
ip -br addr show nebula1 >/dev/null 2>&1 \
    && log "Lighthouse up: $(ip -br addr show nebula1 | awk '{print $3}')" \
    || error "nebula1 interface did not come up — journalctl -u nebula-lighthouse"

# ── 2. Firewall ──────────────────────────────────────────────────────────
if command -v ufw >/dev/null; then
    sudo ufw allow 4242/udp  comment 'nebula lighthouse'        >/dev/null
    sudo ufw allow 8443/tcp  comment 'eads enrollment (TLS)'    >/dev/null
    sudo ufw allow from 10.0.0.0/24 to any port 8090 proto tcp comment 'fleet UI (LAN only)' >/dev/null
    sudo ufw allow in on nebula1                                >/dev/null
    log "UFW: 4242/udp + 8443/tcp + overlay traffic allowed"
    warn "Router: forward UDP 4242 and TCP 8443 to this VM (Xfinity app)."
fi

# ── 3. Identity pool + TLS material ──────────────────────────────────────
[ -d "$HERE/identity-pool" ] && [ -n "$(ls -A "$HERE/identity-pool" 2>/dev/null)" ] \
    && log "Identity pool: $(ls "$HERE/identity-pool" | wc -l) identities staged" \
    || warn "identity-pool/ is empty — export one from the MacBook: ./provision_pool.sh ... (see provisioning README)"
[ -f "$HERE/enroll-tls/enroll.crt" ] && [ -f "$HERE/enroll-tls/enroll.key" ] \
    && log "Enrollment TLS material present" \
    || warn "enroll-tls/ missing enroll.crt/enroll.key (ships in this package)"

# ── 4. Bring up the stack ────────────────────────────────────────────────
log "Starting Docker stack (master, query, postgres, grafana, fleet)..."
docker compose -f "$HERE/docker-compose.yml" up -d --build

sleep 8
echo ""
log "Verification:"
for svc in anylog-master anylog-query eads-postgres eads-grafana eads-fleet eads-archiver; do
    docker ps --format '{{.Names}} {{.Status}}' | grep -q "^$svc" \
        && echo "    $svc: running" || warn "    $svc: NOT running"
done
curl -sf -m 5 -H "command: get status" -H "User-Agent: AnyLog/1.23" http://127.0.0.1:32049 >/dev/null \
    && log "AnyLog master REST answering (32049)" || warn "master REST not answering yet (can take ~60s)"
curl -sf -m 5 -H "command: get status" -H "User-Agent: AnyLog/1.23" http://127.0.0.1:32449 >/dev/null \
    && log "AnyLog QUERY node REST answering (32449) — the website's data source" \
    || warn "query node REST not answering yet (can take ~60s)"
curl -sf -m 5 http://127.0.0.1:8090/health >/dev/null \
    && log "Fleet service answering (8090)" || warn "fleet service not answering"

# ── 5. Duplicate-policy guard ────────────────────────────────────────────
# The AnyLog master re-declares its policies on every restart against the
# persistent ledger (the long-standing duplicate-policy bug). This guard
# keeps the oldest copy of each and drops the rest — idempotent, replaces
# the old wipe-the-volumes workaround. Also installed as a systemd unit so
# every VM reboot self-heals.
log "Running duplicate-policy guard..."
sleep 10   # give the master a moment to finish ledger init
python3 "$HERE/anylog/fix_duplicate_policies.py" || warn "dedup guard could not run (master still starting? re-run: python3 anylog/fix_duplicate_policies.py)"
sudo tee /etc/systemd/system/eads-policy-dedup.service >/dev/null << UNIT
[Unit]
Description=EADS AnyLog duplicate-policy guard
After=docker.service
Requires=docker.service

[Service]
Type=oneshot
ExecStartPre=/bin/sleep 90
ExecStart=/usr/bin/python3 $HERE/anylog/fix_duplicate_policies.py

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload && sudo systemctl enable eads-policy-dedup >/dev/null 2>&1
log "Policy dedup guard installed (runs 90s after every boot)"

echo ""
log "Done. URLs:"
echo "    Grafana (deep dive)   : http://$(hostname -I | awk '{print $1}'):3000   (refresh: 5 min)"
echo "    Fleet map + alerts    : http://$(hostname -I | awk '{print $1}'):8090"
echo "    Enrollment (for Pis)  : https://eads-master.duckdns.org:8443/enroll"
