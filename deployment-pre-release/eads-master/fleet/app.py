"""
EADS Fleet Service — runs on the master VM.

One small app, four jobs:

  1. ENROLLMENT  (POST /enroll, TLS, token-gated)
     Vends pre-generated Nebula identities to new Pis. The MacBook generates
     a pool of identities offline (provision_pool.sh) and exports it here;
     the CA key NEVER touches this server. A Pi that re-enrolls with the
     same hardware UUID gets the same identity back (idempotent), so a
     reflashed box recovers itself with zero operator work.

  2. FLEET STATUS  (GET /, /api/fleet)
     Polls the AnyLog QUERY NODE every POLL_SEC for the latest node_health
     rows, classifies each node ONLINE / STALE / OUTAGE, and serves a
     Leaflet outage map + status table. The page auto-refreshes every
     5 minutes (REFRESH_SEC).

  3. ALERTING
     On state transitions only (no spam): node → OUTAGE fires an alert;
     node → ONLINE after outage fires a recovery notice. Channels: webhook
     (Slack/Discord-compatible JSON) and SMTP email. Both optional via env.

  4. REGISTRY  (GET/POST /api/registry)
     node_name → {label, lat, lon} for the map. Enrollment auto-adds the
     overlay IP and device UUID; you add the human bits (participant
     address coordinates) once per deployment.

AnyLog SQL dialect note: the health query is an env-tunable template
(HEALTH_SQL) because AnyLog's time-window syntax varies by version. The
default targets the standard form; if your query node rejects it, fix the
env var — no code change.
"""

import asyncio
import json
import os
import smtplib
import ssl
import time
from datetime import datetime, timezone
from email.message import EmailMessage
from pathlib import Path

import httpx
import uvicorn
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse

# ── configuration (env) ──────────────────────────────────────────────────
QUERY_CONN   = os.environ.get("QUERY_CONN", "127.0.0.1:32449")
DBMS         = os.environ.get("DEFAULT_DBMS", "eads")
POLL_SEC     = int(os.environ.get("POLL_SEC", "60"))
REFRESH_SEC  = int(os.environ.get("REFRESH_SEC", "300"))      # page auto-refresh: 5 min
STALE_MIN    = float(os.environ.get("STALE_MIN", "3"))        # no health row for N min → STALE
OUTAGE_MIN   = float(os.environ.get("OUTAGE_MIN", "10"))
OPERATOR_REST_PORT = int(os.environ.get("OPERATOR_REST_PORT", "32149"))
PROBE_TIMEOUT = float(os.environ.get("PROBE_TIMEOUT", "5"))      # → OUTAGE + alert
ENROLL_TOKEN = os.environ.get("ENROLL_TOKEN", "")
WEBHOOK_URL  = os.environ.get("ALERT_WEBHOOK_URL", "")
SMTP_HOST    = os.environ.get("SMTP_HOST", "")
SMTP_PORT    = int(os.environ.get("SMTP_PORT", "587"))
SMTP_USER    = os.environ.get("SMTP_USER", "")
SMTP_PASS    = os.environ.get("SMTP_PASS", "")
EMAIL_FROM   = os.environ.get("EMAIL_FROM", SMTP_USER)
EMAIL_TO     = os.environ.get("EMAIL_TO", "")
HEALTH_SQL   = os.environ.get(
    "HEALTH_SQL",
    # select * so columns added later (e.g. gps lat/lon) flow through
    # without touching this template
    'select * from node_health where timestamp >= NOW() - 30 minutes',
)

DATA = Path(os.environ.get("DATA_DIR", "/data"))
POOL = Path(os.environ.get("POOL_DIR", "/pool"))
DATA.mkdir(parents=True, exist_ok=True)

ASSIGN_F   = DATA / "assignments.json"   # uuid → node_name
REGISTRY_F = DATA / "registry.json"      # node_name → {label, lat, lon, nebula_ip, uuid}


def _load(p: Path, dflt):
    try:
        return json.loads(p.read_text())
    except Exception:
        return dflt


def _save(p: Path, obj):
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(obj, indent=2))
    tmp.replace(p)


# ── fleet state (in-memory, rebuilt by the poller) ───────────────────────
fleet: dict[str, dict] = {}     # node_id → latest health + status
fleet_lock = asyncio.Lock()
last_poll = {"ok": False, "at": None, "error": "not yet polled"}
alert_state: dict[str, str] = _load(DATA / "alert_state.json", {})
# node → list of recently seen anomaly ids (alert once per event)
anomaly_seen: dict[str, list] = _load(DATA / "anomaly_state.json", {})

