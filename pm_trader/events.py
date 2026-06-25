"""Append-only event log for post-hoc review & iteration.

Captures the granular series the SQLite ledger doesn't keep — per-poll metrics
(share/mid/reward/inventory), discovery snapshots, and place/exit decisions — as
one JSON object per line in date-stamped files (``events-YYYYMMDD.jsonl``) under a
directory, with a rolling N-day retention (old day-files are simply deleted).

Thread-safe: the background discovery thread and the main poll loop both write,
so every append + file rollover is guarded by a lock. Best-effort by design —
``write`` never raises into the hot loop; losing a log line must never break
trading.
"""

from __future__ import annotations

import json
import threading
from datetime import datetime, timedelta, timezone
from pathlib import Path


class EventLog:
    def __init__(self, directory, retention_days: int = 10) -> None:
        self.dir = Path(directory)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.retention_days = max(1, int(retention_days))
        self._lock = threading.Lock()
        self._fh = None
        self._cur_day: str | None = None
        self.prune()

    def _path_for(self, day: str) -> Path:
        return self.dir / f"events-{day}.jsonl"

    def write(self, kind: str, **fields) -> None:
        """Append one event ``{ts, kind, **fields}``. Best-effort; never raises."""
        try:
            now = datetime.now(timezone.utc)
            day = now.strftime("%Y%m%d")
            line = json.dumps({"ts": now.isoformat(), "kind": kind, **fields},
                              default=str)
            with self._lock:
                if day != self._cur_day:                # first write / date rollover
                    if self._fh is not None:
                        self._fh.close()
                    self._fh = open(self._path_for(day), "a")
                    self._cur_day = day
                    self.prune()                        # roll off old days on rollover
                self._fh.write(line + "\n")
                self._fh.flush()
        except Exception:  # noqa: BLE001 — a lost log line must never break the loop
            pass

    def prune(self) -> None:
        """Delete event files older than the retention window (YYYYMMDD sorts
        chronologically, so a lexical compare against the cutoff is correct)."""
        try:
            cutoff = (datetime.now(timezone.utc)
                      - timedelta(days=self.retention_days)).strftime("%Y%m%d")
            for p in self.dir.glob("events-*.jsonl"):
                day = p.stem.replace("events-", "")
                if len(day) == 8 and day.isdigit() and day < cutoff:
                    p.unlink()
        except Exception:  # noqa: BLE001
            pass

    def close(self) -> None:
        with self._lock:
            if self._fh is not None:
                self._fh.close()
                self._fh = None
            self._cur_day = None
