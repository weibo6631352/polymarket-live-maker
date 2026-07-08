#!/usr/bin/env python3
"""Mania regime gate for the tail-vendor.

Pause a coin's up-tail selling when a trailing signal flags mania onset — the one regime where
selling crypto up-tails blows up (2021: SOL +12%/3d terminal hit 25-42% vs ~5% in calm periods,
clustered across quarters). Validated 2026-07-08 (scratchpad/regime_switch.py): trailing-30d
realized vol cleanly separates forward tail-hit (3% at <70% vol -> 17% at >147%), and — critically —
it LEADS: it flagged the 2021 SOL blow-up in Dec-2020, ~2 months before the Feb-2021 hits, and stayed
paused through the whole 16x move. Self-calibrating: pauses SOL ~37% of days, BTC ~3%.

Gate = pause if trailing-30d annualized realized vol > VOL_TH  OR  trailing-20d return > MOM_TH.
"Leave margin" (user directive): thresholds set on the early/conservative side; tighten VOL_TH to
0.80-0.90 for even more margin. Fail-CLOSED: if price data can't be fetched, pause (conservative).
"""
import json, urllib.request, urllib.parse, math, statistics

VOL_TH = 1.00          # annualized 30d realized vol; >100% = mania regime
MOM_TH = 0.50          # 20d return; >+50% = mania regime
SYMS = {"SOL": "SOLUSDT", "XRP": "XRPUSDT", "BTC": "BTCUSDT", "ETH": "ETHUSDT"}
_HOSTS = ("https://data-api.binance.vision", "https://api.binance.com")


def _closes(sym, limit=40):
    last = None
    for h in _HOSTS:
        try:
            u = f"{h}/api/v3/klines?" + urllib.parse.urlencode(dict(symbol=sym, interval="1d", limit=limit))
            with urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "tv-regime/1"}), timeout=10) as r:
                return [float(k[4]) for k in json.loads(r.read().decode())]
        except Exception as e:
            last = e
    raise last


def coin_gate(coins=("SOL", "XRP", "BTC", "ETH")):
    """Return {coin: {paused, error, vol, mom, reason}}.

    'error' distinguishes a DATA-provider failure (couldn't compute the signal) from a genuine
    mania pause. The caller MUST treat error separately: a data outage is NOT evidence of mania —
    if it were folded into 'paused' it would (a) empty the whitelist on a transient Binance blip and
    (b) trip validate_curation's paused-aware relaxations. On error we still set paused=True
    (never sell a coin whose regime is unknown) but flag error=True so make_whitelist aborts the run
    (keeps the last-good whitelist) instead of emitting a shrunken/empty one.
    """
    out = {}
    for c in coins:
        try:
            cl = _closes(SYMS[c])
            if len(cl) < 31:
                raise ValueError("short series")
            rets = [math.log(cl[i] / cl[i - 1]) for i in range(len(cl) - 30, len(cl))]
            vol = statistics.pstdev(rets) * math.sqrt(365)
            mom = cl[-1] / cl[-21] - 1
            if vol > VOL_TH:
                out[c] = dict(paused=True, error=False, vol=vol, mom=mom, reason=f"vol{100*vol:.0f}%>{100*VOL_TH:.0f}")
            elif mom > MOM_TH:
                out[c] = dict(paused=True, error=False, vol=vol, mom=mom, reason=f"mom+{100*mom:.0f}%>{100*MOM_TH:.0f}")
            else:
                out[c] = dict(paused=False, error=False, vol=vol, mom=mom, reason="ok")
        except Exception as e:
            out[c] = dict(paused=True, error=True, vol=None, mom=None, reason=f"fetch_fail:{type(e).__name__}")
    return out


if __name__ == "__main__":
    g = coin_gate()
    print("mania regime gate (VOL_TH=%.0f%% MOM_TH=+%.0f%%):" % (100 * VOL_TH, 100 * MOM_TH))
    for c, s in g.items():
        v = f"{100*s['vol']:.0f}%" if s['vol'] is not None else "  -"
        m = f"{100*s['mom']:+.0f}%" if s['mom'] is not None else "  -"
        print(f"  {c}: {'PAUSE' if s['paused'] else 'sell ':5}  vol30={v:>5}  mom20={m:>5}  [{s['reason']}]")
