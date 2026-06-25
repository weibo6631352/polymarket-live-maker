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
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from pm_trader.engine import Engine
from pm_trader.events import EventLog
from pm_trader.maker_live import DryRunSubmitter, build_clob_signer
from pm_trader.models import NotInitializedError
from pm_trader.portfolio import select_pools
from pm_trader.rewards import RewardsClient, reward_share, scan, score_pool

log = logging.getLogger("pm_trader.runner")

# Concurrency cap for the held-pool re-score fetch in reevaluate_held(). Each
# held pool's re-score is 3 independent read-only CLOB GETs; fetching the held
# book concurrently keeps the reeval beat from stalling the loop as the book grows.
REEVAL_WORKERS = 16


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
    min_share: float = 0.02            # leave if our est share collapses below this
    min_hold_s: float = 600.0          # don't soft-exit a pool held less than this
    state_dir: str = "state"
    kill_file: str = "KILL"
    # rolling data retention (days) for the event log + equity curve + rotated logs
    retention_days: int = 30
    events_enabled: bool = True        # append-only per-poll/discovery event log

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
            reeval_enabled=os.environ.get("LM_REEVAL", "1").strip() != "0",
            reeval_interval_s=_f("LM_REEVAL_INTERVAL_S", 300.0),
            min_share=_f("LM_MIN_SHARE", 0.02),
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
        self.scanner = scanner_client or RewardsClient()
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
        if self.cfg.live:
            if self.submitter is None:
                self.submitter = build_clob_signer()  # hard-gated; raises unless opted in
            log.warning("LIVE submitter armed — real orders will be placed")
        elif self.cfg.dry_live and self.submitter is None:
            self.submitter = DryRunSubmitter()
            log.info("DRY-LIVE: rehearsing the live code path (orders logged, not sent)")

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
            last_reeval = time.monotonic()
            next_poll = time.monotonic()

            while not self._stop:
                reason = self._kill_check()
                if reason:
                    self.trip_kill(reason)
                    break
                now = time.monotonic()
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
                         "daily_cut", "one_sided")

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
            rows = self.engine.accrue_maker_rewards_live(
                submitter=self.submitter, fills_by_token=fills_by_token,
                recenter_ticks=self.cfg.recenter_ticks)
        else:
            rows = self.engine.accrue_maker_rewards()
        for row in rows:
            if self._events is not None:        # granular per-pool series for review
                q = row.get("quote") or {}
                self._event("poll", cond=q.get("market_condition_id"),
                            token=q.get("token_id"), mid=row.get("mid"),
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
        return rows

    def _poll_fills(self) -> dict:
        """Group the account's REAL trades since last poll by token."""
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
        if self._discovery_thread is not None:
            self._discovery_thread.join(timeout=5.0)
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
    LiveRunner(cfg).run()


if __name__ == "__main__":
    main()
