"""Autonomous live-maker runner — the no-MCP driver.

Replaces the agent/MCP that used to call the maker poll by hand: a single SYNC
process that polls on a fixed cadence (the same beat as before — REST ``/book``
+ ``/midpoint`` every ``poll_seconds``, full reward-universe re-scan every
``discovery_interval_s``), exactly like the paper engine's ``accrue_maker_rewards``
was meant to be called. No WebSocket, no LLM in the loop.

  rediscover (periodic scan) -> select_pools (budget+decorrelate+cooldown)
    -> MakerPortfolio (dry-run, or LIVE with a real py-clob-client submitter)
      -> each poll: pull books -> (live) apply REAL fills -> step each bot
         (re-quote / cancel) -> retire+cooldown halted pools -> kill-switch

Real-money submission flows ONLY when ``PM_TRADER_LIVE=1`` (the operator switch);
otherwise the bots run dry-run and submit nothing.
"""

from __future__ import annotations

import logging
import os
import signal
import time
from dataclasses import dataclass, field
from pathlib import Path

from pm_trader.maker_live import build_clob_signer
from pm_trader.portfolio import MakerPortfolio, fetch_market_data, select_pools
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
    max_loss: float = 20.0              # kill-switch: conservative book MTM floor
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
                f"min_daily=${self.min_daily:.0f} | max_loss/day=${self.max_loss:.0f}")