app = FastAPI(title="EADS Fleet")


# ── AnyLog query-node client ─────────────────────────────────────────────
async def anylog_query(client: httpx.AsyncClient, sql: str):
    """GET to the query node REST with the SQL in the command header,
    destination: network so it fans out to every operator."""
    headers = {
        "User-Agent": "AnyLog/1.23",
        "command": f'sql {DBMS} format=json and stat=false "{sql}"',
        "destination": "network",
    }
    r = await client.get(f"http://{QUERY_CONN}", headers=headers, timeout=20)
    r.raise_for_status()
    body = r.json()
    # AnyLog returns {"Query": [...]} (older builds: a bare list)
    if isinstance(body, dict):
        return body.get("Query", body.get("query", []))
    return body if isinstance(body, list) else []


async def probe_operator(client: httpx.AsyncClient, nebula_ip: str) -> bool:
    """Layer 2: direct liveness probe of a Pi's AnyLog operator REST over
    the Nebula overlay. The fleet container runs host-net on the lighthouse
    host, so it is already on the mesh; every Pi's overlay firewall allows
    32149/tcp inbound. The participant's network needs NOTHING — no port
    forwarding, no exposure: the Pi's outbound tunnel to the lighthouse is
    the path this probe rides back through (relayed if NAT is hostile)."""
    try:
        r = await client.get(f"http://{nebula_ip}:{OPERATOR_REST_PORT}",
                             headers={"User-Agent": "AnyLog/1.23",
                                      "command": "get status"},
                             timeout=PROBE_TIMEOUT)
        return r.status_code == 200
    except Exception:
        return False


def classify(age_min: float) -> str:
    if age_min >= OUTAGE_MIN:
        return "OUTAGE"
    if age_min >= STALE_MIN:
        return "STALE"
    return "ONLINE"


async def poll_once():
    global last_poll
    async with httpx.AsyncClient() as client:
        try:
            rows = await anylog_query(client, HEALTH_SQL)
            last_poll = {"ok": True,
                         "at": datetime.now(timezone.utc).isoformat(),
                         "error": None}
        except Exception as e:
            last_poll = {"ok": False,
                         "at": datetime.now(timezone.utc).isoformat(),
                         "error": str(e)}
            # Query node unreachable: don't invent outages from missing
            # data — mark fleet stale-unknown but keep last knowledge.
            async with fleet_lock:
                for n in fleet.values():
                    n["status"] = "UNKNOWN (query node unreachable)"
            return

    # newest row per node
    latest: dict[str, dict] = {}
    for r in rows:
        nid = str(r.get("node_id", "?"))
        try:
            ts = int(str(r.get("timestamp_us", 0)))
        except ValueError:
            ts = 0
        if nid not in latest or ts > latest[nid]["_ts"]:
            rec = dict(r)
            rec["_ts"] = ts
            latest[nid] = rec

    now_us = time.time() * 1e6
    registry = _load(REGISTRY_F, {})

    # ── Layer 1: data freshness (computed for every node that reported) ──
    fresh: dict[str, dict] = {}
    for nid, rec in latest.items():
        age_min = max(0.0, (now_us - rec["_ts"]) / 60e6)
        rec["age_min"] = round(age_min, 1)
        rec["freshness"] = classify(age_min)
        fresh[nid] = rec
    # registry nodes that never reported at all
    for name, meta in registry.items():
        if name not in fresh:
            fresh[name] = {"node_id": name, "age_min": None,
                           "freshness": "OUTAGE", "_ts": 0, "mode": "-"}

    # ── Layer 2: direct overlay probe — only needed for non-fresh nodes ──
    # Distinguishes a dead/unreachable box (power or network outage at the
    # participant's home) from a reachable box whose data pipeline stalled
    # (sampler/ingestor problem → a Balena fix, not a phone call).
    stale_nodes = [n for n, r in fresh.items() if r["freshness"] != "ONLINE"]
    probe_ok: dict[str, bool] = {}
    if stale_nodes:
        async with httpx.AsyncClient() as pc:
            ips = {n: (registry.get(n, {}) or {}).get("nebula_ip")
                   for n in stale_nodes}
            results = await asyncio.gather(
                *(probe_operator(pc, ip) if ip else asyncio.sleep(0, False)
                  for ip in ips.values()))
            probe_ok = dict(zip(ips.keys(), results))

    # ── GPS auto-fill: nodes report their own coordinates in health rows.
    # Manual registry coords always win; GPS fills blanks and refreshes
    # entries it previously filled (meta flag coords_source=gps).
    reg_dirty = False
    for nid, rec in fresh.items():
        try:
            fix = int(float(str(rec.get("gps_fix", 0))))
            lat = float(str(rec.get("lat", 0)))
            lon = float(str(rec.get("lon", 0)))
        except (ValueError, TypeError):
            continue
        if fix < 2 or (lat == 0 and lon == 0):
            continue                      # no usable fix reported
        meta = registry.setdefault(nid, {})
        manually_set = ("lat" in meta and
                        meta.get("coords_source", "manual") == "manual")
        if not manually_set and (meta.get("lat") != lat or
                                 meta.get("lon") != lon):
            meta.update({"lat": lat, "lon": lon, "coords_source": "gps"})
            reg_dirty = True
    if reg_dirty:
        _save(REGISTRY_F, registry)

    # ── Anomaly-event alerts: a NEW current_anomaly_id on a node fires
    # exactly one alert (the science is happening — tell someone).
    anomaly_events = []
    anom_dirty = False
    for nid, rec in fresh.items():
        aid = str(rec.get("current_anomaly_id", "") or "")
        if not aid or aid in ("none", "-", "0"):
            continue
        seen = anomaly_seen.setdefault(nid, [])
        if aid not in seen:
            seen.append(aid)
            del seen[:-50]               # remember the last 50 per node
            anomaly_events.append((nid, aid, rec.get("mode", "?")))
            anom_dirty = True
    if anom_dirty:
        _save(DATA / "anomaly_state.json", anomaly_seen)

    transitions = []
    async with fleet_lock:
        for nid, rec in fresh.items():
            f = rec["freshness"]
            if f == "ONLINE":
                status = "ONLINE"
            elif probe_ok.get(nid):
                status = "PIPELINE_STALLED"
            else:
                status = f          # STALE (gray zone) or OUTAGE
            rec["status"] = status
            rec["probe_ok"] = probe_ok.get(nid) if nid in probe_ok else None
            rec["meta"] = registry.get(nid, {})
            prev = alert_state.get(nid, "ONLINE")
            if status != prev:
                transitions.append((nid, prev, status))
                alert_state[nid] = status
            fleet[nid] = rec
    if transitions:
        _save(DATA / "alert_state.json", alert_state)
        await fire_alerts(transitions)
    if anomaly_events:
        await fire_anomaly_alerts(anomaly_events)


