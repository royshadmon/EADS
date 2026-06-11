# EADS Master Package

Everything that runs on the master VM (Ubuntu, `eads-master.duckdns.org`):
the Nebula lighthouse, both server-side AnyLog nodes, the Grafana deep-dive
dashboard, and the Fleet service (outage map + alerting + plug-and-play
enrollment).

## Which AnyLog nodes must run, and why

| Node | Where | Role |
|---|---|---|
| **anylog-master** | this VM | The blockchain/ledger. Operators register here and sync policies from it every **5 minutes** (`BLOCKCHAIN_SYNC` on the Pis) — this is the "updates propagate to all boxes" channel. |
| **anylog-query** | this VM | **Required for the website.** Grafana and the Fleet service send SQL to this node with `destination: network`; it fans the query out to every Pi operator and merges the results. The dashboard showed "Disconnected" before precisely because this node was never running. |
| **operators** | the Pis | One per box; store data locally, answer fan-out queries. |

So the answer to "what needs to be spun up": **master + query here, operators
on the Pis.** This compose file runs both server nodes.

## Setup order

```
1. cp .env.example .env && nano .env       # license, ENROLL_TOKEN, alerts
2. Sign the lighthouse identity on the MacBook (CA never leaves it):
     cd <provisioning kit>
     nebula-cert sign -name eads-lighthouse -ip 10.42.0.1/24 \
         -ca-crt ca.crt -ca-key ca.key \
         -out-crt lighthouse.crt -out-key lighthouse.key
     scp ca.crt lighthouse.crt lighthouse.key <vm>:/tmp/
     # on the VM: sudo mkdir -p /etc/nebula && sudo mv /tmp/{ca.crt,lighthouse.crt,lighthouse.key} /etc/nebula/
3. Export the identity pool from the MacBook:
     ./provision_pool.sh 2 30
     scp identity-pool.tar.gz <vm>:~/eads-master/
     # on the VM: cd ~/eads-master && tar xzf identity-pool.tar.gz -C identity-pool --strip-components=1
4. ./master_setup.sh                       # idempotent; installs nebula,
                                           # systemd unit, UFW rules, stack
5. Router (Xfinity app): forward UDP 4242 and TCP 8443 to this VM.
```

`master_setup.sh` ends with a verification pass: container status (now including the archiver), master
REST (32049), query REST (32449), and the fleet service (8090).

## ⚠️ Before enabling enrollment: pre-seed node-02

`eads-node-02` was already enrolled **manually** (via `provision_setenv.sh`)
to the first Pi. The enrollment server doesn't know that, so it would vend
node-02's identity to the next box that asks — an identity collision on the
overlay. Seed the assignment once, before the first auto-enrollment:

```bash
docker compose up -d fleet     # creates the fleet-data volume
docker compose exec fleet python -c "
import json, pathlib
p = pathlib.Path('/data/assignments.json')
a = json.loads(p.read_text()) if p.exists() else {}
a['3d0f39a5b12eb16ed64ce4239ec19ee9'] = 'eads-node-02'
p.write_text(json.dumps(a, indent=2))
print(a)"
```

(That UUID is the first Pi's Balena device UUID. If you enroll any other
node manually in the future, seed it the same way.)

## Data retention policy (what's stored where, for how long)

| Tier | Where | Retention | Mechanism |
|---|---|---|---|
| Routine voltage + health | Pi (operator) | **7 days** | AnyLog daily partitions, `PARTITION_KEEP=7` (set in the Pi compose) |
| **Un-acked anomaly segments** | Pi SD (ingestor) | **NEVER purged** | Protected by design: segments are deleted only after AnyLog acks the final batch. The old 500 MB cap no longer deletes — it warns. The disk safeguard (pause at 85%) stops *new* writes instead of eating captured evidence. |
| Everything | Master Postgres (`eads_archive` schema) | Permanent (your call) | **Archiver service**: copies new rows off the fleet every 5 min via the query node; exactly-once (watermark + dedup); resumes after restarts; tolerates query-node downtime because the Pis buffer 7 days. |

