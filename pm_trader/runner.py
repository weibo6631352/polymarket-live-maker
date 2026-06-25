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
import time
from dataclasses import dataclass, field
from pathlib import Path

from pm_trader.engine import Engine
from pm_trader.maker_live import DryRunSubmitter, build_clob_signer
from pm_trader.models import NotInitializedError
from pm_trader.portfolio import select_pools
from pm_trader.rewards import RewardsClient, scan, score_pool

log = logging.getLogger("pm_trader.runner")


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
    risk_tolerance_days: float = 7.0
    max_token_overlap: int = 1
    poll_seconds: float = 60.0          # same beat as the paper maker poll
    discovery_interval_s: float = 600.0   # full reward-universe re-scan cadence (10 min)
    cooldown_rounds: int = 3
    max_loss: float = 20.0              # kill-switch: maker inventory-PnL floor
    # continuous re-evaluation of HELD pools (degradation exit + opportunity rotation)
    reeval_enabled: bool = True
    reeval_interval_s: float = 300.0    # re-check held pools this often (cheap; held-only)
    min_share: float = 0.02            # leave if our est share collapses below this
    min_hold_s: float = 600.0          # don't soft-exit a pool held less than this
    state_dir: str = "state"
    kill_file: str = "KILL"

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
            reeval_enabled=os.environ.get("LM_REEVAL", "1").strip() != "0",
            reeval_interval_s=_f("LM_REEVAL_INTERVAL_S", 300.0),
            min_share=_f("LM_MIN_SHARE", 0.02),
            min_hold_s=_f("LM_MIN_HOLD_S", 600.0),
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
        self._stop = False
        self._kill_reason = ""

    # -- lifecycle ----------------------------------------------------------

    def run(self) -> None:
        logging.getLogger("pm_trader").info(self.cfg.banner())
        os.makedirs(self.cfg.state_dir, exist_ok=True)
        self._install_signals()
        self._ensure_engine()
        if self.cfg.live:
            if self.submitter is None:
                self.submitter = build_clob_signer()  # hard-gated; raises unless opted in
            log.warning("LIVE submitter armed — real orders will be placed")
        elif self.cfg.dry_live and self.submitter is None:
            self.submitter = DryRunSubmitter()
            log.info("DRY-LIVE: rehearsing the live code path (orders logged, not sent)")

        self._rehydrate()   # adopt maker quotes that survived a prior run / crash
        try:
            # a standing KILL must block ALL placement on (re)start
            reason = self._kill_check()
            if reason:
                self.trip_kill(reason)
                return
            self.rediscover()
            self.reselect()
            last_discovery = last_reeval = time.monotonic()

            while not self._stop:
                reason = self._kill_check()
                if reason:
                    self.trip_kill(reason)
                    break
                now = time.monotonic()
                refreshed = False
                if now - last_discovery >= self.cfg.discovery_interval_s:
                    self.rediscover()           # full universe re-scan
                    last_discovery = now
                    refreshed = True
                if refreshed or (now - last_reeval >= self.cfg.reeval_interval_s):
                    self._tick_cooldowns()      # tick on the reeval beat (gates re-entry)
                    self.reevaluate_held()      # degradation exit + opportunity rotation
                    self.reselect()             # redeploy freed capital
                    last_reeval = now
                self.poll_once()
                self._sleep_fn(self.cfg.poll_seconds)
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

    def rediscover(self) -> None:
        try:
            self.report = scan(self.scanner, min_daily=self.cfg.min_daily,
                               top=self.cfg.scan_top, with_jump_risk=True)
            log.info("discovery: %d safe of %d scored",
                     self.report.get("safe_count", 0), self.report.get("pools_scored", 0))
        except Exception as e:  # noqa: BLE001
            log.warning("discovery scan failed: %s", e)

    def reselect(self) -> None:
        """Converge the held book to the freshly-computed IDEAL selection — this IS
        opportunity-cost rotation, done correctly (select_pools already ranks by
        risk-adjusted yield and respects capital + correlation + cooldown):
          - DROP held pools no longer in the ideal set (cancel/flatten/retire),
          - ADD ideal pools not yet held.
        Stable when the universe is stable (held == ideal -> no churn)."""
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
        # ADD newly selected pools
        for cond, s in want.items():
            if cond in self.placed:
                continue
            try:
                self._place(cond, s)
                self.placed[cond] = s
                self.placed_at[cond] = time.monotonic()
                log.info("placed %s | %s | daily=$%.0f", cond[:10],
                         s["question"][:48], s["daily"])
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
        for cond, pool in list(self.placed.items()):
            q = quotes_by_cond.get(cond)
            if q is None:                       # engine already exited it (reconcile/drift)
                self.placed.pop(cond, None)
                self.placed_at.pop(cond, None)
                continue
            if now - self.placed_at.get(cond, 0.0) < self.cfg.min_hold_s:
                continue                        # anti-churn: respect the minimum hold
            fresh = self._rescore(cond, q["token_id"])
            if fresh is None:
                continue                        # transient read failure — try next round
            reason = self._degrade_reason(fresh)
            if reason:
                self._exit_held(cond, q, reason)

    def _rescore(self, condition_id: str, token_id: str) -> dict | None:
        """Fresh score for one held pool from live data (current daily/share/jump)."""
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

    def _use_live_path(self) -> bool:
        """Drive the engine's *_live methods (real LIVE, or DRY-LIVE rehearsal)."""
        return (self.cfg.live or self.cfg.dry_live) and self.submitter is not None

    def _place(self, condition_id: str, pool: dict) -> None:
        hs = pool["half_spread_c"]
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
                submitter=self.submitter, fills_by_token=fills_by_token)
        else:
            rows = self.engine.accrue_maker_rewards()
        for row in rows:
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
        return None

    # -- shutdown ----------------------------------------------------------

    def _shutdown(self) -> None:
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
    logging.basicConfig(
        level=os.environ.get("LM_LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    LiveRunner(RunnerConfig.from_env()).run()


if __name__ == "__main__":
    main()
