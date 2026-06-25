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
from pm_trader.maker_live import build_clob_signer
from pm_trader.portfolio import select_pools
from pm_trader.rewards import RewardsClient, scan

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
    capital: float = 200.0
    max_pools: int = 3
    min_daily: float = 80.0
    scan_top: int = 60
    half_spread_ticks: int = 1
    risk_tolerance_days: float = 7.0
    max_token_overlap: int = 1
    poll_seconds: float = 60.0          # same beat as the paper maker poll
    discovery_interval_s: float = 1800.0
    cooldown_rounds: int = 3
    max_loss: float = 20.0              # kill-switch: maker inventory-PnL floor
    state_dir: str = "state"
    kill_file: str = "KILL"

    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "RunnerConfig":
        load_dotenv()
        return cls(
            live=os.environ.get("PM_TRADER_LIVE", "0").strip() == "1",
            capital=_f("LM_CAPITAL", 200.0),
            max_pools=_i("LM_MAX_POOLS", 3),
            min_daily=_f("LM_MIN_DAILY", 80.0),
            scan_top=_i("LM_SCAN_TOP", 60),
            poll_seconds=_f("LM_POLL_SECONDS", 60.0),
            discovery_interval_s=_f("LM_DISCOVERY_INTERVAL_S", 1800.0),
            cooldown_rounds=_i("LM_COOLDOWN_ROUNDS", 3),
            max_loss=_f("LM_MAX_LOSS_PER_DAY", 20.0),
        )

    def banner(self) -> str:
        mode = "LIVE — REAL MONEY" if self.live else "DRY-RUN (no orders sent)"
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
        self.cooldown: dict[str, int] = {}   # condition_id -> discovery rounds left
        self._stop = False
        self._kill_reason = ""

    # -- lifecycle ----------------------------------------------------------

    def run(self) -> None:
        logging.getLogger("pm_trader").info(self.cfg.banner())
        os.makedirs(self.cfg.state_dir, exist_ok=True)
        self._install_signals()
        if self.engine is None:
            self.engine = Engine(Path(self.cfg.state_dir))
            self.engine.init_account(self.cfg.capital)  # ledger cash = capital budget
        if self.cfg.live and self.submitter is None:
            self.submitter = build_clob_signer()  # hard-gated; raises unless opted in
            log.warning("LIVE submitter armed — real orders will be placed")

        self.rediscover()
        self.reselect()
        last_discovery = time.monotonic()

        while not self._stop:
            reason = self._kill_check()
            if reason:
                self.trip_kill(reason)
                break
            if time.monotonic() - last_discovery >= self.cfg.discovery_interval_s:
                self._tick_cooldowns()
                self.rediscover()
                self.reselect()
                last_discovery = time.monotonic()
            self.poll_once()
            self._sleep_fn(self.cfg.poll_seconds)

        self._shutdown()

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
        """Re-rank the universe and PLACE any newly selected pool. De-selected but
        still-paying pools keep their resting quote (the engine retires a pool only
        on reconcile/drift-exit) — selection only ADDS."""
        cd = {k for k, v in self.cooldown.items() if v > 0}
        self.selected = select_pools(
            self.report, capital=self.cfg.capital, max_pools=self.cfg.max_pools,
            half_spread_ticks=self.cfg.half_spread_ticks,
            risk_tolerance_days=self.cfg.risk_tolerance_days,
            max_token_overlap=self.cfg.max_token_overlap, cooldown=cd,
        )
        for s in self.selected:
            cond = s["condition_id"]
            if cond in self.placed or cond in cd:
                continue
            try:
                self._place(cond, s)
                self.placed[cond] = s
                log.info("placed %s | %s | daily=$%.0f", cond[:10],
                         s["question"][:48], s["daily"])
            except Exception as e:  # noqa: BLE001 — one bad market mustn't sink the book
                log.warning("place failed for %s: %s", cond[:10], e)
        log.info("active book: %d pools (selected %d)", len(self.placed), len(self.selected))

    def _place(self, condition_id: str, pool: dict) -> None:
        hs = pool["half_spread_c"]
        if self.cfg.live:
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
        if self.cfg.live and self.submitter is not None:
            fills_by_token = self._poll_fills()
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
                    if self.cfg.live and self.submitter is not None:
                        self.submitter({"action": "CANCEL_ALL", "token_id": q["token_id"]})
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
