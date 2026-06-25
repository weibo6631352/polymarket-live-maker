"""Post-hoc review & reward reconciliation — turn the run's data into conclusions.

Three data sources, joined per market:
  - ESTIMATED rewards + realized bleed/inventory P&L: the engine ledger
    (``maker_quotes``, kept for active AND exited quotes).
  - DECISIONS + discovery: the append-only event log (``state/events``).
  - ACTUAL rewards PAID: the public, auth-free data-api ``/activity?type=REWARD``
    feed — the on-chain USDC reward distributions to the wallet.

Reconciling the bot's ESTIMATE against the ACTUAL payout is the point: the
strategy's share estimate is a book snapshot, while Polymarket pays a time-weighted
daily settlement, so the two can diverge a lot — and only this comparison tells you
by how much. All read-only; nothing here places or cancels orders.
"""

from __future__ import annotations

import json
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

import httpx

DATA_API = "https://data-api.polymarket.com"
_TIMEOUT = httpx.Timeout(20.0)


# --------------------------------------------------------------------------
# ACTUAL rewards — the truth source (data-api /activity?type=REWARD)
# --------------------------------------------------------------------------

def fetch_actual_rewards(wallet: str, *, start: int | None = None,
                         end: int | None = None, http: httpx.Client | None = None,
                         page: int = 500, max_pages: int = 50) -> list[dict]:
    """Actual reward payouts for ``wallet``: ``[{ts, condition_id, usdc, tx}]``.

    Hits the public data-api ``/activity`` filtered to ``type=REWARD`` (each row is
    a real on-chain USDC reward distribution). Paginates by offset; ``start``/``end``
    are unix seconds. ``http`` is injectable for tests. Best-effort: returns what it
    could fetch, never raises on a transient page error.
    """
    client = http or httpx.Client(timeout=_TIMEOUT)  # trust_env -> honors proxy
    out: list[dict] = []
    seen_tx: set = set()
    try:
        for i in range(max_pages):
            params: dict = {"user": wallet, "type": "REWARD",
                            "limit": page, "offset": i * page}
            if start is not None:
                params["start"] = int(start)
            if end is not None:
                params["end"] = int(end)
            resp = client.get(f"{DATA_API}/activity", params=params)
            resp.raise_for_status()
            rows = resp.json()
            if not isinstance(rows, list) or not rows:
                break
            new = 0
            for r in rows:
                tx = r.get("transactionHash")
                key = (tx, r.get("conditionId"), r.get("timestamp"))
                if key in seen_tx:
                    continue          # guard against an offset that doesn't advance
                seen_tx.add(key)
                out.append({
                    "ts": r.get("timestamp"),
                    "condition_id": r.get("conditionId"),
                    "usdc": float(r.get("usdcSize") or r.get("size") or 0.0),
                    "tx": tx,
                })
                new += 1
            if len(rows) < page or new == 0:
                break
    finally:
        if http is None:
            client.close()
    return out


# --------------------------------------------------------------------------
# Event log loading
# --------------------------------------------------------------------------

def load_events(events_dir, *, days: int | None = None) -> list[dict]:
    """Load events from ``events-*.jsonl`` files, optionally only the last N days."""
    d = Path(events_dir)
    if not d.exists():
        return []
    cutoff = None
    if days is not None:
        cutoff = (datetime.now(timezone.utc) - timedelta(days=days)).strftime("%Y%m%d")
    out: list[dict] = []
    for p in sorted(d.glob("events-*.jsonl")):
        day = p.stem.replace("events-", "")
        if cutoff is not None and day < cutoff:
            continue
        for line in p.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
    return out


# --------------------------------------------------------------------------
# Summary — join ledger (estimate) + events (decisions) + actual (truth)
# --------------------------------------------------------------------------

