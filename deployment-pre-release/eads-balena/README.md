# EADS Pi Node — Balena Release (final, deployment-ready)

Seven-container release for the Raspberry Pi 3 A+ fleet (Balena fleet: EADS).
This version supersedes all previous releases. Verified in CI-style testing:
ingestor compiled clean at `-O3 -Wall -Wextra`, unit suites pass, and a
45-second functional soak (synthetic 7,680 Hz sampler + fake AnyLog) confirmed
the full normal/anomaly/drain lifecycle with row-exact arithmetic.

## Services and CPU budget

| Service | Cores | Role |
|---|---|---|
| `sampler` | 2,3 | `sampling_bin` (RT FIFO 60, core 2 alone) + `eads_ingestor` (all threads core 3) |
| `anylog-operator` | 0,1 | Local AnyLog store; syncs to master over the overlay |
| `nebula` | 0 | Overlay mesh client; stable `10.42.0.<N>` reachable from anywhere |
| `gpsd` | 0 | GPS HAT on UART → SHM time feed |
| `chrony` | 0 | Disciplines the system clock from GPS (NTP fallback); `makestep` for cold boots |
| `dashboard` | 0 | **NEW** — local health page at `http://<pi-lan-ip>:8080`, no cloud needed |
| `wifi-connect` | 0 | **NEW** — captive-portal WiFi onboarding ("EADS-Setup" AP) when the box has no internet |

Core 2 is reserved exclusively for `sampling_bin`: its DMA double-buffer copy
loop is hard real-time, and sharing that core is what caused the exit-134
assertion crashes. Do not move anything onto core 2.

## Architecture

```
              AnyLog master + Nebula lighthouse
                 10.42.0.1  (eads-master.duckdns.org, UDP 4242)
                          │
        ──────────────────┼──────────────────
        Nebula overlay 10.42.0.0/24 (encrypted, NAT-traversing)
        │                                    │
   Pi @ 10.42.0.2                       Pi @ 10.42.0.3 ...
   (node 1 reserved for the lighthouse; participant Pis start at 2)
```

