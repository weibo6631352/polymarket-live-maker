#!/usr/bin/env python3
"""Touch/reach ("What price will <coin> hit") curation — OPTIMAL config from the 07-08/09 research
(memory: touch-reach-markets). SEPARATE leg from the terminal vendor; env-gated OFF by default.

Edge lives ONLY in: LATE entry (<=3d to window end) + DEEP OTM (per-coin touch model <=3%) + 2-10c band
+ premium>model_fair. Rally survival rests on a TIGHT regime gate (vol>65% BTC/ETH, >55% SOL/XRP, mom>40%).
IRON RULE (2021-proven): deep band only — the near band (+8-10%) blows up (20-24% touch in a mania).

Design: layered, single-responsibility, low-coupling —
  config(TouchConfig) · model(data-driven JSON) · market feed(one pull) · gate(reuses regime.py) ·
  policy(pure evaluate()) · thin orchestrator(touch_rows).
  Extend a coin = add to ALL of: touch_model.json + SYMBOL + TouchConfig.vol_th + regime.SYMS (all four, or the
  gate fails closed / raises for that coin).

Dry:    python3 touch_curate.py
Import: from touch_curate import touch_rows   (make_whitelist merges when TV_TOUCH=1)
"""
import collections, json, os, re, time, urllib.request, urllib.parse
from dataclasses import dataclass, field
from datetime import datetime, timezone

try:
    import regime                       # optional: reuse its vol/mom fetch (same dir)
except Exception:
    regime = None

HERE = os.path.dirname(os.path.abspath(__file__))
GAMMA = "https://gamma-api.polymarket.com"
UA = {"User-Agent": "tv-touch/1", "Accept": "application/json"}
SYMBOL = {"BTC": "BTCUSDT", "ETH": "ETHUSDT", "SOL": "SOLUSDT", "XRP": "XRPUSDT"}
REACH_WORD = {"BTC": "bitcoin", "ETH": "ethereum", "SOL": "solana", "XRP": "xrp"}


# ── layer: config (all tunables in one place; inject a variant to experiment) ──────────────
@dataclass(frozen=True)
class TouchConfig:
    band: tuple = (0.02, 0.10)          # 2-10c premium band — MATCHES validated touch_optimal.py OOS (0.02<=p<0.10)
    max_days: float = 3.0               # LATE entry only (edge = surviving residual)
    min_moneyness: float = 0.15         # IRON RULE (2021 stress): hard deep-band floor +15% OTM. touch_optimal
                                        # used only model_touch<=3%, but that's regime-dependent — in calm markets
                                        # +8-10% (the NEAR band that hit 20-24% in 2021) passes <=3% and would be
                                        # sold. This structural floor makes "deep band only" unconditional.
    model_touch_cap: float = 0.03       # DEEP-OTM tilt: only sell where model touch <= 3% (recent-calib refinement)
    edge_margin: float = 0.010          # sell only if premium > model_fair + 1c
    mom_th: float = 0.40                # 20d momentum mania trip
    tick: float = 0.001
    base_coll: float = 20.0             # risk-parity base $; coll = base*(0.02/touch), cap size_cap_x*base
    size_cap_x: float = 2.0             # sizing cap (deepest/safest gets at most 2x base) — touch_optimal
    percoin_week_cap: float = 60.0      # max $ collateral per coin per ISO-week — touch_optimal
    vol_th: dict = field(default_factory=lambda: {"BTC": 0.65, "ETH": 0.65, "SOL": 0.55, "XRP": 0.55})

CFG = TouchConfig()


# ── layer: touch model (data-driven; regenerate touch_model.json, never edit code) ─────────
_EMBEDDED_MODEL = {   # fallback if touch_model.json missing
    "BTC": [[-1, .02, .68], [.02, .06, .05], [.06, .10, .006], [.10, .15, .005], [.15, 9, .005]],
    "ETH": [[-1, .02, .96], [.02, .06, .17], [.06, .10, .01], [.10, .15, .006], [.15, 9, .005]],
    "SOL": [[-1, .02, 1.0], [.02, .06, .12], [.06, .10, .006], [.10, .15, .005], [.15, 9, .005]],
    "XRP": [[-1, .02, .88], [.02, .06, .17], [.06, .10, .07], [.10, .15, .02], [.15, 9, .006]],
}

def load_model(path=os.path.join(HERE, "touch_model.json")):
    try:
        raw = json.load(open(path))
        return {k: v for k, v in raw.items() if not k.startswith("_")}
    except Exception:
        return _EMBEDDED_MODEL

