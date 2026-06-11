#!/usr/bin/env python3
"""
fix_duplicate_policies.py — EADS master, duplicate-policy guard.

THE BUG: on every restart, the AnyLog master's deploy script re-declares
its master/cluster policies unconditionally. Against a persistent ledger
volume, each restart adds another copy — the long-standing
duplicate-policy-on-restart problem (previous workaround: wipe volumes).

THE FIX SHAPE: the true fix is a conditional declaration inside the
image's deployment-scripts (declare only if `blockchain get master where
name=... and company=...` comes back empty). That script lives inside the
anylogco image. Until it's patched there, THIS guard makes restarts safe:
run it after every stack start; it finds policy types with duplicates
(same name+company), keeps the OLDEST (the one operators already
reference), and drops the rest from the ledger. Idempotent — a clean
ledger is a no-op.

Usage:
    python3 fix_duplicate_policies.py            # fix
    python3 fix_duplicate_policies.py --dry-run  # report only

Env (all optional):
    MASTER_REST      default 127.0.0.1:32049
    POLICY_TYPES     default master,cluster,operator
    DROP_CMD         default 'blockchain drop policy where id = {id}'
                     (override if your AnyLog build's dialect differs)

Wired into master_setup.sh after the stack comes up; also safe from cron.
Stdlib only — no pip deps on the VM.
"""

import json
import os
import sys
import urllib.request

MASTER_REST  = os.environ.get("MASTER_REST", "127.0.0.1:32049")
POLICY_TYPES = [t.strip() for t in
                os.environ.get("POLICY_TYPES", "master,cluster,operator")
                .split(",") if t.strip()]
DROP_CMD     = os.environ.get("DROP_CMD",
                              "blockchain drop policy where id = {id}")
DRY = "--dry-run" in sys.argv


def anylog(command: str, method: str = "GET"):
    req = urllib.request.Request(
        f"http://{MASTER_REST}", method=method,
        headers={"command": command, "User-Agent": "AnyLog/1.23"})
    with urllib.request.urlopen(req, timeout=15) as r:
        body = r.read().decode(errors="replace").strip()
    if not body:
        return []
    try:
        return json.loads(body)
    except json.JSONDecodeError:
        # AnyLog sometimes replies with plain text for empty/info results
        return body


def policies_of(ptype: str):
    """Return list of (id, name, company, date) for one policy type."""
    out = []
    res = anylog(f"blockchain get {ptype}")
    if not isinstance(res, list):
        return out
    for item in res:
        body = item.get(ptype) if isinstance(item, dict) else None
        if not isinstance(body, dict):
            continue
        out.append((body.get("id", ""), body.get("name", "?"),
                    body.get("company", "?"), body.get("date", "")))
    return out


def main():
    total_dropped = 0
    for ptype in POLICY_TYPES:
        try:
            pols = policies_of(ptype)
        except Exception as e:
            print(f"[dedup] {ptype}: master REST unreachable ({e}) — "
                  "is the stack up?")
            sys.exit(1)
        groups: dict[tuple, list] = {}
        for pid, name, company, date in pols:
            groups.setdefault((name, company), []).append((date, pid))
        for (name, company), members in groups.items():
            if len(members) <= 1:
                continue
            members.sort()                 # oldest date first
            keep = members[0]
            drops = members[1:]
            print(f"[dedup] {ptype} '{name}'/{company}: {len(members)} "
                  f"copies — keeping {keep[1][:12]}… ({keep[0]}), "
                  f"dropping {len(drops)}")
            for date, pid in drops:
                if not pid:
                    print(f"[dedup]   SKIP copy dated {date}: no id field "
                          "— drop manually via the AnyLog CLI")
                    continue
                if DRY:
                    print(f"[dedup]   would drop {pid}")
                    continue
                cmd = DROP_CMD.format(id=pid)
                try:
                    anylog(cmd, method="POST")
                    print(f"[dedup]   dropped {pid}")
                    total_dropped += 1
                except Exception as e:
                    print(f"[dedup]   drop FAILED for {pid}: {e} — if this "
                          "persists, check DROP_CMD dialect against your "
                          "AnyLog build")
    if total_dropped == 0 and not DRY:
        print("[dedup] ledger clean — nothing to do")
    else:
        print(f"[dedup] done ({'dry-run' if DRY else f'{total_dropped} dropped'})")


if __name__ == "__main__":
    main()