The order matters: local 7-day overwrite is *safe* because the archiver
drains everything to Postgres long before partitions drop, and an SD card
death costs at most one archive cycle (~5 min) of routine data — never an
already-captured anomaly that reached AnyLog.

Verify the archive is flowing: `docker logs eads-archiver` and
`psql -U eads -d eads_data -c "select * from eads_archive.archive_state;"`

## Outage detection: two layers

The fleet poller classifies on **two signals**, not one:

1. **Data freshness** (query node): newest `node_health` row per node.
2. **Direct overlay probe**: for any node not fresh, the fleet service
   dials its operator REST (`10.42.0.N:32149, get status`) over Nebula.
   The participant's network needs nothing for this — no port forwarding,
   no exposure; the probe rides back through the Pi's own outbound tunnel
   to the lighthouse (relayed automatically on hostile NATs).

| Freshness | Probe | Status | Meaning → action |
|---|---|---|---|
| fresh | — | `ONLINE` | all good |
| stale | answers | `PIPELINE_STALLED` | box alive, data not flowing → fix via Balena |
| stale | dead | `STALE`/`OUTAGE` | power or internet down at the site → contact participant |

Alerts name the distinction explicitly, and an external system (e.g. an
AMS station) can consume the same structured webhook (`ALERT_WEBHOOK_URL`)
or poll `GET /api/fleet`.

## Anomaly-event alerts

Beyond liveness, the fleet poller watches `current_anomaly_id` in health
rows: a **new** anomaly id on any node fires exactly one
`⚡ ANOMALY DETECTED` alert (webhook + email, same channels) — you hear
about the science within one poll cycle (~60 s), not when you next open
the dashboard. Seen ids persist in `/data/anomaly_state.json` (last 50
per node), so restarts don't re-alert.

## GPS auto-location

Boxes locate themselves: the gpsd container publishes the fix, each
health row carries `lat`/`lon`/`gps_fix`, and the fleet poller auto-fills
the registry (`coords_source: gps`) — the outage map populates with zero
manual entry. Coordinates you set by hand (`coords_source: manual`, the
default for POSTed entries) always win and are never overwritten. Indoors
with no fix, rows carry `gps_fix: 0` and are ignored; enter that one
node's coordinates manually.

## Duplicate-policy guard (restart bug)

`anylog/fix_duplicate_policies.py` replaces the wipe-the-volumes
workaround: it keeps the **oldest** copy of each master/cluster/operator
policy (the one existing nodes reference) and drops the rest. Idempotent;
`--dry-run` to inspect first. `master_setup.sh` runs it after stack start
AND installs it as a systemd oneshot (90 s after every VM boot), so
restarts self-heal. The true fix remains a conditional declaration inside
the image's deployment-scripts — documented in the script header — but
with the guard in place the bug is contained either way. If drops fail,
the `DROP_CMD` env template adjusts the dialect without code changes.

## The two websites

| URL | What | Refresh |
|---|---|---|
| `http://<vm>:8090` | **Fleet service** — Leaflet outage map, node status table (ONLINE / STALE / OUTAGE), alert engine | every 5 min (page), poller every 60 s |
| `http://<vm>:3000` | **Grafana** — deep-dive panels on the real schema: node_health table, voltage_est, anomaly flag, sampling rate, backlog, disk, drain rate | every 5 min |

### Outage mapping
The map needs coordinates per node. Add them once per deployment:

```bash
curl -X POST http://<vm>:8090/api/registry -H 'Content-Type: application/json' \
  -d '{"eads-node-02": {"label": "Participant A, Santa Cruz", "lat": 36.9741, "lon": -122.0308}}'
```

