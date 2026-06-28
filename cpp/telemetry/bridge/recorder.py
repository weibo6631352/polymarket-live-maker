#!/usr/bin/env python3
"""recorder.py — telemetry stdin -> SQLite, one table per topic ("便于分析" store).

Reads JSON lines ({"topic": "...", "data": {...}}) from stdin and INSERTs each into a
table named after the topic (lower-cased). The table is created on first sight; columns
are the data keys (REAL for numbers/bools, TEXT otherwise). New keys seen later trigger
ALTER TABLE ADD COLUMN. Sequences/objects (e.g. OrderBookL2 bids/asks) are stored as JSON
text. Commits are batched (by row count or elapsed time). One file: state/telemetry.db.

Deps: stdlib only (sqlite3, json). Usage: recorder.py [db_path]   (default state/telemetry.db)
"""
import json
import os
import re
import sqlite3
import sys
import time

DB_PATH = sys.argv[1] if len(sys.argv) > 1 else "state/telemetry.db"
COMMIT_ROWS = 200       # commit after this many pending inserts
COMMIT_SECS = 1.0       # ...or after this many seconds since last commit

_IDENT = re.compile(r"[^A-Za-z0-9_]")


def ident(name: str) -> str:
    """Sanitize a table/column identifier (IDL field names are already safe)."""
    s = _IDENT.sub("_", str(name))
    if not s or s[0].isdigit():
        s = "_" + s
    return s


def col_type(v) -> str:
    # Coordinator spec: all TEXT/REAL. bool is a subclass of int -> REAL (0/1).
    return "REAL" if isinstance(v, (int, float, bool)) else "TEXT"


def bind(v):
    if isinstance(v, bool):
        return 1 if v else 0
    if isinstance(v, (int, float)) or v is None:
        return v
    if isinstance(v, (list, dict)):
        return json.dumps(v, separators=(",", ":"))
    return str(v)


class Recorder:
    def __init__(self, conn: sqlite3.Connection):
        self.conn = conn
        self.cols: dict[str, set[str]] = {}  # table -> known columns
        self._load_existing_schema()

    def _load_existing_schema(self):
        cur = self.conn.cursor()
        for (tbl,) in cur.execute(
                "SELECT name FROM sqlite_master WHERE type='table'").fetchall():
            info = cur.execute(f'PRAGMA table_info("{tbl}")').fetchall()
            self.cols[tbl] = {row[1] for row in info}

    def _ensure_table(self, tbl: str, data: dict):
        keys = [ident(k) for k in data.keys()]
        if tbl not in self.cols:
            coldefs = ", ".join(f'"{k}" {col_type(data[orig])}'
                                for k, orig in zip(keys, data.keys()))
            if not coldefs:
                coldefs = '"_empty" TEXT'
            self.conn.execute(f'CREATE TABLE IF NOT EXISTS "{tbl}" ({coldefs})')
            self.cols[tbl] = set(keys)
        else:
            known = self.cols[tbl]
            for k, orig in zip(keys, data.keys()):
                if k not in known:
                    self.conn.execute(
                        f'ALTER TABLE "{tbl}" ADD COLUMN "{k}" {col_type(data[orig])}')
                    known.add(k)

    def insert(self, topic: str, data: dict):
        tbl = ident(topic).lower()
        self._ensure_table(tbl, data)
        keys = [ident(k) for k in data.keys()]
        if not keys:
            return
        cols = ", ".join(f'"{k}"' for k in keys)
        ph = ", ".join("?" for _ in keys)
        vals = [bind(data[k]) for k in data.keys()]
        self.conn.execute(f'INSERT INTO "{tbl}" ({cols}) VALUES ({ph})', vals)


def main():
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    rec = Recorder(conn)

    pending = 0
    last_commit = time.monotonic()
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
            topic = msg["topic"]
            data = msg.get("data") or {}
            if not isinstance(data, dict):
                continue
        except (ValueError, KeyError, TypeError):
            continue
        try:
            rec.insert(topic, data)
            pending += 1
        except sqlite3.Error as e:
            print(f"recorder: insert error: {e}", file=sys.stderr, flush=True)
            continue
        now = time.monotonic()
        if pending >= COMMIT_ROWS or (pending and now - last_commit >= COMMIT_SECS):
            conn.commit()
            pending = 0
            last_commit = now

    conn.commit()
    conn.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
