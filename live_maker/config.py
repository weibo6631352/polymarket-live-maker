"""Configuration — pools/capital/risk params + secrets from the environment.

The private key is read from the environment ONLY (``.env`` is gitignored). The
real-money switch ``PM_LIVE`` defaults OFF: unless it is exactly ``"1"`` the bot
runs in dry-run (computes + logs every order/cancel, sends nothing). Going live
is the operator's explicit action — nothing here flips it automatically.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path


def load_dotenv(path: str | os.PathLike = ".env") -> dict[str, str]:
    """Minimal ``.env`` reader: ``KEY=VALUE`` lines into ``os.environ`` (without
    overriding already-set vars). No dependency on python-dotenv. Returns the
    parsed pairs. Silently no-ops if the file is absent.
    """
    p = Path(path)
    parsed: dict[str, str] = {}
    if not p.exists():
        return parsed
    for raw in p.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        parsed[key] = val
        os.environ.setdefault(key, val)
    return parsed


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
class Config:
    """All tunables for one live-maker process. Construct via :meth:`from_env`."""

    # --- secrets / identity (env only) ---
    private_key: str = ""
    wallet: str | None = None

    # --- real-money gate (operator-owned) ---
    live: bool = False           # True only when PM_LIVE == "1"

    # --- capital & book sizing ---
    capital: float = 200.0       # total USDC budget across the book
    max_pools: int = 3
    min_daily: float = 80.0      # ignore reward pools below this $/day
    half_spread_ticks: int = 1   # quote one tick inside the band
    risk_tolerance_days: float = 7.0
    max_token_overlap: int = 1
    require_safe: bool = True

    # --- risk / kill-switch ---
    max_loss_per_day: float = 20.0    # halt + flatten if realized loss exceeds this
    max_inventory_mult: float = 5.0   # per-pool inventory cap = mult * min_size (shares)
    cooldown_rounds: int = 3          # discovery rounds a jumped pool stays benched

    # --- timing ---
    discovery_interval_s: float = 1800.0  # full reward-universe re-scan cadence
    reconcile_interval_s: float = 120.0   # re-check each pool's reward config
    book_poll_s: float = 15.0             # REST order-book failsafe cadence
    ws_stale_s: float = 30.0              # if no WS event in this long -> force a REST poll

    # --- io ---
    state_dir: str = "state"
    kill_file: str = "KILL"      # touch this file to trip the kill-switch

    # scan pull breadth (how many top-daily pools to score per scan)
    scan_top: int = 60

    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls, *, dotenv: str | None = ".env") -> "Config":
        if dotenv is not None:
            load_dotenv(dotenv)
        live = os.environ.get("PM_LIVE", "0").strip() == "1"
        return cls(
            private_key=os.environ.get("POLYMARKET_PRIVATE_KEY", ""),
            wallet=os.environ.get("POLYMARKET_WALLET_ADDRESS") or None,
            live=live,
            capital=_f("LM_CAPITAL", 200.0),
            max_pools=_i("LM_MAX_POOLS", 3),
            min_daily=_f("LM_MIN_DAILY", 80.0),
            max_loss_per_day=_f("LM_MAX_LOSS_PER_DAY", 20.0),
            max_inventory_mult=_f("LM_MAX_INVENTORY_MULT", 5.0),
            discovery_interval_s=_f("LM_DISCOVERY_INTERVAL_S", 1800.0),
            reconcile_interval_s=_f("LM_RECONCILE_INTERVAL_S", 120.0),
            book_poll_s=_f("LM_BOOK_POLL_S", 15.0),
            ws_stale_s=_f("LM_WS_STALE_S", 30.0),
            scan_top=_i("LM_SCAN_TOP", 60),
        )

    def require_key(self) -> None:
        """Raise if no signing key is present (needed even for dry-run auth)."""
        if not self.private_key:
            raise RuntimeError(
                "POLYMARKET_PRIVATE_KEY is not set. Copy .env.example to .env and "
                "fill it in (the key is read from the environment only)."
            )

    def banner(self) -> str:
        mode = "LIVE — REAL MONEY" if self.live else "DRY-RUN (no orders sent)"
        return (
            f"live-maker | mode={mode} | capital=${self.capital:.0f} | "
            f"max_pools={self.max_pools} | min_daily=${self.min_daily:.0f} | "
            f"max_loss/day=${self.max_loss_per_day:.0f}"
        )
