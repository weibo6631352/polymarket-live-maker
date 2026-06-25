"""Tests for the append-only review/iteration event log (EventLog)."""

from __future__ import annotations

import json
import threading
from datetime import datetime, timezone

from pm_trader.events import EventLog


def _today() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d")


def _lines(path):
    return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]


class TestWrite:
    def test_write_creates_dated_file_with_record(self, tmp_path):
        log = EventLog(tmp_path, retention_days=10)
        log.write("poll", cond="0xa", reward=0.5, share=0.2)
        log.close()
        f = tmp_path / f"events-{_today()}.jsonl"
        assert f.exists()
        rows = _lines(f)
        assert len(rows) == 1
        r = rows[0]
        assert r["kind"] == "poll" and r["cond"] == "0xa" and r["reward"] == 0.5
        assert "ts" in r and r["ts"].endswith("+00:00")

    def test_appends_multiple(self, tmp_path):
        log = EventLog(tmp_path)
        for i in range(5):
            log.write("poll", i=i)
        log.close()
        rows = _lines(tmp_path / f"events-{_today()}.jsonl")
        assert [r["i"] for r in rows] == [0, 1, 2, 3, 4]

    def test_non_serializable_does_not_raise(self, tmp_path):
        log = EventLog(tmp_path)
        log.write("x", obj=object())   # default=str handles it; must not raise
        log.close()
        assert len(_lines(tmp_path / f"events-{_today()}.jsonl")) == 1

    def test_write_after_close_reopens(self, tmp_path):
        log = EventLog(tmp_path)
        log.write("a", n=1)
        log.close()
        log.write("b", n=2)            # close() resets; next write reopens
        log.close()
        rows = _lines(tmp_path / f"events-{_today()}.jsonl")
        assert [r["kind"] for r in rows] == ["a", "b"]


class TestPrune:
    def test_prune_drops_old_keeps_recent(self, tmp_path):
        old = tmp_path / "events-20000101.jsonl"      # year 2000 -> well past window
        old.write_text('{"kind":"poll"}\n')
        recent = tmp_path / f"events-{_today()}.jsonl"
        recent.write_text('{"kind":"poll"}\n')
        EventLog(tmp_path, retention_days=10).prune()
        assert not old.exists()
        assert recent.exists()

    def test_prune_ignores_unrelated_files(self, tmp_path):
        keep = tmp_path / "notes.txt"
        keep.write_text("hi")
        EventLog(tmp_path).prune()
        assert keep.exists()

    def test_constructor_prunes(self, tmp_path):
        (tmp_path / "events-20000101.jsonl").write_text("{}\n")
        EventLog(tmp_path, retention_days=10)         # __init__ calls prune()
        assert not (tmp_path / "events-20000101.jsonl").exists()


class TestThreadSafety:
    def test_concurrent_writes_no_corruption(self, tmp_path):
        log = EventLog(tmp_path)
        n_threads, per = 8, 100

        def worker(tid):
            for i in range(per):
                log.write("poll", tid=tid, i=i)

        threads = [threading.Thread(target=worker, args=(t,)) for t in range(n_threads)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        log.close()
        rows = _lines(tmp_path / f"events-{_today()}.jsonl")   # every line valid JSON
        assert len(rows) == n_threads * per
