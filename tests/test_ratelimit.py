"""Tests for the token-bucket rate limiter."""

from __future__ import annotations

import threading
import time

from pm_trader.ratelimit import TokenBucket


class TestTryAcquire:
    def test_burst_capacity_then_empty(self):
        b = TokenBucket(rate=1000, burst=3)
        assert b.try_acquire() and b.try_acquire() and b.try_acquire()
        assert not b.try_acquire()          # burst exhausted

    def test_default_burst_is_rate(self):
        assert TokenBucket(rate=5).capacity == 5.0

    def test_refills_over_time(self):
        b = TokenBucket(rate=100, burst=1)
        assert b.try_acquire()
        assert not b.try_acquire()
        time.sleep(0.05)                    # ~5 tokens refilled at 100/s
        assert b.try_acquire()


class TestAcquireBlocking:
    def test_blocks_until_available_then_succeeds(self):
        b = TokenBucket(rate=50, burst=1)
        b.try_acquire()                     # drain
        t0 = time.monotonic()
        assert b.acquire(1, timeout=1.0)    # must wait ~1/50=20ms for a refill
        assert time.monotonic() - t0 >= 0.01

    def test_timeout_returns_false(self):
        b = TokenBucket(rate=1, burst=1)
        b.try_acquire()                     # drain; next token ~1s away
        assert b.acquire(1, timeout=0.05) is False


class TestRateCap:
    def test_concurrent_threads_capped_to_rate(self):
        # 8 threads hammering acquire() must not exceed ~rate over the window
        rate = 200.0
        b = TokenBucket(rate=rate, burst=10)
        got = [0]
        lock = threading.Lock()
        stop = time.monotonic() + 0.5

        def worker():
            n = 0
            while time.monotonic() < stop:
                if b.acquire(1, timeout=0.1):
                    n += 1
            with lock:
                got[0] += n

        threads = [threading.Thread(target=worker) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        # over ~0.5s at 200/s we expect ~100 (+ burst 10). Allow generous slack but
        # it MUST be far below the unthrottled count (8 threads would do thousands).
        assert got[0] <= 100 + 10 + 40, f"rate cap leaked: {got[0]}"
        assert got[0] >= 60, f"throttled too hard: {got[0]}"
