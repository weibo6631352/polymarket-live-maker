"""Thread-safe token-bucket rate limiter.

The CLOB enforces a global per-key request cap (the order-book poll limit is ~149
req/s). The maker loop's reads (book/midpoint) and writes (cancel/place) all count
against that one budget, so a single shared bucket paces every request and keeps us
under the cap — fast polling without tripping Cloudflare 429s. ``acquire`` blocks
the calling thread until a token frees, so N concurrent pollers self-throttle to
the global rate.
"""

from __future__ import annotations

import threading
import time


class TokenBucket:
    """Refills at ``rate`` tokens/sec up to ``burst`` capacity. Thread-safe."""

    def __init__(self, rate: float, burst: float | None = None) -> None:
        self.rate = float(rate)
        self.capacity = float(burst) if burst is not None else max(1.0, float(rate))
        self._tokens = self.capacity
        self._last = time.monotonic()
        self._lock = threading.Lock()

    def _refill_locked(self) -> None:
        now = time.monotonic()
        elapsed = now - self._last
        if elapsed > 0:
            self._tokens = min(self.capacity, self._tokens + elapsed * self.rate)
            self._last = now

    def try_acquire(self, n: float = 1.0) -> bool:
        """Take ``n`` tokens if available right now; never blocks."""
        with self._lock:
            self._refill_locked()
            if self._tokens >= n:
                self._tokens -= n
                return True
            return False

    def acquire(self, n: float = 1.0, timeout: float | None = None) -> bool:
        """Block until ``n`` tokens are taken, or ``timeout`` elapses (then False).

        Cooperative: sleeps in small slices so many threads share the bucket fairly
        enough for rate-capping (this is a safety throttle, not a fairness scheduler).
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            with self._lock:
                self._refill_locked()
                if self._tokens >= n:
                    self._tokens -= n
                    return True
                deficit = n - self._tokens
                wait = deficit / self.rate if self.rate > 0 else 0.05
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                wait = min(wait, remaining)
            time.sleep(min(max(wait, 0.0), 0.05))   # cap the polling granularity

    @property
    def available(self) -> float:
        with self._lock:
            self._refill_locked()
            return self._tokens