Enrollment automatically records each node's overlay IP and device UUID in
the same registry. Nodes in the registry that have **never** reported are
surfaced as OUTAGE (so a box that dies before its first heartbeat is still
visible).

### Alerting
Fires on **state transitions only** (no repeat spam): node → OUTAGE after
10 min silent (`OUTAGE_MIN`), plus a recovery notice when it returns.
Channels (both optional, set in `.env`):
- `ALERT_WEBHOOK_URL` — Slack/Discord-compatible JSON webhook
- SMTP email — Gmail App Password (`myaccount.google.com/apppasswords`)

If the **query node itself** is unreachable, nodes are marked
`UNKNOWN (query node unreachable)` rather than falsely declared outages,
and the page shows a warning banner.

### 5-minute updates
Three places, all aligned: the Fleet page auto-refreshes every 300 s, the
Grafana dashboard refresh is `5m`, and the Pis' `BLOCKCHAIN_SYNC=5 minutes`
means any policy added to the master reaches every box within one cycle.

## Plug-and-play enrollment (how a box joins with zero per-device work)

```
Pi boots → no identity found → POST https://eads-master.duckdns.org:8443/enroll
   {token: EADS_ENROLL_TOKEN (fleet-wide Balena var, set ONCE ever),
    uuid:  the box's hardware UUID}
→ fleet service vends the lowest free identity from identity-pool/
→ Pi caches it on-device; same UUID always gets the same identity back
```

Transport is TLS with the certificate **pinned** in the Pi image
(`enroll-ca.crt`, generated from `enroll-tls/` here) — a wrong or
impersonating server fails closed. `enroll-tls/enroll.key` is **private
material**: it stays on this VM only.

Set the token (must match the Balena fleet var `EADS_ENROLL_TOKEN`):

```bash
openssl rand -hex 24    # → ENROLL_TOKEN in .env  AND  balena env add EADS_ENROLL_TOKEN <value> --fleet EADS
```

## Migrating from the existing `~/eads` stack

Your VM already runs anylog-master + postgres from `~/eads/docker-compose.yml`
under the `eads.service` systemd unit. This package is a **superset**. To
adopt it without breaking the running master:

1. `sudo systemctl stop eads && sudo systemctl disable eads`
2. `docker compose -f ~/eads/docker-compose.yml down` (volumes survive)
3. Bring this stack up (`./master_setup.sh`). Note two deliberate changes:
   - `OVERLAY_IP=10.42.0.1` (was `10.0.0.179`) — the master must advertise
     its **overlay** address now that operators reach it through Nebula.
   - Postgres credentials/volume names match the old stack, but the volume
     is a fresh named volume; if you need the old master's ledger state,
     either keep the old volumes by editing the `volumes:` section to bind
     the previous paths, or start clean (the known duplicate-policy-on-
     restart bug makes a clean start the safer option anyway).
4. Optional: re-point or remove the old `eads.service`; this package can be
   wrapped in an equivalent unit (`docker compose up -d` in
   `ExecStart`) if you want boot-time autostart again.

## Honest caveats (read before trusting it blindly)

- **AnyLog SQL dialect**: the Fleet poller's health query is the env-tunable
  template `HEALTH_SQL` because AnyLog's time-window syntax varies by build
  and I could not verify it against a real AnyLog here. If the poller logs
  query errors, adjust that one env var — no code change.
- **Grafana time columns**: panels use `insert_timestamp` (AnyLog's ingest
  time), which always exists. The Pi's native microsecond clock is in
  `timestamp_us`; switch a panel's `time_column` only if your AnyLog maps it
  to a timestamp type.
- **OVERLAY_IP advertising** is verified in config logic, not on your
  hardware. After the first Pi connects, run `test network` in the AnyLog
  CLI and confirm the operator shows at `10.42.0.<N>:32148`.
- The duplicate-blockchain-policy-on-restart bug is **not** fixed by this
  package; clean-start remains the workaround.
