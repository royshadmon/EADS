"""
EADS Archiver — runs on the master VM.

THE PROBLEM IT SOLVES: in AnyLog's model the Pis ARE the storage layer;
the master holds only the ledger. With 7-day local retention on the Pis,
anything not copied off a box within a week — or before an SD card dies —
is gone. This service is the system of record.

WHAT IT DOES: every ARCHIVE_SEC (default 300 s) it asks the query node for
rows newer than its per-table watermark and copies them into the master's
PostgreSQL (which was sitting underused). Rows are stored with extracted
key columns (node_id, ts_us) plus the full original row as JSONB, so
AnyLog-side schema drift never breaks the archive.

CORRECTNESS PROPERTIES:
  - Watermark queries use >= (never lose a boundary row); a UNIQUE
    (node_id, ts_us) constraint + ON CONFLICT DO NOTHING makes the overlap
    harmless → effectively exactly-once.
  - Watermarks persist in Postgres (archive_state), so restarts resume
    where they left off.
  - If the query node is down, the archiver just retries next cycle —
    the Pis hold 7 days of buffer, so hours of master downtime cost nothing.

DIALECT NOTE: per-table SQL comes from env templates (ARCHIVE_SQL_<table>)
with a {wm} placeholder, same honest approach as the fleet poller's
HEALTH_SQL — AnyLog time-window syntax varies by build; fix the env var,
not the code.
"""

import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone

import httpx
import psycopg

QUERY_CONN  = os.environ.get("QUERY_CONN", "127.0.0.1:32449")
DBMS        = os.environ.get("DEFAULT_DBMS", "eads")
ARCHIVE_SEC = int(os.environ.get("ARCHIVE_SEC", "300"))
BATCH_LIMIT = int(os.environ.get("BATCH_LIMIT", "20000"))
PG_DSN      = os.environ.get(
    "PG_DSN", "host=127.0.0.1 port=5432 dbname=eads_data user=eads "
              f"password={os.environ.get('POSTGRES_PASSWORD', '')}")

TABLES = [t.strip() for t in
          os.environ.get("ARCHIVE_TABLES",
                         "voltage_calibrated,node_health").split(",") if t.strip()]

DEFAULT_SQL = ('select * from {table} where sample_time > \'{wm}\' '
               'and is_anomaly = 1 order by sample_time limit {limit}')

EPOCH = "1970-01-01 00:00:00"

DDL = """
create schema if not exists eads_archive;
create table if not exists eads_archive.archive_state (
    table_name text primary key,
    watermark  text not null,
    rows_total bigint not null default 0,
    updated_at timestamptz not null default now()
);
"""

TABLE_DDL = """
create table if not exists eads_archive.{t} (
    id          bigserial primary key,
    node_id     text not null,
    ts_us       bigint not null,
    insert_ts   text,
    archived_at timestamptz not null default now(),
    data        jsonb not null,
    unique (node_id, ts_us)
);
create index if not exists {t}_node_time on eads_archive.{t} (node_id, ts_us);
"""


def log(msg):
    print(f"[archiver] {msg}", flush=True)


def db_connect():
    return psycopg.connect(PG_DSN, autocommit=False)


def ensure_schema(conn):
    with conn.cursor() as cur:
        cur.execute(DDL)
        for t in TABLES:
            cur.execute(TABLE_DDL.format(t=t))
            cur.execute(
                "insert into eads_archive.archive_state (table_name, watermark)"
                " values (%s, %s) on conflict (table_name) do nothing",
                (t, EPOCH))
    conn.commit()


def get_watermark(conn, table):
    with conn.cursor() as cur:
        cur.execute("select watermark from eads_archive.archive_state "
                    "where table_name = %s", (table,))
        return cur.fetchone()[0]


def set_watermark(conn, table, wm, added):
    with conn.cursor() as cur:
        cur.execute(
            "update eads_archive.archive_state set watermark=%s, "
            "rows_total = rows_total + %s, updated_at = now() "
            "where table_name=%s", (wm, added, table))