def summarize(quotes: list, events: list[dict], actual: list[dict]) -> dict:
    """Build the review report from ledger quotes, events, and actual rewards.

    ``quotes`` are ledger MakerQuote objects (active + exited). ``events`` are
    event-log dicts. ``actual`` is :func:`fetch_actual_rewards` output.
    """
    # estimate side, grouped per market (sum across that market's quotes)
    pools: dict[str, dict] = {}
    for q in quotes:
        cond = q.market_condition_id
        p = pools.setdefault(cond, {
            "condition_id": cond, "slug": q.market_slug,
            "est_reward": 0.0, "bleed": 0.0, "inv_pnl": 0.0, "fills": 0,
            "actual_reward": 0.0, "n_quotes": 0, "status": q.status,
        })
        p["est_reward"] += q.accrued_rewards
        p["bleed"] += q.realized_bleed
        p["inv_pnl"] += q.inventory_pnl
        p["fills"] += q.fills
        p["n_quotes"] += 1
        p["status"] = q.status          # last seen

    # actual side, grouped per market
    actual_by_cond: dict[str, float] = defaultdict(float)
    for a in actual:
        actual_by_cond[a.get("condition_id")] += a.get("usdc", 0.0)
    for cond, amt in actual_by_cond.items():
        pools.setdefault(cond, {
            "condition_id": cond, "slug": "", "est_reward": 0.0, "bleed": 0.0,
            "inv_pnl": 0.0, "fills": 0, "actual_reward": 0.0, "n_quotes": 0,
            "status": "",
        })["actual_reward"] += amt

    for p in pools.values():
        p["net_est"] = round(p["est_reward"] + p["inv_pnl"], 4)
        p["est_reward"] = round(p["est_reward"], 4)
        p["actual_reward"] = round(p["actual_reward"], 4)
        p["bleed"] = round(p["bleed"], 4)
        p["inv_pnl"] = round(p["inv_pnl"], 4)

    # decisions + discovery from the event log
    places = sum(1 for e in events if e.get("kind") == "place")
    exits_by_reason: dict[str, int] = defaultdict(int)
    for e in events:
        if e.get("kind") == "exit":
            exits_by_reason[e.get("reason") or "?"] += 1
    discos = [e for e in events if e.get("kind") == "discovery"]
    avg_safe = round(sum(e.get("safe", 0) for e in discos) / len(discos), 1) if discos else 0.0

    est_total = round(sum(p["est_reward"] for p in pools.values()), 4)
    act_total = round(sum(p["actual_reward"] for p in pools.values()), 4)
    bleed_total = round(sum(p["bleed"] for p in pools.values()), 4)
    inv_pnl_total = round(sum(p["inv_pnl"] for p in pools.values()), 4)
    ratio = round(act_total / est_total, 3) if est_total > 0 else None

    return {
        "estimated_reward_total": est_total,
        "actual_reward_total": act_total,
        "reconciliation_ratio": ratio,          # actual / estimated (None if no est)
        "bleed_total": bleed_total,
        "inventory_pnl_total": inv_pnl_total,
        "net_estimated": round(est_total + inv_pnl_total, 4),
        "net_actual": round(act_total + inv_pnl_total, 4),
        "decisions": {"places": places, "exits_by_reason": dict(exits_by_reason)},
        "discovery": {"scans": len(discos), "avg_safe_pools": avg_safe},
        "per_pool": sorted(pools.values(), key=lambda p: -p["actual_reward"] - p["est_reward"]),
    }


def format_report(report: dict) -> str:
    """Render the review report as human-readable text."""
    r = report
    ratio = r["reconciliation_ratio"]
    ratio_s = f"{ratio:.2f}x" if ratio is not None else "n/a (no estimate)"
    lines = [
        "=" * 64,
        "MAKER REVIEW — estimate vs ACTUAL reward reconciliation",
        "=" * 64,
        f"estimated reward : ${r['estimated_reward_total']:.2f}",
        f"ACTUAL reward    : ${r['actual_reward_total']:.2f}   (actual/est = {ratio_s})",
        f"adverse bleed    : ${r['bleed_total']:.2f}",
        f"inventory P&L    : ${r['inventory_pnl_total']:.2f}",
        f"net (estimated)  : ${r['net_estimated']:.2f}",
        f"net (ACTUAL)     : ${r['net_actual']:.2f}",
        "-" * 64,
        f"decisions: {r['decisions']['places']} placements | "
        f"exits: {r['decisions']['exits_by_reason'] or '{}'}",
        f"discovery: {r['discovery']['scans']} scans | "
        f"avg {r['discovery']['avg_safe_pools']} safe pools/scan",
        "-" * 64,
        f"{'market':<28} {'est$':>8} {'actual$':>9} {'bleed$':>8} {'fills':>6} {'status':>9}",
    ]
    for p in r["per_pool"][:25]:
        name = (p["slug"] or p["condition_id"] or "")[:27]
        lines.append(f"{name:<28} {p['est_reward']:>8.2f} {p['actual_reward']:>9.2f} "
                     f"{p['bleed']:>8.2f} {p['fills']:>6} {p['status']:>9}")
    if ratio is not None and ratio < 0.6:
        lines.append("-" * 64)
        lines.append(f"⚠ actual is only {ratio:.0%} of estimate — share is being "
                     "over-estimated (book snapshot vs Polymarket's full-day settlement).")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Orchestrator + CLI entry
# --------------------------------------------------------------------------

def run_review(state_dir: str = "state", *, wallet: str | None = None,
               days: int = 10, http: httpx.Client | None = None) -> dict:
    """Load ledger + events, fetch actual rewards, and return the report dict."""
    from pm_trader.db import Database
    from pm_trader.orders import get_all_maker_quotes

    db = Database(Path(state_dir))
    try:
        quotes = get_all_maker_quotes(db.conn)
    except Exception:  # noqa: BLE001 — fresh/empty ledger
        quotes = []
    events = load_events(Path(state_dir) / "events", days=days)
    actual: list[dict] = []
    if wallet:
        start = int((datetime.now(timezone.utc) - timedelta(days=days)).timestamp())
        actual = fetch_actual_rewards(wallet, start=start, http=http)
    db.close()
    return summarize(quotes, events, actual)


def main() -> None:
    import os
    import sys

    from pm_trader.runner import load_dotenv
    load_dotenv()
    state_dir = os.environ.get("LM_STATE_DIR", "state")
    wallet = os.environ.get("POLYMARKET_FUNDER") or None
    days = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    if not wallet:
        print("(no POLYMARKET_FUNDER set — showing estimate only, no actual "
              "reconciliation)")
    print(format_report(run_review(state_dir, wallet=wallet, days=days)))


if __name__ == "__main__":
    main()