def model_touch(model, coin, moneyness):
    """Empirical realized touch rate for (coin, moneyness). Unknown/ITM -> 1.0 (never sell). Pure."""
    for lo, hi, rate in model.get(coin, []):
        if lo <= moneyness < hi:
            return rate
    return 1.0


# ── layer: market data source (ONE gamma pull; normalized rows) ────────────────────────────
def _get(path, params):
    try:
        u = f"{GAMMA}{path}?" + urllib.parse.urlencode(params)
        with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=25) as r:
            return json.loads(r.read().decode())
    except Exception:
        return None

def spot(coin):
    try:
        u = f"https://data-api.binance.vision/api/v3/ticker/price?symbol={SYMBOL[coin]}"
        with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=15) as r:
            return float(json.loads(r.read().decode())["price"])
    except Exception:
        return None

def fetch_reach_markets(coins):
    """Pull crypto events ONCE; return normalized, slug-deduped reach markets: {coin,slug,K,T,ask,vol24}."""
    word2coin = {REACH_WORD[c]: c for c in coins}
    evs = _get("/events", dict(tag_slug="crypto", active="true", closed="false", limit=200)) or []
    out, seen = [], set()
    for ev in evs:
        blob = (ev.get("title", "") + " " + ev.get("slug", "")).lower()
        coin = next((c for w, c in word2coin.items() if w in blob), None)
        if not coin or "hit" not in blob:
            continue
        for m in ev.get("markets", []):
            slug = m.get("slug")
            if not slug or slug in seen:
                continue                                 # de-dup: same market can appear under >1 event grouping
            q = (m.get("question") or "")
            if "reach" not in q.lower():
                continue
            # strike: $, thousands-commas, decimals (XRP $3.50), and K/M suffix — plain \$([\d,]+) truncated decimals
            km = re.search(r"\$\s*([\d,]+(?:\.\d+)?)\s*([KkMm])?", q)
            if not km:
                continue
            try:
                ask = float(m.get("bestAsk"))
                T = datetime.fromisoformat((m.get("endDate") or "").replace("Z", "+00:00")).timestamp()
            except Exception:
                continue
            strike = float(km.group(1).replace(",", "")) * {"k": 1e3, "m": 1e6}.get((km.group(2) or "").lower(), 1.0)
            seen.add(slug)
            out.append(dict(coin=coin, slug=slug, K=strike, T=T, ask=ask,
                            vol24=round(float(m.get("volume24hr") or 0))))
    return out


# ── layer: regime gate (reuse regime.py's fetch; apply TOUCH thresholds) ───────────────────
def gate_states(coins):
    if regime is None:
        return {}
    try:
        return regime.coin_gate(coins)
    except Exception:
        return {}

def gate_open(coin, states, cfg=CFG):
    """True if this coin may vend. FAIL-CLOSED: missing regime info, data error, or a coin with no
    configured vol threshold all -> paused (never vend ungated). Pure."""
    s = states.get(coin)
    if s is None or s.get("error"):
        return False                                 # no regime info / data outage -> pause (fail-closed)
    th = cfg.vol_th.get(coin)
    if th is None:
        return False                                 # coin has no touch vol threshold configured -> pause (add it to vol_th)
    v = s.get("vol")
    return not (v is not None and (v > th or (s.get("mom") or 0) > cfg.mom_th))


# ── layer: selection policy (pure: market + spot + model + cfg -> row | None) ──────────────
def evaluate(mkt, sp, model, cfg=CFG):
    coin, K, ask = mkt["coin"], mkt["K"], mkt["ask"]
    if not sp or K <= sp:
        return None                                  # up-touch, OTM only
    days = (mkt["T"] - time.time()) / 86400.0
    if not (0 < days <= cfg.max_days):
        return None                                  # LATE entry only
    if not (cfg.band[0] <= ask <= cfg.band[1]):
        return None                                  # 2-10c band
    mny = K / sp - 1.0
    if mny < cfg.min_moneyness:
        return None                                  # IRON RULE: deep band only (never the near band)
    mt = model_touch(model, coin, mny)
    # NOTE: with min_moneyness=0.15 and the current calibration, only the deep [.15,9] bucket (touch<=0.006)
    # is reachable, so these two guards do NOT bind today — they are secondary defenses that fire only if a
    # future recalibration lifts the deep-bucket touch >3% (model_touch_cap) or premiums go thin (edge_margin).
    if mt > cfg.model_touch_cap:
        return None                                  # DEEP-OTM tilt (secondary; min_moneyness is the binding floor)
    sell = max(0.02, round(ask - cfg.tick, 3))
    edge = sell - mt
    if edge < cfg.edge_margin:
        return None                                  # sell only model-overpriced (secondary; see NOTE)
    # risk-parity size: coll = base*(0.02/touch), capped at size_cap_x*base (deeper/safer -> more). touch_optimal.
    # ⚠ INTEGRATION: the whitelist->bot channel currently carries only slugs (no per-row size); the bot must be
    # taught to honor `collateral`/`shares` before TV_TOUCH goes live, else it applies default sizing and oversizes.
    coll = round(min(cfg.base_coll * (0.02 / max(mt, 0.005)), cfg.size_cap_x * cfg.base_coll), 1)
    return dict(slug=mkt["slug"], coin=coin, ask=ask, sell=sell, fair_hi=round(mt, 4),
                edge=round(edge, 4), anchor="touch-model", days=round(days, 2), vol24=mkt["vol24"],
                moneyness=round(mny, 3), clamp=False, T=mkt["T"], collateral=coll,
                shares=round(coll / (1 - sell)),                   # BUY-NO shares the bot places (coll/(1-sell))
                score=round(edge / max(mt, 0.005), 4))