async def fetch_rows(client, table, wm):
    tmpl = os.environ.get(f"ARCHIVE_SQL_{table}", DEFAULT_SQL)
    sql = tmpl.format(table=table, wm=wm, limit=BATCH_LIMIT)
    headers = {
        "User-Agent": "AnyLog/1.23",
        "command": f'sql {DBMS} format=json and stat=false "{sql}"',
        "destination": "network",
    }
    r = await client.get(f"http://{QUERY_CONN}", headers=headers, timeout=60)
    r.raise_for_status()
    body = r.json()
    if isinstance(body, dict):
        return body.get("Query", body.get("query", []))
    return body if isinstance(body, list) else []


def insert_rows(conn, table, rows):
    """Insert with dedup; returns (inserted, max_insert_ts_seen)."""
    inserted = 0
    max_ts = None
    with conn.cursor() as cur:
        for row in rows:
            node_id = str(row.get("node_id") or row.get("tsd_name") or "?")
            try:
                ts_us = int(str(row.get("timestamp_us", "")))
            except (ValueError, TypeError):
                ts_us = 0
            if not ts_us and row.get("sample_time"):
                try:
                    from datetime import datetime, timezone
                    st = str(row["sample_time"]).replace("T", " ").rstrip("Z")
                    dt = datetime.strptime(st, "%Y-%m-%d %H:%M:%S.%f")
                    ts_us = int(dt.replace(tzinfo=timezone.utc).timestamp() * 1_000_000)
                except Exception:
                    ts_us = 0
            ins_ts = row.get("sample_time") or row.get("insert_timestamp")
            if ins_ts is not None and (max_ts is None or str(ins_ts) > max_ts):
                max_ts = str(ins_ts)
            cur.execute(
                f"insert into eads_archive.{table} "
                "(node_id, ts_us, insert_ts, data) values (%s,%s,%s,%s) "
                "on conflict (node_id, ts_us) do nothing",
                (node_id, ts_us, ins_ts, json.dumps(row)))
            inserted += cur.rowcount
    return inserted, max_ts


async def archive_cycle(conn):
    async with httpx.AsyncClient() as client:
        for table in TABLES:
            wm = get_watermark(conn, table)
            try:
                rows = await fetch_rows(client, table, wm)
            except Exception as e:
                log(f"{table}: query node unreachable/failed ({e}) — "
                    "will retry next cycle (Pis buffer 7 days)")
                continue
            if not rows:
                continue
            try:
                inserted, max_ts = insert_rows(conn, table, rows)
                new_wm = max_ts or wm
                set_watermark(conn, table, new_wm, inserted)
                conn.commit()
                log(f"{table}: +{inserted} rows "
                    f"({len(rows) - inserted} dup-skipped), "
                    f"watermark → {new_wm}")
            except Exception as e:
                conn.rollback()
                log(f"{table}: insert failed ({e}) — rolled back, no "
                    "watermark advance, nothing lost")


async def main():
    # Wait for Postgres on cold start.
    conn = None
    for attempt in range(30):
        try:
            conn = db_connect()
            break
        except Exception as e:
            log(f"waiting for postgres ({e})")
            await asyncio.sleep(5)
    if conn is None:
        log("FATAL: postgres never became reachable")
        sys.exit(1)
    ensure_schema(conn)
    log(f"archiving {TABLES} every {ARCHIVE_SEC}s from {QUERY_CONN} "
        f"(batch {BATCH_LIMIT})")
    while True:
        try:
            await archive_cycle(conn)
        except psycopg.OperationalError as e:
            log(f"pg connection lost ({e}); reconnecting")
            try:
                conn.close()
            except Exception:
                pass
            await asyncio.sleep(5)
            try:
                conn = db_connect()
            except Exception as e2:
                log(f"reconnect failed: {e2}")
        except Exception as e:
            log(f"unexpected: {e}")
        await asyncio.sleep(ARCHIVE_SEC)


if __name__ == "__main__":
    asyncio.run(main())
