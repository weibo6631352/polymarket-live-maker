"""Periodic market discovery — re-scan the FULL reward-pool universe on a
schedule. Migrated from ``pm_trader/discovery.py``, async.

Reward pools churn constantly (new ones funded, others resolve, jump-risk
shifts), so the SAFE candidate set must be refreshed, not scanned once.
``refresh`` runs one full (paginated, uncapped) scan and returns the current SAFE
pools (optionally persisting them); ``watch`` repeats it every ``interval_s``.
Read-only — discovery never places orders.
"""

from __future__ import annotations

import asyncio
import json

from live_maker.scanner import AsyncRewardsClient, scan


async def refresh(
    client: AsyncRewardsClient, *, out_path: str | None = None, **scan_kwargs
) -> dict:
    """One full scan -> a SAFE-pool summary, optionally persisted to ``out_path``."""
    report = await scan(client, **scan_kwargs)
    safe = [p for p in report["pools"]
            if p["jump_verdict"] == "SAFE" and not p["empty_band"]]
    summary = {
        "total_reward_pools": report["total_reward_pools"],
        "pools_scored": report["pools_scored"],
        "safe_count": len(safe),
        "safe": safe,
    }
    if out_path is not None:
        with open(out_path, "w") as f:
            json.dump(summary, f, indent=2)
    return summary


async def watch(
    client: AsyncRewardsClient,
    *,
    interval_s: float,
    rounds: int,
    out_path: str | None = None,
    **scan_kwargs,
) -> list[dict]:
    """Re-run ``refresh`` ``rounds`` times, ``interval_s`` apart."""
    results: list[dict] = []
    for i in range(max(0, rounds)):
        results.append(await refresh(client, out_path=out_path, **scan_kwargs))
        if i < rounds - 1:
            await asyncio.sleep(interval_s)
    return results