# ── alerting ─────────────────────────────────────────────────────────────
async def fire_alerts(transitions):
    lines = []
    for nid, prev, cur in transitions:
        if cur == "OUTAGE":
            lines.append(f"🔴 OUTAGE: {nid} silent AND unreachable over the "
                         f"overlay — likely power or internet down at the "
                         f"site (was {prev}).")
        elif cur == "PIPELINE_STALLED":
            lines.append(f"🟠 PIPELINE STALLED: {nid} is reachable but not "
                         f"reporting — box is alive; check sampler/ingestor "
                         f"via Balena (was {prev}).")
        elif cur == "ONLINE" and prev in ("OUTAGE", "STALE",
                                          "PIPELINE_STALLED"):
            lines.append(f"🟢 RECOVERED: {nid} is reporting again.")
        elif cur == "STALE":
            lines.append(f"🟡 STALE: {nid} late by >{STALE_MIN} min.")
    if not lines:
        return
    text = "EADS fleet alert\n" + "\n".join(lines)
    if WEBHOOK_URL:
        try:
            async with httpx.AsyncClient() as c:
                await c.post(WEBHOOK_URL, json={"text": text, "content": text},
                             timeout=10)
        except Exception as e:
            print(f"[alert] webhook failed: {e}")
    if SMTP_HOST and EMAIL_TO:
        try:
            await asyncio.to_thread(_send_email, "EADS fleet alert", text)
        except Exception as e:
            print(f"[alert] email failed: {e}")
    print(f"[alert] {text}")


def _send_email(subject: str, body: str):
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = EMAIL_FROM
    msg["To"] = EMAIL_TO
    msg.set_content(body)
    with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as s:
        s.starttls(context=ssl.create_default_context())
        if SMTP_USER:
            s.login(SMTP_USER, SMTP_PASS)
        s.send_message(msg)