class LiveRunner:
    """The autonomous polling loop. Inject ``client``/``submitter`` in tests."""

    def __init__(self, config: RunnerConfig, *, client: RewardsClient | None = None,
                 submitter=None, sleeper=time.sleep) -> None:
        self.cfg = config
        self.client = client or RewardsClient()
        self.submitter = submitter  # set in run() for live, unless injected
        self._sleep_fn = sleeper
        self.report: dict = {"pools": []}
        self.selected: list[dict] = []
        self.portfolio: MakerPortfolio | None = None
        self.cooldown: dict[str, int] = {}     # token/condition_id -> rounds left
        self.cash_flow: dict[str, float] = {}  # token -> sells$ - buys$ (real fills)
        self.last_mid: dict[str, float] = {}
        self._stop = False
        self._kill_reason = ""

    # -- lifecycle ----------------------------------------------------------

    def run(self) -> None:
        logging.getLogger("pm_trader").info(self.cfg.banner())
        os.makedirs(self.cfg.state_dir, exist_ok=True)
        self._install_signals()
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
                pass  # not in main thread

    def trip_kill(self, reason: str) -> None:
        if not self._stop:
            self._kill_reason = reason
            self._stop = True
            log.warning("KILL-SWITCH: %s — cancelling all + standing down", reason)

    # -- discovery / selection ---------------------------------------------

    def rediscover(self) -> None:
        try:
            self.report = scan(self.client, min_daily=self.cfg.min_daily,
                               top=self.cfg.scan_top, with_jump_risk=True)
            log.info("discovery: %d safe of %d scored",
                     self.report.get("safe_count", 0), self.report.get("pools_scored", 0))
        except Exception as e:  # noqa: BLE001 — keep the loop alive on a scan hiccup
            log.warning("discovery scan failed: %s", e)

    def reselect(self) -> None:
        cd = {k for k, v in self.cooldown.items() if v > 0}
        self.selected = select_pools(
            self.report, capital=self.cfg.capital, max_pools=self.cfg.max_pools,
            half_spread_ticks=self.cfg.half_spread_ticks,
            risk_tolerance_days=self.cfg.risk_tolerance_days,
            max_token_overlap=self.cfg.max_token_overlap, cooldown=cd,
        )
        # cancel any orders on pools we are dropping before rebuilding the book
        if self.portfolio is not None:
            keep = {s["token"] for s in self.selected}
            for token in list(self.portfolio.bots):
                if token not in keep:
                    self._cancel_token(token)
        self.portfolio = MakerPortfolio(
            self.selected, dry_run=not self.cfg.live, submitter=self.submitter)
        log.info("active book: %d pools, est reward $%.2f/day, committed $%.0f",
                 len(self.selected),
                 sum(s["est_daily_reward"] for s in self.selected),
                 sum(s["committed_capital"] for s in self.selected))

    def _tick_cooldowns(self) -> None:
        for k in list(self.cooldown):
            self.cooldown[k] -= 1
            if self.cooldown[k] <= 0:
                del self.cooldown[k]

    # -- the poll ----------------------------------------------------------

    def poll_once(self) -> list[dict]:
        if self.portfolio is None or not self.selected:
            return []
        try:
            market_data = fetch_market_data(self.client, self.selected)
        except Exception as e:  # noqa: BLE001
            log.warning("market-data fetch failed: %s", e)
            return []
        for token, (_book, mid) in market_data.items():
            self.last_mid[token] = mid

        if self.cfg.live and self.submitter is not None:
            self._apply_real_fills()

        plans = self.portfolio.plan_all(market_data)
        for plan in plans:
            if plan.get("halted"):
                self._retire(plan["token_id"], cooldown=True, reason="jump")
        return plans

    def _apply_real_fills(self) -> None:
        """Poll the account's REAL trades and feed them to the bots (the operator's
        chosen fill source). Only the live submitter exposes ``poll_fills``."""
        poll = getattr(self.submitter, "poll_fills", None)
        if poll is None:
            return
        try:
            fills = poll()
        except Exception as e:  # noqa: BLE001
            log.warning("fill poll failed: %s", e)
            return
        for f in fills:
            token = f.get("token_id")
            bot = self.portfolio.bots.get(token) if self.portfolio else None
            if bot is not None:
                bot.apply_real_fill(f["side"], f["size"])
            signed = (1.0 if f["side"] == "SELL" else -1.0) * f["size"] * f["price"]
            self.cash_flow[token] = self.cash_flow.get(token, 0.0) + signed
            log.info("FILL %s %s %.2f @ %.4f", str(token)[:10], f["side"], f["size"], f["price"])

    # -- pool retirement / cancel ------------------------------------------

    def _retire(self, token: str, *, cooldown: bool, reason: str) -> None:
        if self.portfolio is not None:
            meta = self.portfolio.meta.get(token, {})
            self.portfolio.bots.pop(token, None)
            self.portfolio.meta.pop(token, None)
            if cooldown:
                self.cooldown[meta.get("condition_id", token)] = self.cfg.cooldown_rounds
                self.cooldown[token] = self.cfg.cooldown_rounds
        log.info("retired %s (%s)", str(token)[:10], reason)

    def _cancel_token(self, token: str) -> None:
        if self.submitter is not None:
            try:
                self.submitter({"action": "CANCEL_ALL", "token_id": token})
            except Exception as e:  # noqa: BLE001
                log.warning("cancel failed for %s: %s", str(token)[:10], e)

    # -- kill-switch -------------------------------------------------------

    def book_mtm(self) -> float:
        """Conservative book P&L from REAL fills (excludes reward income, which PM
        pays separately) — a lower bound for the loss kill-switch."""
        total = sum(self.cash_flow.values())
        inv = self.portfolio.bots if self.portfolio else {}
        for token, bot in inv.items():
            mid = self.last_mid.get(token)
            if mid is not None:
                total += bot.inventory * mid
        return total

    def _kill_check(self) -> str | None:
        if os.path.exists(self.cfg.kill_file) or os.path.exists(
            os.path.join(self.cfg.state_dir, self.cfg.kill_file)
        ):
            return "kill-file"
        mtm = self.book_mtm()
        if mtm <= -self.cfg.max_loss:
            return f"max-loss/day (book MTM ${mtm:.2f})"
        return None

    # -- shutdown ----------------------------------------------------------

    def _shutdown(self) -> None:
        log.warning("shutdown (%s): cancelling all orders", self._kill_reason or "stop")
        if self.portfolio is not None:
            for token in list(self.portfolio.bots):
                self._cancel_token(token)
        try:
            self.client.close()
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
