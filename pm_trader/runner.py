"""Autonomous live-maker runner — drives the ENGINE maker loop on a fixed cadence.

The no-MCP driver: a single SYNC process that, on the same beat as before, runs
the engine's maker poll (``accrue_maker_rewards``) — inheriting ALL its historical
optimizations: per-loop reconcile-exit (pool left the program), drift-exit (a
full-band move from entry), reward accrual + capital ledger, inventory cap + skew.

  rediscover (periodic scan) -> select_pools -> engine.place_maker_quote(_live)
    -> each poll: engine.accrue_maker_rewards(_live) -> handle exits (cooldown)
       -> kill-switch

In LIVE (``PM_TRADER_LIVE=1``) it uses the engine's ``*_live`` methods: REAL
orders via a py-clob-client submitter, inventory from REAL polled fills (not the
maker_fill sim), plus fast-cancel re-centering. In DRY-RUN it uses the paper
methods (simulated, no orders sent). No WebSocket, no LLM in the loop.
"""

from __future__ import annotations

import logging
import os
import signal
import threading
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from pm_trader.engine import Engine
from pm_trader.events import EventLog
from pm_trader.orderbook import depth_ahead
from pm_trader.maker_live import DryRunSubmitter, build_clob_signer
from pm_trader.models import NotInitializedError
from pm_trader.portfolio import select_pools
from pm_trader.ratelimit import TokenBucket
from pm_trader.rewards import RewardsClient, reward_share, scan, score_pool
from pm_trader.ws import MarketChannel, UserChannel

log = logging.getLogger("pm_trader.runner")

# Concurrency cap for the held-pool re-score fetch in reevaluate_held(). Each
# held pool's re-score is 3 independent read-only CLOB GETs; fetching the held
# book concurrently keeps the reeval beat from stalling the loop as the book grows.
REEVAL_WORKERS = 16


class _LockingSubmitter:
    """Serialize ALL submitter access across the poll thread and the WS reflex
    thread (both cancel/place orders). The inner submitter does the real work; this
    only adds one lock so the two threads never call py-clob-client concurrently."""

    def __init__(self, inner) -> None:
        self._inner = inner
        self._lock = threading.Lock()

    def __call__(self, action):
        with self._lock:
            return self._inner(action)

    def __getattr__(self, name):           # forward poll_fills/api_creds/cancel_order/…
        attr = getattr(self._inner, name)
        if callable(attr):
            def locked(*a, **k):
                with self._lock:
                    return attr(*a, **k)
            return locked
        return attr