async def fire_anomaly_alerts(events):
    lines = [f"⚡ ANOMALY DETECTED: {nid} event id={aid} (mode {mode}) — "
             f"pre-trigger + live samples are streaming to AnyLog now."
             for nid, aid, mode in events]
    text = "EADS anomaly alert\n" + "\n".join(lines)
    if WEBHOOK_URL:
        try:
            async with httpx.AsyncClient() as c:
                await c.post(WEBHOOK_URL, json={"text": text, "content": text},
                             timeout=10)
        except Exception as e:
            print(f"[alert] anomaly webhook failed: {e}")
    if SMTP_HOST and EMAIL_TO:
        try:
            await asyncio.to_thread(_send_email, "EADS anomaly alert", text)
        except Exception as e:
            print(f"[alert] anomaly email failed: {e}")
    print(f"[alert] {text}")


async def poller():
    while True:
        try:
            await poll_once()
        except Exception as e:
            print(f"[poller] unexpected: {e}")
        await asyncio.sleep(POLL_SEC)


@app.on_event("startup")
async def _startup():
    asyncio.create_task(poller())


# ── enrollment ───────────────────────────────────────────────────────────
@app.post("/enroll")
async def enroll(req: Request):
    if not ENROLL_TOKEN:
        raise HTTPException(503, "enrollment disabled (no ENROLL_TOKEN set)")
    try:
        body = await req.json()
    except Exception:
        raise HTTPException(400, "JSON body required")
    if str(body.get("token", "")) != ENROLL_TOKEN:
        raise HTTPException(403, "bad token")
    uuid = str(body.get("uuid", "")).strip()
    if not uuid or len(uuid) < 6:
        raise HTTPException(400, "uuid required")

    assignments = _load(ASSIGN_F, {})
    registry = _load(REGISTRY_F, {})

    if uuid in assignments:                      # idempotent re-enroll
        node_name = assignments[uuid]
    else:
        taken = set(assignments.values())
        candidates = sorted(p.name for p in POOL.iterdir()
                            if p.is_dir() and (p / "config.yml").exists())
        free = [c for c in candidates if c not in taken]
        if not free:
            raise HTTPException(409, "identity pool exhausted — generate "
                                     "more with provision_pool.sh and "
                                     "re-export")
        node_name = free[0]
        assignments[uuid] = node_name
        _save(ASSIGN_F, assignments)

    cfg = (POOL / node_name / "config.yml").read_text()
    node_num = int(node_name.rsplit("-", 1)[1])
    nebula_ip = f"10.42.0.{node_num}"
    reg = registry.get(node_name, {})
    reg.update({"nebula_ip": nebula_ip, "uuid": uuid,
                "enrolled_at": datetime.now(timezone.utc).isoformat()})
    registry[node_name] = reg
    _save(REGISTRY_F, registry)
    print(f"[enroll] {uuid} → {node_name} ({nebula_ip})")
    return {"node_name": node_name, "nebula_ip": nebula_ip, "config": cfg}


# ── APIs ─────────────────────────────────────────────────────────────────
@app.get("/api/fleet")
async def api_fleet():
    async with fleet_lock:
        nodes = [{k: v for k, v in rec.items() if k != "_ts"}
                 for rec in fleet.values()]
    return {"poll": last_poll, "outage_after_min": OUTAGE_MIN,
            "refresh_sec": REFRESH_SEC, "nodes": nodes}


@app.get("/api/registry")
async def api_registry_get():
    return _load(REGISTRY_F, {})


@app.post("/api/registry")
async def api_registry_set(req: Request):
    body = await req.json()
    registry = _load(REGISTRY_F, {})
    for name, meta in body.items():
        registry.setdefault(name, {}).update(meta)
    _save(REGISTRY_F, registry)
    return {"ok": True, "nodes": len(registry)}


@app.get("/health")
async def health():
    return {"ok": True, "query_node": QUERY_CONN, "last_poll": last_poll}


