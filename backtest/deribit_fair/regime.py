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

VOL_TH = 1.00          # annualized 30d realized vol; >100% = mania regime (L3, lagging)
MOM_TH = 0.50          # 20d return; >+50% = mania regime (L3, lagging)
# L1/L2 LEADING entry-gate triggers — validated 2026-07-12 (rally_defense_backtest.py, commit b348835):
# pausing NEW sells when these fire skips the worst tail entries (avg -3..-4.5c vs -1c kept) and cuts
# maxDD 36% (BTC)/45% (ETH); BTC-2021 kept-book turns +0.88c/pos. (TRIMMING held positions was REFUTED —
# whipsaws, negative in 2021 — so the gate only pauses NEW sells; held positions ride to resolution.)
RET7_TH = 0.10         # 7d return > +10%  -> pause (leading momentum)
RET3_TH = 0.07         # 3d return > +7%   -> pause (leading momentum, faster spike)
VOLEXP_TH = 1.5        # 7d/60d realized-vol expansion > 1.5x -> pause (vol regime shifting up)
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
            cl = _closes(SYMS[c], limit=70)     # 70d: enough for the 60d vol-expansion baseline
            if len(cl) < 31:
                raise ValueError("short series")
            lr = [math.log(cl[i] / cl[i - 1]) for i in range(1, len(cl))]
            vol = statistics.pstdev(lr[-30:]) * math.sqrt(365)        # L3: 30d realized vol
            mom = cl[-1] / cl[-21] - 1                                # L3: 20d return
            ret7 = cl[-1] / cl[-8] - 1                                # L2: 7d return
            ret3 = cl[-1] / cl[-4] - 1                                # L2: 3d return
            vol7 = statistics.pstdev(lr[-7:]) * math.sqrt(365) if len(lr) >= 7 else None
            vol60 = statistics.pstdev(lr[-60:]) * math.sqrt(365) if len(lr) >= 60 else None
            volexp = (vol7 / vol60) if (vol7 and vol60 and vol60 > 0) else None
            # most-severe trigger first; L3 (mania) then L2 (leading entry-gate). Held positions are never
            # trimmed by this — the gate only excludes the coin from the whitelist = pause NEW sells.
            reason = None
            if vol > VOL_TH:                    reason = f"vol{100*vol:.0f}%>{100*VOL_TH:.0f}"          # L3
            elif mom > MOM_TH:                  reason = f"mom+{100*mom:.0f}%>{100*MOM_TH:.0f}"         # L3
            elif ret3 > RET3_TH:                reason = f"3d+{100*ret3:.0f}%>{100*RET3_TH:.0f}"        # L2
            elif ret7 > RET7_TH:                reason = f"7d+{100*ret7:.0f}%>{100*RET7_TH:.0f}"        # L2
            elif volexp is not None and volexp > VOLEXP_TH:
                                                reason = f"volexp{volexp:.1f}x>{VOLEXP_TH}"             # L2
            out[c] = dict(paused=reason is not None, error=False, vol=vol, mom=mom,
                          ret7=ret7, ret3=ret3, volexp=volexp, reason=reason or "ok")
        except Exception as e:
            out[c] = dict(paused=True, error=True, vol=None, mom=None,
                          ret7=None, ret3=None, volexp=None, reason=f"fetch_fail:{type(e).__name__}")
    return out


if __name__ == "__main__":
    g = coin_gate()
    print("regime gate  L3(vol>%.0f%% / mom20>+%.0f%%)  L2(3d>+%.0f%% / 7d>+%.0f%% / volexp>%.1fx):"
          % (100 * VOL_TH, 100 * MOM_TH, 100 * RET3_TH, 100 * RET7_TH, VOLEXP_TH))
    for c, s in g.items():
        v = f"{100*s['vol']:.0f}%" if s.get('vol') is not None else "  -"
        m = f"{100*s['mom']:+.0f}%" if s.get('mom') is not None else "  -"
        r7 = f"{100*s['ret7']:+.0f}%" if s.get('ret7') is not None else "  -"
        r3 = f"{100*s['ret3']:+.0f}%" if s.get('ret3') is not None else "  -"
        ve = f"{s['volexp']:.1f}x" if s.get('volexp') is not None else "  -"
        print(f"  {c}: {'PAUSE' if s['paused'] else 'sell ':5}  vol30={v:>5} mom20={m:>5} 3d={r3:>5} 7d={r7:>5} volexp={ve:>5}  [{s['reason']}]")
