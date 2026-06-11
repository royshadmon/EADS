# Nebula Overlay Setup for EADS

This guide gets the Nebula mesh running. You do this ONCE on the master VM,
then sign one cert per Pi as you enroll them. After that, deployers just
"plug in and connect WiFi" — no router config at remote sites.

## Architecture

```
                      Internet
                          |
            +-------------+-------------+
            |  Master VM (lighthouse)   |
            |  Public: eads-master.duckdns.org
            |  Public UDP 4242 forwarded
            |  Nebula IP: 10.42.0.1     |
            +-------------+-------------+
                          |  (Nebula overlay, encrypted)
            +-------------+-------------+
            |                           |
   +--------+--------+         +--------+--------+
   | Pi (anywhere)   |         | Pi (anywhere)   |
   | Nebula: 10.42.0.10        | Nebula: 10.42.0.11
   | (NAT, no fwd)             | (NAT, no fwd)
   +-----------------+         +-----------------+
```

Subnet: `10.42.0.0/24` — chosen to avoid your existing 10.0.0.X home and lab LANs.

## One-time master setup

### 1. Install nebula on the master VM

```bash
# On master VM (Ubuntu)
cd /tmp
wget https://github.com/slackhq/nebula/releases/download/v1.9.5/nebula-linux-amd64.tar.gz
tar -xzf nebula-linux-amd64.tar.gz
sudo install -m 755 nebula nebula-cert /usr/local/bin/
```

### 2. Generate the CA (do this ONCE — and **back up the CA key**!)

```bash
sudo mkdir -p /etc/nebula
cd /etc/nebula
sudo nebula-cert ca -name "EADS-CA" -duration "8760h"   # 1 year
# Produces ca.crt and ca.key. BACK UP ca.key SECURELY — if it leaks, anyone
# can sign certs that join your network. Restrict permissions:
sudo chmod 600 ca.key
```

### 3. Sign the lighthouse cert (the master)

```bash
sudo nebula-cert sign -name "master" -ip "10.42.0.1/24" -groups "master"
# Produces master.crt and master.key
```

### 4. Forward UDP 4242 on your Xfinity router

Xfinity admin → port forwarding → forward **UDP 4242** to your master VM's
LAN IP (the VM's `10.0.0.X`, not the PC's). Nebula uses UDP 4242 by default
for hole-punching and lighthouse coordination.

### 5. Create the lighthouse config

```bash
sudo tee /etc/nebula/config.yml > /dev/null << 'CFG'
pki:
  ca: /etc/nebula/ca.crt
  cert: /etc/nebula/master.crt
  key: /etc/nebula/master.key

# The lighthouse has no static_host_map entries — it IS the rendezvous point.
static_host_map: {}

lighthouse:
  am_lighthouse: true
  interval: 60

listen:
  host: 0.0.0.0
  port: 4242

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
    - port: 32048    # AnyLog master TCP
      proto: tcp
      host: any
    - port: 32049    # AnyLog master REST (if exposed)
      proto: tcp
      host: any
CFG
```

### 6. Run nebula as a systemd service on the master

```bash
sudo tee /etc/systemd/system/nebula.service > /dev/null << 'UNIT'
[Unit]
Description=Nebula overlay (EADS lighthouse)
Wants=basic.target network-online.target
After=basic.target network.target network-online.target

[Service]
SyslogIdentifier=nebula
ExecReload=/bin/kill -HUP $MAINPID
ExecStart=/usr/local/bin/nebula -config /etc/nebula/config.yml
Restart=always

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now nebula
sudo systemctl status nebula
```

Verify the `nebula1` interface exists and has the right IP:

```bash
ip addr show nebula1
# should show inet 10.42.0.1/24
```

### 7. Update AnyLog master to also bind on the overlay

Your AnyLog master should accept connections on `10.42.0.1:32048` (the
nebula1 interface) in addition to whatever it already listens on. If it
binds to `0.0.0.0:32048` it already does — no change needed. If it binds
to a specific IP, change to `0.0.0.0`.

## Enrolling a new Pi

When you ship a new Pi, do these steps before/during deployment:

### 1. Assign it a Nebula IP and sign a cert

Pick the next free IP in `10.42.0.10` upward. On the master:

```bash
cd /etc/nebula
sudo nebula-cert sign \
    -name "operator-<short-uuid>" \
    -ip   "10.42.0.<N>/24" \
    -groups "operator"
# Produces operator-<uuid>.crt and operator-<uuid>.key
```

Record which Pi UUID got which Nebula IP. A simple text file is fine.

### 2. Put the cert + key + CA into Balena per-device variables

Enroll the device with the provisioning kit (preferred — one command):

```
./provision_setenv.sh <N> <device-uuid>
```

This sets `NEBULA_CONFIG` (the full self-contained config with certs
inlined), `EADS_NEBULA_IP`, `EADS_NODE_NAME`, and `LEDGER_CONN` as per-device
variables. The supervisor caches them on the Pi — they survive reboots, data
purges, and cloud outages.

Legacy fallback (still supported by the container): set `NEBULA_CA_CRT`,
`NEBULA_HOST_CRT`, and `NEBULA_HOST_KEY` individually in the dashboard.

### 3. Power on the Pi at its install site

It boots, joins WiFi, pulls the Balena release, starts the nebula container
which reads its certs from env vars, joins the overlay, gets `10.42.0.<N>`,
and the operator registers with the master over the overlay. No router
config at the install site is needed.

## Verification

On the master VM:
```bash
ping 10.42.0.10                              # should reach an enrolled Pi
sudo nebula-cert print -path /etc/nebula/ca.crt   # CA info
```

On the Pi (via `balena exec` into the nebula container):
```bash
ping 10.42.0.1                               # should reach the lighthouse
ip addr show nebula1                         # see assigned overlay IP
```

In the operator container's logs:
```
[Identity] NEBULA_IP   : 10.42.0.10  (advertised to master; TCP_BIND=true)
```

On the master AnyLog CLI:
```
test network
```
Each operator should appear with its `10.42.0.X` Nebula IP and status `+`.

## Honest caveats

1. **Master-side router config is still required** — UDP 4242 forwarded
   on your Xfinity router. This is once, on YOUR router, not on any
   deployer's router. Without it, nodes can't reach the lighthouse to
   bootstrap.

2. **CA key security** — `/etc/nebula/ca.key` on the master VM is the root
   of trust. If it leaks, anyone with it can sign certs and join your
   network. Restrict its permissions (chmod 600) and back it up offline.

3. **Cert expiry** — host certs expire (default ~1 year). When they expire,
   the Pi drops off the overlay. You re-sign and update the Balena var.

4. **First connection takes 30-60s** — Nebula has to discover, hole-punch,
   and establish. The operator's `Waiting for Nebula overlay IP` retry loop
   gives it 30s. If your link is slow, increase that in deploy_anylog.sh.

5. **NAT hole-punching usually works but isn't guaranteed.** Symmetric NATs
   (some corporate / cellular networks) defeat hole-punching. In those
   cases you'd need a relay node — but most home/office routers are fine.