# ── UI ───────────────────────────────────────────────────────────────────
PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EADS Fleet</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0d1117;
color:#e6edf3;margin:0}header{padding:14px 18px;display:flex;
align-items:baseline;gap:14px}h1{font-size:18px;margin:0}
#sub{color:#8b949e;font-size:12px}#map{height:46vh;margin:0 14px;
border-radius:10px;border:1px solid #30363d}
table{width:calc(100% - 28px);margin:14px;border-collapse:collapse;font-size:13px}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #21262d}
th{color:#8b949e;font-size:11px;text-transform:uppercase}
.ONLINE{color:#3fb950;font-weight:600}.STALE{color:#d29922;font-weight:600}
.OUTAGE{color:#f85149;font-weight:600}.PIPELINE_STALLED{color:#ff8c42;font-weight:600}
.UNKNOWN{color:#8b949e}.warn{background:#3d1d20;color:#f85149;
padding:6px 14px;margin:0 14px;border-radius:8px;font-size:12px;display:none}
</style></head><body>
<header><h1>EADS Fleet</h1>
<span id="sub">updated <span id="ts">–</span> · auto-refresh __REFRESH__s</span>
</header>
<div class="warn" id="warn"></div>
<div id="map"></div>
<table><thead><tr><th>Node</th><th>Status</th><th>Mode</th><th>Last seen</th>
<th>Samples/s</th><th>Backlog rows</th><th>Disk %</th><th>Uptime</th>
<th>Location</th></tr></thead><tbody id="tb"></tbody></table>
<script>
const map = L.map('map').setView([36.97,-122.03],10);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',
  {attribution:'© OpenStreetMap'}).addTo(map);
let markers = [];
const COLOR = {ONLINE:'#3fb950',STALE:'#d29922',OUTAGE:'#f85149',PIPELINE_STALLED:'#ff8c42'};
function fmtAge(m){return m==null?'never':(m<1?'<1 min ago':m.toFixed(0)+' min ago')}
function fmtUp(s){s=+s;return isNaN(s)?'—':(s/3600).toFixed(1)+' h'}
async function tick(){
  const r = await fetch('/api/fleet'); const d = await r.json();
  document.getElementById('ts').textContent = new Date().toLocaleTimeString();
  const w = document.getElementById('warn');
  if(!d.poll.ok){w.style.display='block';
    w.textContent='Query node unreachable: '+d.poll.error+
      ' — statuses may be stale';}
  else w.style.display='none';
  markers.forEach(m=>map.removeLayer(m)); markers=[];
  let rows='';
  d.nodes.sort((a,b)=>a.node_id.localeCompare(b.node_id));
  for(const n of d.nodes){
    const st=(n.status||'UNKNOWN').split(' ')[0];
    rows+=`<tr><td>${n.node_id}</td><td class="${st}">${n.status}</td>
      <td>${n.mode||'—'}</td><td>${fmtAge(n.age_min)}</td>
      <td>${n.samples_sec_actual||'—'}</td>
      <td>${n.pending_upload_rows||'0'}</td>
      <td>${n.disk_pct||'—'}</td><td>${fmtUp(n.uptime_sec)}</td>
      <td>${(n.meta&&n.meta.label)||''}</td></tr>`;
    if(n.meta&&n.meta.lat!=null&&n.meta.lon!=null){
      const mk=L.circleMarker([n.meta.lat,n.meta.lon],{radius:10,
        color:COLOR[st]||'#8b949e',fillColor:COLOR[st]||'#8b949e',
        fillOpacity:.85}).addTo(map)
        .bindPopup(`<b>${n.node_id}</b><br>${n.status}<br>`+
                   `${(n.meta.label||'')}<br>${fmtAge(n.age_min)}`);
      markers.push(mk);
    }
  }
  document.getElementById('tb').innerHTML = rows;
  if(markers.length){map.fitBounds(L.featureGroup(markers).getBounds()
    .pad(0.3));}
}
tick(); setInterval(tick, __REFRESH__*1000);
</script></body></html>"""


@app.get("/", response_class=HTMLResponse)
async def index():
    return PAGE.replace("__REFRESH__", str(REFRESH_SEC))


if __name__ == "__main__":
    port = int(os.environ.get("FLEET_PORT", "8090"))
    tls_port = int(os.environ.get("ENROLL_PORT", "8443"))
    cert = os.environ.get("TLS_CERT", "/tls/enroll.crt")
    key = os.environ.get("TLS_KEY", "/tls/enroll.key")

    async def serve():
        cfgs = [uvicorn.Config(app, host="0.0.0.0", port=port,
                               log_level="info")]
        if os.path.exists(cert) and os.path.exists(key):
            cfgs.append(uvicorn.Config(app, host="0.0.0.0", port=tls_port,
                                       ssl_certfile=cert, ssl_keyfile=key,
                                       log_level="info"))
        else:
            print(f"[fleet] WARNING: TLS cert/key not found at {cert} — "
                  "enrollment port disabled")
        await asyncio.gather(*(uvicorn.Server(c).serve() for c in cfgs))

    asyncio.run(serve())