# ── orchestrator (thin) ────────────────────────────────────────────────────────────────────
def _iso_week(T):
    d = datetime.fromtimestamp(T, timezone.utc).isocalendar()
    return (d[0], d[1])

def _apply_percoin_week_cap(rows, cfg):
    """Greedy best-edge-first fill of the per-coin ISO-week collateral cap ($60). touch_optimal analog
    (backtest fills chronologically; live takes best score first within the same cap)."""
    used = collections.defaultdict(float)
    out = []
    for r in sorted(rows, key=lambda r: -r["score"]):
        k = (r["coin"], _iso_week(r["T"]))
        if used[k] + r["collateral"] > cfg.percoin_week_cap:
            continue
        used[k] += r["collateral"]
        out.append(r)
    return out

def touch_rows(coins=("BTC", "ETH", "SOL", "XRP"), cfg=CFG, model=None):
    """Selected reach tails to vend. Returns None (NOT []) when the mania gate is unavailable or incomplete
    — a fail-closed 'keep last-good, do not flush' signal the caller MUST honor (matches make_whitelist's
    sys.exit on regime outage). [] means the gate is healthy but nothing qualifies right now."""
    model = model or load_model()
    if regime is None:
        return None                          # no mania gate available -> refuse (fail-closed)
    states = gate_states(coins)
    if not states or any(c not in states or states[c].get("error") for c in coins):
        return None                          # regime outage / partial fetch -> keep last-good, never vend partial/ungated
    active = [c for c in coins if gate_open(c, states, cfg)]
    if not active:
        return []                            # all coins mania-paused -> genuinely nothing to vend
    markets = fetch_reach_markets(active)
    need = {m["coin"] for m in markets}      # only spot the coins that actually have candidate reach markets
    spots = {c: spot(c) for c in need}
    rows = [r for m in markets if (r := evaluate(m, spots.get(m["coin"]), model, cfg))]
    return _apply_percoin_week_cap(rows, cfg)


if __name__ == "__main__":
    st = gate_states(SYMBOL)
    print(f"=== touch regime gate (BTC/ETH>{100*CFG.vol_th['BTC']:.0f}%, SOL/XRP>{100*CFG.vol_th['SOL']:.0f}%, mom>{100*CFG.mom_th:.0f}%) ===")
    for c in SYMBOL:
        s = st.get(c, {}); v = f"{100*s['vol']:.0f}%" if s.get("vol") is not None else "?"
        print(f"  {c}: vol30={v} mom={100*(s.get('mom') or 0):+.0f}% -> {'active' if gate_open(c, st) else 'PAUSE'}")
    rows = touch_rows()
    if rows is None:
        print("\n=== FAIL-CLOSED: regime gate unavailable/incomplete -> vend nothing, keep last-good ===")
    elif not rows:
        print("\n=== 0 reach tails — gate healthy but no deep-OTM late-entry tail in band right now ===")
    else:
        print(f"\n=== reach tails the OPTIMAL config would vend NOW ({len(rows)}) ===")
        for r in sorted(rows, key=lambda r: -r["score"]):
            print(f"  {r['slug'][:44]:44} {r['coin']} {r['moneyness']*100:+.0f}%OTM ask={r['ask']:.3f} "
                  f"sell={r['sell']:.3f} mTouch={r['fair_hi']:.3f} edge={r['edge']:+.3f} d={r['days']:.1f} "
                  f"${r['collateral']:.0f}/{r['shares']}sh vol24={r['vol24']}")
        tot = sum(r["collateral"] for r in rows)
        print(f"  -- total collateral ${tot:.0f} across {len(rows)} (per-coin/week cap ${CFG.percoin_week_cap:.0f})")