All containers run `network_mode: host`. The `nebula1` tun interface lands in
the host namespace where every sibling container sees it — no `service:`
coupling (which Balena's parser rejects alongside `ipc:` settings anyway).

## Credential delivery (why there are no cert files here)

Balena containers cannot read SD-card boot-partition files (no bind mounts of
`/mnt/boot`, no bootfs feature label). Each Pi's Nebula identity arrives as
the `NEBULA_CONFIG` device env var — a complete config.yml with the CA cert,
host cert, and host key inlined as PEM — set once at enrollment by
`provisioning/provision_setenv.sh`. The supervisor caches device vars
on-device: they survive reboots, data purges, and cloud outages. The nebula
container validates the config (`nebula -test`) before starting the daemon.

## Deployment — plug-and-play (zero per-device Balena work)

One-time fleet setup:
1. `balena push EADS` from this directory.
2. On the MacBook: `./provision_pool.sh 2 30` and export the pool to the
   master (`eads-master/identity-pool/` — see the master README).
3. Set the fleet enrollment token ONCE:
   `balena env add EADS_ENROLL_TOKEN <openssl rand -hex 24> --fleet EADS`
   (same value as `ENROLL_TOKEN` in the master's `.env`).

Per box, forever after: **flash the generic fleet image, hand it to the
participant.** They plug it in; if it has no internet it raises the
"EADS-Setup" WiFi portal — they join it from a phone, pick their home WiFi,
done. The box then auto-enrolls against the master over pinned TLS, receives
its Nebula identity, caches it on-device, and joins the fleet. A reflashed
or purged box re-enrolls and gets the SAME identity back (assignments are
keyed to the hardware UUID).

Balena's role shrinks to what you wanted: pushing updates and remote
debugging. The manual path (`provision_setenv.sh <N> <uuid>`) still works
and overrides auto-enrollment — useful for pinning a specific box to a
specific identity.

Verify per device:
   - `balena logs <uuid> --service nebula` → `[Nebula] Overlay IP : 10.42.0.<N>`
   - `balena logs <uuid> --service anylog-operator` → `NEBULA_IP : 10.42.0.<N>`
   - From the master: `ping 10.42.0.<N>`, then `test network` in the AnyLog
     CLI → operator listed at `10.42.0.<N>:32148`.
   - Browse `http://<pi-lan-ip>:8080` → live dashboard with service badges.

## Normal operating behavior

Samples continuously at 7,680 Hz; pushes ~512 voltage rows + 1 health row to
the local operator every 10 s in NORMAL mode. On anomaly: flushes the 2 s
pre-trigger ring, streams flagged samples to an SD-backed segment, and drains
the segment to AnyLog at an adaptive rate capped at the tested 3,000 rows/s.
Backlog capped at 500 MB (oldest segments purged beyond that). A rotating
metrics CSV (30 s cadence, 10 MB cap, ~46 days) records app + system health
plus cross-service probes (nebula up, operator up, master reachable, clock
synced) — the dashboard reads its latest row.

## Data retention (on this box)

Routine data lives **7 days** locally (AnyLog daily partitions,
`PARTITION_KEEP=7`) — the master's archiver copies everything off within
minutes, so local data is a buffer, not the record. **Un-acked anomaly
segments are never purged**: the backlog cap warns instead of deleting,
and the disk safeguard pauses new writes rather than discarding captured
evidence. A box that rides out a week-long internet outage delivers its
anomalies when connectivity returns (the drain clears ~1 GB/hour).

## Timestamps

`sampling_bin` stamps samples with the BCM2835 free-running counter (µs since
boot). The ingestor translates per sample using the
`CLOCK_REALTIME − CLOCK_MONOTONIC` offset — both clocks derive from the same
1 MHz hardware timer, and chrony+GPS keep `CLOCK_REALTIME` true. This is the
fix for the year-2031 bug; do not "simplify" it away.

## What changed vs. the previous release

- All services on `network_mode: host` (fixes the Balena compose parse error)
- `LEDGER_CONN` corrected to `10.42.0.1:32048` everywhere (was 10.99.x)
- Nebula reads `NEBULA_CONFIG` (inlined PEM) with boot-time `nebula -test`
  validation; legacy 3-var delivery still works as a fallback
- `deploy_anylog.sh` takes `EADS_NEBULA_IP` as a fast path before polling the
  shared volume
- NEW `dashboard` service (single C binary, ~2 MB RSS, port 8080)
- Ingestor: cached per-table HTTP header lists (no per-PUT alloc/free churn);
  stale 10.99 fallback removed
- chrony: `makestep 1.0 -1` (step any >1 s offset — a Pi with no RTC boots
  minutes-to-years wrong; slewing would take days) and measurement/statistics
  logging removed (continuous SD writes for no operational value)
- gpsd image: dropped `gpsd-clients` (debug-only tools)
- NEW `wifi-connect` service: end-user WiFi onboarding via captive portal
- NEW plug-and-play auto-enrollment: nebula container phones home to the
  master's enrollment service (pinned TLS) when it has no identity; cached
  on-device afterwards; idempotent per hardware UUID
- Ingestor + dashboard prefer the enrolled node name (`/shared-nebula/
  node_name`) so fleet queries are human-readable
- Removed: `publisher/` (retired Python path), duplicate top-level
  `sampling/`, stale `docker-compose.prod.yml`

## Honest caveats

- AnyLog binding/advertising the overlay IP is verified in logic, not on your
  hardware — confirm the first Pi registers at `10.42.0.<N>` (`test network`)
  before scaling out.
- GPS lock indoors may never happen; chrony then needs internet NTP. Check
  `chronyc tracking` at the install location.
- Master-side router must forward UDP 4242 (the one unavoidable rule).
- Nebula certs default to 1-year validity — re-sign and re-run
  `provision_setenv.sh` when one lapses.
- Deploy to ONE Pi and soak ≥24 h watching `metrics.csv` before fleet-wide.