def load_dotenv(path: str | os.PathLike = ".env") -> None:
    """Minimal ``.env`` reader: ``KEY=VALUE`` -> os.environ (no override). No dep."""
    p = Path(path)
    if not p.exists():
        return
    for raw in p.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def _f(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _i(name: str, default: int) -> int:
    try:
        return int(float(os.environ.get(name, default)))
    except (TypeError, ValueError):
        return default


@dataclass
class RunnerConfig:
    live: bool = False
    # When not live: dry_live=True rehearses the LIVE code path (accrue_maker_rewards_live
    # + a DryRunSubmitter that logs orders and sends nothing). dry_live=False uses the
    # PAPER simulator (accrue_maker_rewards, maker_fill). live=True always wins.
    dry_live: bool = True
    capital: float = 200.0
    max_pools: int = 3
    min_daily: float = 80.0
    scan_top: int = 60
    half_spread_ticks: int = 1
    # use the volatility-aware optimal half-spread (engine.suggest_maker_half_spread)
    # per pool at selection instead of the fixed 1-tick offset. Off by default.
    use_optimal_spread: bool = False
    recenter_ticks: int = 1            # re-quote only after the mid moves this many ticks
    crossing_cost_c: float = 0.0       # PAPER sim: taker cost (cents) charged on exit-flatten
    risk_tolerance_days: float = 7.0
    max_token_overlap: int = 1
    poll_seconds: float = 60.0          # same beat as the paper maker poll
    discovery_interval_s: float = 600.0   # full reward-universe re-scan cadence (10 min)
    cooldown_rounds: int = 3
    max_loss: float = 20.0              # kill-switch: maker inventory-PnL floor
    min_wallet_usdc: float = 0.0        # kill-switch: hard floor on REAL wallet USDC (0=off)
    # continuous re-evaluation of HELD pools (degradation exit + opportunity rotation)
    reeval_enabled: bool = True
    reeval_interval_s: float = 300.0    # re-check held pools this often (cheap; held-only)
    min_share: float = 0.02            # reeval: leave if our est share collapses below this
    # entry floor: skip a candidate whose estimated reward (share x daily) is below
    # this $/day — not worth the ~$50 capital lock. (Reward-based, NOT share-based:
    # a small share of a high-daily pool still earns.) 0 = off.
    min_pool_reward: float = 0.5
    # exit a held pool whose real-time mid velocity exceeds this (cents/sec) — a
    # choppy book bleeds via small pick-offs the daily jump_verdict misses. 0 = off.
    max_mid_vel_cps: float = 4.0
    min_hold_s: float = 600.0          # don't soft-exit a pool held less than this
    state_dir: str = "state"
    kill_file: str = "KILL"
    # rolling data retention (days) for the event log + equity curve + rotated logs
    retention_days: int = 30
    events_enabled: bool = True        # append-only per-poll/discovery event log
    # throttle the per-pool "poll" heartbeat event: at a 1s cadence the granular
    # series would bloat, so write at most one poll event per pool per this many
    # seconds. Decisions (place/exit/reflex/discovery) are ALWAYS logged.
    event_poll_every_s: float = 10.0
    # periodically log achieved request rate (read/write) + load average — a side
    # gauge of throughput AND CPU load. 0 = off.
    stats_every_s: float = 60.0
    # global CLOB request cap (req/s), shared by reads + writes + discovery via one
    # TokenBucket. 149 = the order-book rate limit (matches the sports-trader-cpp
    # daemon, i.e. 1 below 150). The bucket is the hard ceiling; tune via env.
    max_req_per_sec: float = 149.0
    # tokens reserved for HIGH-priority order ops (cancels/places). Reads (book
    # re-sync etc.) are low-priority and leave this headroom, so a cancel never
    # queues behind a flood of reads — it fires immediately.
    write_reserve: float = 20.0
    # real-time WS market data: book/mid pushed (~ms) instead of REST-polled, and
    # real fills via the user channel. Dataclass default False keeps unit tests
    # network-free; from_env turns it ON. Falls back to REST per-token if the WS
    # cache isn't fresh, so it degrades gracefully.
    ws_enabled: bool = False
    # REST /book re-sync workers: this many threads continuously round-robin
    # authoritative /book snapshots over the held tokens (low-priority reads), SOAKING
    # the leftover budget (149 − writes − reserve) to keep the WS book authoritative.
    # Bucket-paced, so they self-limit and never starve cancels. 0 = off.
    resync_workers: int = 8

    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "RunnerConfig":
        load_dotenv()
        return cls(
            live=os.environ.get("PM_TRADER_LIVE", "0").strip() == "1",
            dry_live=os.environ.get("LM_DRY_LIVE", "1").strip() != "0",
            capital=_f("LM_CAPITAL", 200.0),
            max_pools=_i("LM_MAX_POOLS", 3),
            min_daily=_f("LM_MIN_DAILY", 80.0),
            scan_top=_i("LM_SCAN_TOP", 60),
            poll_seconds=_f("LM_POLL_SECONDS", 60.0),
            discovery_interval_s=_f("LM_DISCOVERY_INTERVAL_S", 600.0),
            cooldown_rounds=_i("LM_COOLDOWN_ROUNDS", 3),
            max_loss=_f("LM_MAX_LOSS_PER_DAY", 20.0),
            retention_days=_i("LM_RETENTION_DAYS", 30),
            events_enabled=os.environ.get("LM_EVENTS", "1").strip() != "0",
            event_poll_every_s=_f("LM_EVENT_POLL_EVERY_S", 10.0),
            stats_every_s=_f("LM_STATS_EVERY_S", 60.0),
            max_req_per_sec=_f("LM_MAX_REQ_PER_SEC", 149.0),
            write_reserve=_f("LM_WRITE_RESERVE", 20.0),
            ws_enabled=os.environ.get("LM_WS", "1").strip() != "0",
            resync_workers=_i("LM_BOOK_RESYNC_WORKERS", 8),
            reeval_enabled=os.environ.get("LM_REEVAL", "1").strip() != "0",
            reeval_interval_s=_f("LM_REEVAL_INTERVAL_S", 300.0),
            min_share=_f("LM_MIN_SHARE", 0.02),
            min_pool_reward=_f("LM_MIN_POOL_REWARD", 0.5),
            max_mid_vel_cps=_f("LM_MAX_MID_VEL_CPS", 4.0),
            min_hold_s=_f("LM_MIN_HOLD_S", 600.0),
            min_wallet_usdc=_f("LM_MIN_WALLET_USDC", 0.0),
            use_optimal_spread=os.environ.get("LM_OPTIMAL_SPREAD", "0").strip() == "1",
            recenter_ticks=_i("LM_RECENTER_TICKS", 1),
            crossing_cost_c=_f("LM_CROSSING_COST_C", 0.0),
        )

    def banner(self) -> str:
        if self.live:
            mode = "LIVE — REAL MONEY"
        elif self.dry_live:
            mode = "DRY-LIVE (live code path, no orders/fills -> P&L = gross reward only)"
        else:
            mode = "PAPER (simulator)"
        return (f"live-maker | mode={mode} | capital=${self.capital:.0f} | "
                f"max_pools={self.max_pools} | poll={self.poll_seconds:.0f}s | "
                f"min_daily=${self.min_daily:.0f} | max_loss=${self.max_loss:.0f}")


class LiveRunner:
    """The autonomous polling loop, driving the engine. Inject deps in tests."""

    def __init__(self, config: RunnerConfig, *, engine: Engine | None = None,
                 scanner_client: RewardsClient | None = None, submitter=None,
                 sleeper=time.sleep) -> None:
        self.cfg = config
        self.engine = engine
        # ONE shared token bucket = the global CLOB req/s budget, shared (FIFO) by
        # every request source: the fast hot-poll reads, reeval, the background
        # discovery scan, and live order ops. Keeps total req/s under the cap so
        # the loop can poll sub-second without 429s.
        self.rate_limiter = (TokenBucket(config.max_req_per_sec,
                                         reserve=config.write_reserve)
                             if config.max_req_per_sec and config.max_req_per_sec > 0
                             else None)
        self.scanner = scanner_client or RewardsClient(rate_limiter=self.rate_limiter)
        if self.rate_limiter is not None and hasattr(self.scanner, "rate_limiter"):
            self.scanner.rate_limiter = self.rate_limiter   # injected scanners too
        self.submitter = submitter
        self._sleep_fn = sleeper
        self.report: dict = {"pools": []}
        self.selected: list[dict] = []
        self.placed: dict[str, dict] = {}    # condition_id -> selected pool dict
        self.placed_at: dict[str, float] = {}  # condition_id -> monotonic placement time
        self.cooldown: dict[str, int] = {}   # condition_id -> discovery rounds left
        self._last_scan_ok: float | None = None  # monotonic time of last good discovery
        self._stop = False
        self._kill_reason = ""
        # background discovery: the heavy reward-universe scan runs off the hot loop
        self._discovery_thread: threading.Thread | None = None
        self._discovery_stop = threading.Event()
        self._events: EventLog | None = None   # created in run(); None in unit tests
        self._market_ch: MarketChannel | None = None   # real-time book (WS)
        self._user_ch: UserChannel | None = None       # real-time fills (WS, live)
        self._resync_thread: threading.Thread | None = None  # REST /book re-sync
        self._resync_pool: ThreadPoolExecutor | None = None  # concurrent /book workers
        self._poll_evt_at: dict[str, float] = {}   # token -> last poll-event time (throttle)
        self._stats_at: float | None = None        # last stats log (monotonic)
        self._stats_granted: tuple[float, float] = (0.0, 0.0)  # (granted, granted_low)
        self._mid_hist: dict[str, deque] = {}      # token -> recent (ts, mid) for velocity
        self._ws_upd_at: dict[str, tuple[float, int]] = {}  # token -> (ts, update count)
        # WS reflex: cancel a held pool's orders the instant its mid moves beyond the
        # band (decoupled from accounting; the next poll reposts via force_recenter).
        self._reflex_refs: dict[str, tuple[float, float]] = {}  # token -> (mid, band)
        self._reflex_cancelled: set[str] = set()       # reflex-pulled, awaiting repost
        self._reflex_lock = threading.Lock()

    def _event(self, kind: str, **fields) -> None:
        """Append one review/iteration event (no-op until run() opens the log)."""
        if self._events is not None:
            self._events.write(kind, **fields)

    # -- lifecycle ----------------------------------------------------------

    def run(self) -> None:
        logging.getLogger("pm_trader").info(self.cfg.banner())
        os.makedirs(self.cfg.state_dir, exist_ok=True)
        if self.cfg.events_enabled and self._events is None:
            self._events = EventLog(Path(self.cfg.state_dir) / "events",
                                    retention_days=self.cfg.retention_days)
        self._install_signals()
        self._ensure_engine()
        self.engine.maker_crossing_cost_c = self.cfg.crossing_cost_c  # PAPER-sim realism
        if self.rate_limiter is not None and getattr(self.engine, "api", None) is not None:
            self.engine.api.rate_limiter = self.rate_limiter   # pace the engine's reads
        if self.cfg.live:
            if self.submitter is None:
                # hard-gated; raises unless opted in. Shares the global req/s budget.
                self.submitter = build_clob_signer(rate_limiter=self.rate_limiter)
            log.warning("LIVE submitter armed — real orders will be placed")
        elif self.cfg.dry_live and self.submitter is None:
            self.submitter = DryRunSubmitter()
            log.info("DRY-LIVE: rehearsing the live code path (orders logged, not sent)")

        self._start_ws()               # real-time book (any mode) + fills (live)
        self._rehydrate()              # adopt maker quotes that survived a prior run
        self._reconcile_broker_orders()  # LIVE: cancel orphaned on-chain orders
        try:
            # a standing KILL must block ALL placement on (re)start
            reason = self._kill_check()
            if reason:
                self.trip_kill(reason)
                return
            self.rediscover()                  # initial SYNC scan so reselect has data
            self.reselect()
            self._start_discovery_thread()     # subsequent scans run OFF the hot loop
            last_reeval = last_stats = time.monotonic()
            next_poll = time.monotonic()

            while not self._stop:
                reason = self._kill_check()
                if reason:
                    self.trip_kill(reason)
                    break
                now = time.monotonic()
                if self.cfg.stats_every_s > 0 and now - last_stats >= self.cfg.stats_every_s:
                    self._log_stats()           # req/s + load average (throughput/CPU)
                    last_stats = now
                # Discovery now runs on a background thread (keeps self.report +
                # _last_scan_ok fresh); the loop only does reeval/reselect on their
                # own beat and the fast poll. reselect reads the latest bg scan.
                if now - last_reeval >= self.cfg.reeval_interval_s:
                    self._tick_cooldowns()      # tick on the reeval beat (gates re-entry)
                    self.reevaluate_held()      # degradation exit + opportunity rotation
                    self.reselect()             # redeploy freed capital
                    last_reeval = now
                self.poll_once()
                # DEADLINE scheduling: sleep only the time this cycle's work did NOT
                # already consume, instead of a fixed sleep stacked on top of it. The
                # heartbeat — and therefore cancel latency, the profit lever — stays a
                # true poll_seconds as the book grows. If a cycle overruns the budget
                # (book too large / network slow), sleep 0, warn, and re-anchor rather
                # than drift ever further behind.
                next_poll += self.cfg.poll_seconds
                delay = next_poll - time.monotonic()
                if delay < 0:
                    log.warning("poll cycle overran the %.0fs budget by %.1fs — not "
                                "sleeping (consider fewer pools or faster network)",
                                self.cfg.poll_seconds, -delay)
                    next_poll = time.monotonic()   # re-anchor the cadence
                    delay = 0.0
                self._sleep_fn(delay)
        finally:
            self._shutdown()   # ALWAYS cancel/flatten on any exit (kill, signal, crash)

    def _ensure_engine(self) -> None:
        """Open the engine WITHOUT wiping a surviving ledger. init_account is a full
        reset (deletes maker_quotes), so on a restart we must NOT call it when an
        account already exists — otherwise we'd forget held quotes and re-place
        duplicates while the old orders rest orphaned on-chain."""
        if self.engine is None:
            self.engine = Engine(Path(self.cfg.state_dir))
        try:
            self.engine.get_account()        # raises NotInitializedError if brand new
        except NotInitializedError:
            self.engine.init_account(self.cfg.capital)  # fresh start: cash = budget

    def _rehydrate(self) -> None:
        """Adopt maker quotes that survived a restart (the engine's SQLite ledger
        persists them). Without this the runner forgets the pre-restart book and
        re-places duplicate orders while the old ones rest orphaned on-chain.

        NOTE: in LIVE this restores ledger tracking + the ability to re-center/exit
        the pre-restart orders, but it does NOT yet reconcile against the broker's
        actually-resting orders — see the open 'broker order reconciliation' item.
        """
        if self.engine is None:
            return
        now = time.monotonic()
        adopted = 0
        for q in self.engine.get_maker_quotes():
            cond = q.get("market_condition_id")
            if not cond or cond in self.placed:
                continue
            self.placed[cond] = {
                "condition_id": cond, "token": q.get("token_id", ""),
                "question": q.get("market_slug", ""),
                "half_spread_c": q.get("half_spread_c"),
                "daily": q.get("daily_rate", 0.0), "share": 0.0,
                "committed_capital": q.get("committed_capital", 0.0),
                "est_daily_reward": 0.0,
            }
            self.placed_at[cond] = now
            adopted += 1
        if adopted:
            log.warning("rehydrated %d maker quote(s) from a prior run", adopted)

    def _reconcile_broker_orders(self) -> None:
        """LIVE restart safety: cancel any REAL resting order with no adopted quote
        (truly orphaned from a prior run) so the broker state matches the ledger
        before the loop starts placing again. Orders on adopted tokens are left for
        the loop to re-center/manage."""
        if not self.cfg.live or self.submitter is None:
            return
        lister = getattr(self.submitter, "list_open_orders", None)
        canceller = getattr(self.submitter, "cancel_order", None)
        if lister is None or canceller is None:
            return
        held_tokens = {m.get("token") for m in self.placed.values()}
        try:
            open_orders = lister()
        except Exception as e:  # noqa: BLE001
            log.warning("open-order reconcile failed (leaving orders as-is): %s", e)
            return
        for o in open_orders:
            token = o.get("asset_id") or o.get("token_id")
            oid = o.get("id") or o.get("orderID") or o.get("order_id")
            if oid and token not in held_tokens:
                log.warning("cancelling ORPHANED resting order %s on %s",
                            oid, str(token)[:10])
                try:
                    canceller(oid)
                except Exception as e:  # noqa: BLE001
                    log.warning("cancel orphan failed: %s", e)

    def _install_signals(self) -> None:
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                signal.signal(sig, lambda *_s: self.trip_kill("signal"))
            except (ValueError, OSError):
                pass

    def trip_kill(self, reason: str) -> None:
        if not self._stop:
            self._kill_reason = reason
            self._stop = True
            log.warning("KILL-SWITCH: %s — cancelling all + standing down", reason)

    # -- real-time WS channels ---------------------------------------------

    def _start_ws(self) -> None:
        """Start the WS channels: market (book/mid, ANY mode → engine.book_source)
        and user (real fills, LIVE only). Best-effort — a WS failure just leaves the
        engine on REST (book_source unset / fills via the submitter)."""
        if not self.cfg.ws_enabled:
            return
        # serialize submitter access: the WS reflex + the poll thread both cancel
        if self.submitter is not None and not isinstance(self.submitter, _LockingSubmitter):
            self.submitter = _LockingSubmitter(self.submitter)
        try:
            self._market_ch = MarketChannel()
            self._market_ch.set_price_callback(self._on_ws_price)  # reflex cancel
            self._market_ch.start()
            if hasattr(self.engine, "book_source"):
                self.engine.book_source = self._market_ch
            log.info("WS market channel started (real-time book + reflex cancel)")
            self._start_book_resync()        # authoritative REST /book anti-drift
        except Exception as e:  # noqa: BLE001
            log.warning("WS market channel failed to start (using REST): %s", e)
            self._market_ch = None
        if self.cfg.live and self.submitter is not None:
            get_creds = getattr(self.submitter, "api_creds", None)
            if callable(get_creds):
                try:
                    self._user_ch = UserChannel(
                        get_creds(),
                        invert_side=getattr(self.submitter, "_invert_side", False))
                    self._user_ch.start()
                    log.info("WS user channel started (real-time fills)")
                except Exception as e:  # noqa: BLE001
                    log.warning("WS user channel failed (REST fills): %s", e)
                    self._user_ch = None

    def _held_tokens(self) -> list[str]:
        if self.engine is None:
            return []
        try:
            return [q["token_id"] for q in self.engine.get_maker_quotes()]
        except Exception:  # noqa: BLE001
            return []

    def _start_book_resync(self) -> None:
        """Continuously re-pull authoritative REST /book for every held token to keep
        the WS book correct. ``resync_workers`` threads fetch concurrently; each GET
        is a LOW-priority bucket read, so they SOAK the leftover budget
        (149 − writes − reserve) yet self-limit and never starve cancels (which hold
        the reserved lane). No fixed rate — the bucket + network latency pace it."""
        if (self._market_ch is None or self.cfg.resync_workers <= 0
                or self.rate_limiter is None):
            return
        self._resync_pool = ThreadPoolExecutor(max_workers=self.cfg.resync_workers,
                                               thread_name_prefix="resync")

        def _loop() -> None:
            while not self._discovery_stop.is_set():
                # held tokens from the in-memory reflex map (lock-protected) — NOT the
                # engine: sqlite is single-thread, so a bg thread must never touch it.
                with self._reflex_lock:
                    tokens = list(self._reflex_refs)
                if not tokens:
                    if self._discovery_stop.wait(0.5):
                        break
                    continue
                # one /book per held token per round, concurrently; each blocks on the
                # low-priority bucket, so the round (and thus the loop) is bucket-paced.
                futs = [self._resync_pool.submit(self._resync_once, t) for t in tokens]
                for f in futs:
                    try:
                        f.result()
                    except Exception:  # noqa: BLE001
                        pass

        self._resync_thread = threading.Thread(target=_loop, name="book-resync",
                                               daemon=True)
        self._resync_thread.start()

    def _resync_once(self, token: str) -> None:
        """One authoritative REST /book pull -> overwrite the WS cache. Best effort."""
        try:
            book = self.scanner.book(token)          # paced by the shared 149 bucket
            if book:
                self._market_ch.apply_rest_snapshot(token, book)
        except Exception as e:  # noqa: BLE001
            log.debug("book resync failed for %s: %s", str(token)[:10], e)

    def _log_stats(self) -> None:
        """Log achieved request rate (read/write split) + load average — a side gauge
        of throughput and CPU load. Rate = grants since the last call / elapsed."""
        rl = self.rate_limiter
        if rl is None:
            return
        now = time.monotonic()
        g, gl = rl.granted, rl.granted_low
        if self._stats_at is not None and now > self._stats_at:
            dt = now - self._stats_at
            dg = (g - self._stats_granted[0]) / dt
            dgl = (gl - self._stats_granted[1]) / dt
            try:
                load1 = os.getloadavg()[0]
            except (OSError, AttributeError):
                load1 = -1.0
            log.info("stats: %.1f req/s (read %.1f / write %.1f, cap %.0f) | "
                     "load %.2f | pools %d | threads %d",
                     dg, dgl, dg - dgl, self.cfg.max_req_per_sec,
                     load1, len(self.placed), threading.active_count())
        self._stats_at = now
        self._stats_granted = (g, gl)
        self._log_pool_metrics()

    def _mid_velocity(self, token: str) -> float:
        """Recent mid velocity in cents/sec: sum |Δmid| over the per-poll history
        window / its span. 0 if too few samples. A book-chop / pick-off-risk gauge."""
        hist = self._mid_hist.get(token)
        if not hist or len(hist) < 2 or hist[-1][0] <= hist[0][0]:
            return 0.0
        moves = sum(abs(hist[i][1] - hist[i - 1][1]) for i in range(1, len(hist)))
        return moves / (hist[-1][0] - hist[0][0]) * 100.0

    def _log_pool_metrics(self) -> None:
        """Per held pool (main thread → sqlite ok): depth AHEAD of our quote (queue
        gauge) + book movement speed (WS updates/s + mid velocity). Logged + emitted
        as a 'metrics' event for review."""
        if self.engine is None or self._market_ch is None:
            return
        try:
            quotes = self.engine.get_maker_quotes()
        except Exception:  # noqa: BLE001
            return
        now = time.monotonic()
        for q in quotes:
            tok = q.get("token_id")
            book = self._market_ch.get_book(tok)
            if not tok or book is None:
                continue
            mid = self._market_ch.get_midpoint(tok)
            hs = (q.get("half_spread_c") or 0.0) / 100.0
            bid_px, ask_px = round(mid - hs, 4), round(mid + hs, 4)
            b_better, b_at = depth_ahead(book, bid_px, "bid")
            a_better, a_at = depth_ahead(book, ask_px, "ask")
            vel = self._mid_velocity(tok)        # cents/s, and WS updates/s
            cnt = self._market_ch.updates(tok)
            prev = self._ws_upd_at.get(tok)
            upd_s = (cnt - prev[1]) / (now - prev[0]) if prev and now > prev[0] else 0.0
            self._ws_upd_at[tok] = (now, cnt)
            log.info("metrics %s | ahead bid %.0f(+%.0f@lvl)@%.3f / ask %.0f(+%.0f@lvl)@%.3f "
                     "| mid %.4f vel %.2fc/s | book %.1f upd/s",
                     str(tok)[:8], b_better, b_at, bid_px, a_better, a_at, ask_px,
                     mid, vel, upd_s)
            self._event("metrics", token=tok, mid=mid, bid_px=bid_px, ask_px=ask_px,
                        ahead_bid=b_better, at_bid=b_at, ahead_ask=a_better, at_ask=a_at,
                        mid_vel_cps=round(vel, 3), book_upd_s=round(upd_s, 2))

    def _sync_ws_subscriptions(self) -> None:
        """Point the WS channels at the currently-held pools + refresh reflex refs."""
        if self._market_ch is not None:
            self._market_ch.set_tokens(self._held_tokens())
        if self._user_ch is not None:
            self._user_ch.set_markets(list(self.placed.keys()))
        self._refresh_reflex_refs()

    def _refresh_reflex_refs(self) -> None:
        """Rebuild the per-token (centred mid, band) the reflex compares against —
        from the ledger's current quotes (their last_mid is where we're quoting)."""
        refs: dict[str, tuple[float, float]] = {}
        if self.engine is not None:
            try:
                for q in self.engine.get_maker_quotes():
                    tick = q.get("tick", 0.01) or 0.01
                    band = max(1, self.cfg.recenter_ticks) * tick
                    refs[q["token_id"]] = (q.get("last_mid", 0.0), band)
            except Exception:  # noqa: BLE001
                return
        with self._reflex_lock:
            self._reflex_refs = refs

    def _on_ws_price(self, token: str, mid: float) -> None:
        """WS price callback (reader thread): the instant a held pool's mid moves
        beyond its band, CANCEL its resting orders — pulling the stale quote before
        it's picked off. No ledger/reward work here (decoupled); the next poll
        reposts it via force_recenter so the pool is never left uncovered."""
        with self._reflex_lock:
            ref = self._reflex_refs.get(token)
            if ref is None or token in self._reflex_cancelled:
                return
            ref_mid, band = ref
            if abs(mid - ref_mid) < band:
                return
            self._reflex_cancelled.add(token)      # claim it before the slow I/O
        try:
            self.submitter({"action": "CANCEL_ALL", "token_id": token})
            log.info("WS reflex CANCEL %s (mid %.4f moved >= %.3f from %.4f)",
                     str(token)[:10], mid, band, ref_mid)
            self._event("reflex_cancel", token=token, mid=mid, ref=ref_mid, band=band)
        except Exception as e:  # noqa: BLE001 — let the next move/poll retry
            log.warning("reflex cancel failed for %s: %s", str(token)[:10], e)
            with self._reflex_lock:
                self._reflex_cancelled.discard(token)

    # -- discovery / selection ---------------------------------------------

    def _start_discovery_thread(self) -> None:
        """Run the heavy reward-universe scan on a BACKGROUND thread so it never
        blocks the poll/cancel loop. The scan (seconds long, every
        ``discovery_interval_s``) updates ``self.report`` + ``self._last_scan_ok``;
        the main loop reads them (atomic reference swap under the GIL — no lock
        needed). The initial scan already ran synchronously in :meth:`run`, so the
        thread WAITS before its first scan. Disabled when the interval is <= 0."""
        if self.cfg.discovery_interval_s <= 0:
            return

        def _loop() -> None:
            while not self._discovery_stop.is_set():
                if self._discovery_stop.wait(self.cfg.discovery_interval_s):
                    break                       # stop signalled during the wait
                self.rediscover()               # own try/except + staleness guard

        self._discovery_thread = threading.Thread(
            target=_loop, name="discovery", daemon=True)
        self._discovery_thread.start()

    def rediscover(self) -> None:
        try:
            self.report = scan(self.scanner, min_daily=self.cfg.min_daily,
                               top=self.cfg.scan_top, with_jump_risk=True)
            self._last_scan_ok = time.monotonic()
            log.info("discovery: %d safe of %d scored",
                     self.report.get("safe_count", 0), self.report.get("pools_scored", 0))
            # snapshot the universe for review (top candidates + their key stats)
            self._event("discovery", safe=self.report.get("safe_count", 0),
                        scored=self.report.get("pools_scored", 0),
                        top=[{"cond": p.get("condition_id"), "q": p.get("question"),
                              "daily": p.get("daily"), "share": p.get("share"),
                              "jump": p.get("jump_verdict")}
                             for p in (self.report.get("pools") or [])[:10]])
        except Exception as e:  # noqa: BLE001 — keep the LAST good report; staleness guard handles it
            log.warning("discovery scan failed: %s", e)

    def _report_stale(self) -> bool:
        """The universe scan hasn't succeeded recently -> don't add/rotate pools off
        stale data (held pools are still managed via reevaluate_held + poll)."""
        if self._last_scan_ok is None:
            return True
        return (time.monotonic() - self._last_scan_ok) > 2.0 * self.cfg.discovery_interval_s

    def reselect(self) -> None:
        """Converge the held book to the freshly-computed IDEAL selection — this IS
        opportunity-cost rotation, done correctly (select_pools already ranks by
        risk-adjusted yield and respects capital + correlation + cooldown):
          - DROP held pools no longer in the ideal set (cancel/flatten/retire),
          - ADD ideal pools not yet held.
        Stable when the universe is stable (held == ideal -> no churn)."""
        if self._report_stale():
            log.warning("discovery report stale -> skipping reselect (held pools "
                        "still managed); waiting for a fresh scan")
            return
        cd = {k for k, v in self.cooldown.items() if v > 0}
        self.selected = select_pools(
            self.report, capital=self.cfg.capital, max_pools=self.cfg.max_pools,
            half_spread_ticks=self.cfg.half_spread_ticks,
            risk_tolerance_days=self.cfg.risk_tolerance_days,
            max_token_overlap=self.cfg.max_token_overlap, cooldown=cd,
        )
        want = {s["condition_id"]: s for s in self.selected}
        # DROP held pools that fell out of the ideal selection (rank-out rotation)
        if self.engine is not None:
            quotes_by_cond = {q["market_condition_id"]: q
                              for q in self.engine.get_maker_quotes()}
            for cond in list(self.placed):
                if cond not in want:
                    q = quotes_by_cond.get(cond)
                    if q is not None:
                        self._exit_held(cond, q, "deselected")  # no cooldown: may return
                    else:
                        self.placed.pop(cond, None)
                        self.placed_at.pop(cond, None)
        # ADD newly selected pools, never exceeding the capital budget (the engine
        # cash gate can drift with P&L, so enforce the budget here too).
        committed = sum(m.get("committed_capital", 0.0) for m in self.placed.values())
        for cond, s in want.items():
            if cond in self.placed:
                continue
            if (s.get("est_daily_reward") or 0.0) < self.cfg.min_pool_reward:
                continue   # too little reward (share x daily) to justify the capital lock
                           # — NOT share alone: a small share of a big pool still pays
            cap = s.get("committed_capital", 0.0)
            if committed + cap > self.cfg.capital + 1e-6:
                continue   # would exceed the budget given what's already held
            try:
                self._place(cond, s)
                committed += cap
                self.placed[cond] = s
                self.placed_at[cond] = time.monotonic()
                log.info("placed %s | %s | daily=$%.0f", cond[:10],
                         s["question"][:48], s["daily"])
                self._event("place", cond=cond, q=s.get("question"),
                            daily=s.get("daily"), share=s.get("share"),
                            committed=cap)
            except Exception as e:  # noqa: BLE001 — one bad market mustn't sink the book
                log.warning("place failed for %s: %s", cond[:10], e)
        log.info("active book: %d pools (selected %d)", len(self.placed), len(self.selected))
        self._sync_ws_subscriptions()   # point WS at the new held set

    # -- continuous re-evaluation of held pools ----------------------------

    def reevaluate_held(self) -> None:
        """Re-score every HELD pool against CURRENT conditions and exit the ones
        that degraded (rewards cut, competition flooded our share, jump-risk rose,
        book went one-sided) or that a clearly better pool should replace
        (opportunity-cost rotation). Freed capital is redeployed by reselect().

        Runs in ALL modes (paper / dry-live / live): paper just retires the ledger
        quote; dry-live logs the cancel; live cancels + flattens for real.
        """
        if not self.cfg.reeval_enabled or not self.placed or self.engine is None:
            return
        quotes_by_cond = {q["market_condition_id"]: q
                          for q in self.engine.get_maker_quotes()}
        now = time.monotonic()
        # Build the worklist on the main thread (DB read + anti-churn gating); drop
        # held pools the engine already exited.
        worklist: list[tuple[str, dict]] = []
        for cond, pool in list(self.placed.items()):
            q = quotes_by_cond.get(cond)
            if q is None:                       # engine already exited it (reconcile/drift)
                self.placed.pop(cond, None)
                self.placed_at.pop(cond, None)
                continue
            if now - self.placed_at.get(cond, 0.0) < self.cfg.min_hold_s:
                continue                        # anti-churn: respect the minimum hold
            worklist.append((cond, q))
        if not worklist:
            return
        # Re-score every held pool CONCURRENTLY — independent read-only I/O, and
        # _rescore swallows its own errors to None so no worker raises. The exit
        # decisions (cancel/flatten + ledger writes) then run SERIALLY below on the
        # main thread, preserving order and keeping sqlite single-threaded.
        def _score(cq: tuple[str, dict]) -> dict | None:
            cond, q = cq
            return self._rescore(cond, q["token_id"], own_size=q.get("size", 0.0),
                                 own_half_spread_c=q.get("half_spread_c", 0.0))

        workers = min(REEVAL_WORKERS, len(worklist))
        with ThreadPoolExecutor(max_workers=workers) as ex:
            fresh_list = list(ex.map(_score, worklist))
        for (cond, q), fresh in zip(worklist, fresh_list):
            if fresh is None:
                continue                        # transient read failure — try next round
            reason = self._degrade_reason(fresh)
            # real-time chop guard: a fast-moving book bleeds via many small pick-offs
            # that the daily jump_verdict misses — exit on sustained mid velocity.
            if not reason and self.cfg.max_mid_vel_cps > 0:
                vel = self._mid_velocity(q["token_id"])
                if vel > self.cfg.max_mid_vel_cps:
                    reason = "fast_book"
                    log.info("re-eval %s mid velocity %.2fc/s > %.2f -> fast_book",
                             cond[:10], vel, self.cfg.max_mid_vel_cps)
            if reason:
                self._exit_held(cond, q, reason)

    def _rescore(self, condition_id: str, token_id: str, *, own_size: float = 0.0,
                own_half_spread_c: float = 0.0) -> dict | None:
        """Fresh score for one held pool from live data (current daily/share/jump).

        The public book includes OUR OWN resting order, so score_pool's share is
        understated; subtract our binding-side Qmin to recover the true competitor
        share (otherwise re-eval spuriously flags share_collapsed in LIVE)."""
        try:
            cfg = self.engine.api.get_reward_config(condition_id)
        except Exception:  # noqa: BLE001
            return None
        if not cfg or cfg.get("daily", 0) <= 0:
            return {"daily": 0.0}               # left the program -> exit
        try:
            book = self.scanner.book(token_id)
            history = self.scanner.prices_history(token_id)
        except Exception:  # noqa: BLE001
            return None
        scored = score_pool(cfg, book, history)
        if scored is None:
            # book went one-sided/empty -> can't quote two-sided. This is a
            # degradation (exit), NOT a transient read failure (None).
            return {"one_sided": True}
        c = scored.get("max_spread_c", 0.0)
        if own_size > 0 and own_half_spread_c > 0 and c > 0:
            own_q = own_size * ((c - own_half_spread_c) / c) ** 2
            competitor = max(0.0, scored.get("min_side_score", 0.0) - own_q)
            scored["share"] = round(reward_share(cfg["min_size"], cfg["tick"], c, competitor), 4)
            scored["reward_per_day"] = round(scored["share"] * cfg["daily"], 2)
        return scored                           # share / reward_per_day / jump_verdict / empty_band

    # reasons that bench a pool for a few rounds (it degraded, don't immediately
    # re-add it). "deselected" (a clean rank-out) is NOT benched.
    _COOLDOWN_REASONS = ("jump_risk_rose", "empty_band", "share_collapsed",
                         "daily_cut", "one_sided", "fast_book")

    def _degrade_reason(self, fresh: dict) -> str | None:
        """Decide whether a held pool degraded enough to exit. Returns a reason or
        None. Rotation (a better pool appeared) is handled by reselect converging to
        the ideal selection — NOT here — so this only judges THIS pool on its own."""
        if fresh.get("one_sided"):
            return "one_sided"
        daily = fresh.get("daily", 0.0) or 0.0
        if daily <= 0:
            return "rewards_ended"
        if daily < self.cfg.min_daily:
            return "daily_cut"
        if fresh.get("jump_verdict") in ("WATCH", "KILL"):
            return "jump_risk_rose"
        if fresh.get("empty_band"):
            return "empty_band"
        if fresh.get("share", 1.0) < self.cfg.min_share:
            return "share_collapsed"
        return None

    def _exit_held(self, cond: str, quote: dict, reason: str) -> None:
        """Cancel + (live) flatten + retire a held pool, in any mode."""
        token, qid, inv = quote["token_id"], quote["id"], quote.get("inventory", 0.0)
        if self._use_live_path() and self.submitter is not None:
            self.submitter({"action": "CANCEL_ALL", "token_id": token})
            if self.cfg.live:
                self.engine._flatten_live(self.submitter, token, inv)
        self.engine.cancel_maker_quote(qid)     # cancels DB quote + frees committed capital
        self.placed.pop(cond, None)
        self.placed_at.pop(cond, None)
        if reason in self._COOLDOWN_REASONS:    # degraded -> bench a few rounds
            self.cooldown[cond] = self.cfg.cooldown_rounds
        log.info("re-eval EXIT %s (%s)", cond[:10], reason)
        self._event("exit", cond=cond, reason=reason, inventory=inv)

    def _use_live_path(self) -> bool:
        """Drive the engine's *_live methods (real LIVE, or DRY-LIVE rehearsal)."""
        return (self.cfg.live or self.cfg.dry_live) and self.submitter is not None

    def _place(self, condition_id: str, pool: dict) -> None:
        hs = pool["half_spread_c"]
        if self.cfg.use_optimal_spread:
            try:  # vol-aware net-optimal offset (reuses the tested engine model)
                rec = self.engine.suggest_maker_half_spread(
                    condition_id, poll_seconds=self.cfg.poll_seconds)
                hs = rec.get("half_spread_c") or hs
            except Exception as e:  # noqa: BLE001
                log.warning("optimal-spread calc failed for %s: %s; using %.2fc",
                            condition_id[:10], e, hs)
        if self._use_live_path():
            self.engine.place_maker_quote_live(
                condition_id, submitter=self.submitter, half_spread_cents=hs)
        else:
            self.engine.place_maker_quote(condition_id, half_spread_cents=hs)

    def _tick_cooldowns(self) -> None:
        for k in list(self.cooldown):
            self.cooldown[k] -= 1
            if self.cooldown[k] <= 0:
                del self.cooldown[k]

    # -- the poll (drives the engine maker loop) ---------------------------

    def poll_once(self) -> list[dict]:
        if self._use_live_path():
            fills_by_token = self._poll_fills()   # DryRunSubmitter -> {} (no fills)
            with self._reflex_lock:               # reflex-pulled tokens -> force repost
                forced = self._reflex_cancelled
                self._reflex_cancelled = set()
            rows = self.engine.accrue_maker_rewards_live(
                submitter=self.submitter, fills_by_token=fills_by_token,
                recenter_ticks=self.cfg.recenter_ticks, force_recenter=forced)
        else:
            rows = self.engine.accrue_maker_rewards()
        now_m = time.monotonic()
        for row in rows:
            q = row.get("quote") or {}
            tok = q.get("token_id")
            mid_v = row.get("mid")
            if tok and isinstance(mid_v, (int, float)) and mid_v > 0:  # for velocity
                self._mid_hist.setdefault(tok, deque(maxlen=120)).append((now_m, mid_v))
            # throttle the per-pool "poll" heartbeat (always log fills/reconcile/exit)
            notable = bool(row.get("fills_applied") or row.get("reconciled")
                           or row.get("exit_failed"))
            due = (tok not in self._poll_evt_at
                   or now_m - self._poll_evt_at[tok] >= self.cfg.event_poll_every_s)
            if self._events is not None and (notable or due):
                self._poll_evt_at[tok] = now_m
                self._event("poll", cond=q.get("market_condition_id"),
                            token=tok, mid=row.get("mid"),
                            reward=row.get("reward"), share=row.get("share"),
                            inventory=row.get("inventory"),
                            inv_pnl_delta=row.get("inventory_pnl_delta"),
                            fills=row.get("fills_applied"),
                            reconciled=row.get("reconciled"),
                            exit_failed=row.get("exit_failed"))
            if row.get("exit_failed"):
                # the engine could NOT cancel/flatten the real orders — the quote is
                # still active and tracked; surface loudly and let it retry next poll.
                cond = row["quote"]["market_condition_id"]
                log.error("EXIT FAILED for %s (%s) — real order/position may survive; "
                          "retrying next poll", cond[:10], row["exit_failed"])
                continue
            reason = row.get("reconciled")
            if reason:
                cond = row["quote"]["market_condition_id"]
                self.placed.pop(cond, None)
                self.placed_at.pop(cond, None)
                if reason != "rewards_ended":   # a jump/drift -> bench it a while
                    self.cooldown[cond] = self.cfg.cooldown_rounds
                log.info("pool %s exited (%s)", cond[:10], reason)
        self._refresh_reflex_refs()             # refs track the new last_mid / book
        return rows

    def _poll_fills(self) -> dict:
        """Group the account's REAL fills since last poll by token. Prefers the WS
        user channel (real-time push); falls back to the submitter's REST poll."""
        if self._user_ch is not None:
            poll = self._user_ch.poll_fills
        else:
            poll = getattr(self.submitter, "poll_fills", None)
        if poll is None:
            return {}
        try:
            fills = poll()
        except Exception as e:  # noqa: BLE001
            log.warning("fill poll failed: %s", e)
            return {}
        out: dict = {}
        for f in fills:
            out.setdefault(f.get("token_id"), []).append(f)
            log.info("FILL %s %s %.2f @ %.4f", str(f.get("token_id"))[:10],
                     f.get("side"), f.get("size", 0), f.get("price", 0))
        return out

    # -- kill-switch -------------------------------------------------------

    def _kill_check(self) -> str | None:
        if os.path.exists(self.cfg.kill_file) or os.path.exists(
            os.path.join(self.cfg.state_dir, self.cfg.kill_file)
        ):
            return "kill-file"
        if self.engine is not None:
            try:
                inv_pnl = self.engine.get_maker_summary().get("inventory_pnl", 0.0)
            except Exception:  # noqa: BLE001
                inv_pnl = 0.0
            if inv_pnl <= -self.cfg.max_loss:
                return f"max-loss (maker inventory P&L ${inv_pnl:.2f})"
        # INDEPENDENT wallet-balance floor: trips on the REAL USDC balance, immune to
        # any ledger self-report drift (operator sets LM_MIN_WALLET_USDC).
        if self.cfg.live and self.cfg.min_wallet_usdc > 0 and self.submitter is not None:
            reader = getattr(self.submitter, "usdc_balance", None)
            bal = reader() if reader is not None else None
            if bal is not None and bal < self.cfg.min_wallet_usdc:
                return f"wallet-floor (real USDC ${bal:.2f} < ${self.cfg.min_wallet_usdc:.2f})"
        return None

    # -- shutdown ----------------------------------------------------------

    def _shutdown(self) -> None:
        # stop the background discovery thread first (it holds no orders/ledger state)
        self._discovery_stop.set()
        for t in (self._discovery_thread, self._resync_thread):
            if t is not None:
                t.join(timeout=5.0)
        if self._resync_pool is not None:
            self._resync_pool.shutdown(wait=False)   # drop in-flight /book fetches
        for ch in (self._market_ch, self._user_ch):   # close WS channels (no state)
            if ch is not None:
                try:
                    ch.stop()
                except Exception:  # noqa: BLE001
                    pass
        if self._events is not None:
            self._events.close()
        log.warning("shutdown (%s): cancelling all maker quotes", self._kill_reason or "stop")
        if self.engine is not None:
            try:
                for q in self.engine.get_maker_quotes():
                    if self._use_live_path():
                        self.submitter({"action": "CANCEL_ALL", "token_id": q["token_id"]})
                        if self.cfg.live:
                            self.engine._flatten_live(self.submitter, q["token_id"], q["inventory"])
                    self.engine.cancel_maker_quote(q["id"])
            except Exception as e:  # noqa: BLE001
                log.warning("shutdown cleanup error: %s", e)
            self.engine.close()
        try:
            self.scanner.close()
        except Exception:  # noqa: BLE001
            pass
        closer = getattr(self.submitter, "close", None)   # stop the keep-warm thread
        if callable(closer):
            try:
                closer()
            except Exception:  # noqa: BLE001
                pass
        log.warning("live-maker stopped.")


def main() -> None:
    cfg = RunnerConfig.from_env()
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    # also persist a rolling N-day file log (stdout alone is ephemeral)
    try:
        from logging.handlers import TimedRotatingFileHandler
        os.makedirs(cfg.state_dir, exist_ok=True)
        handlers.append(TimedRotatingFileHandler(
            os.path.join(cfg.state_dir, "runner.log"),
            when="midnight", backupCount=cfg.retention_days))
    except Exception:  # noqa: BLE001 — file logging is best-effort, never block startup
        pass
    logging.basicConfig(
        level=os.environ.get("LM_LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=handlers,
    )
    # CRITICAL at a fast cadence: httpx/httpcore log EVERY request at INFO. With a
    # 1s poll + 20/s /book re-sync + reflex that is millions of lines/day → silence
    # them to WARNING. websocket-client's per-frame logs likewise.
    for noisy in ("httpx", "httpcore", "websocket", "hpack"):
        logging.getLogger(noisy).setLevel(logging.WARNING)
    LiveRunner(cfg).run()


if __name__ == "__main__":
    main()
