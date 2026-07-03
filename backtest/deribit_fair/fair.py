#!/usr/bin/env python3
"""Risk-neutral fair values from cached Deribit chains.

Terminal digitals: skew-adjusted Black-76 digital, P(S_T > K) = N(d2) - F phi(d1) sqrt(T) dsigma/dK,
with mark-IV smile interpolated linearly in log-strike per expiry and total variance
linear in time between bracketing expiries. Forward log-interpolated in time.

Touch (one-touch by T): reflection-principle barrier-hit probability under GBM with
risk-neutral log-drift m = (ln(F_T/S0) - w/2)/T and vol taken from the smile AT the
barrier level (practitioner approximation; exact one-touch under skew would need a
local/stoch-vol model — error is second order for the accuracy needed here).

Strikes are quoted by PM against Binance USDT pairs; Deribit index is USD. We rescale
PM strikes by (index_usdt/index_usd) before lookup.
"""
import json, math, os, bisect
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SQ2 = math.sqrt(2.0)

def N(x):  return 0.5 * math.erfc(-x / SQ2)
def phi(x): return math.exp(-0.5 * x * x) / math.sqrt(2 * math.pi)

class Chain:
    def __init__(self, path=os.path.join(HERE, "deribit_chain.json")):
        raw = json.load(open(path))
        self.ts = raw["ts"]
        self.cur = {}
        for cur in ("BTC", "ETH"):
            d = raw[cur]
            smiles = defaultdict(dict)   # expiry_ts(s) -> {K: iv}
            fwd = {}                     # expiry_ts -> forward
            exp_by_name = {i["instrument_name"]: i for i in d["instruments"]}
            for s in d["summary"]:
                name = s["instrument_name"]
                inst = exp_by_name.get(name)
                if not inst:
                    continue
                et = inst["expiration_timestamp"] / 1000.0
                K = inst["strike"]
                iv = s.get("mark_iv")
                if iv is None or iv <= 0:
                    continue
                # average C and P mark IVs when both present
                prev = smiles[et].get(K)
                smiles[et][K] = (prev + iv / 100.0) / 2 if prev else iv / 100.0
                if s.get("underlying_price"):
                    fwd[et] = s["underlying_price"]
            self.cur[cur] = {
                "expiries": sorted(smiles.keys()),
                "smiles": {et: sorted(ks.items()) for et, ks in smiles.items()},
                "fwd": fwd,
                "spot_usd": d["index_usd"],
                "usdt_ratio": d["index_usdt"] / d["index_usd"],
            }

    # ---- smile interpolation ----
    def _iv_at_expiry(self, cur, et, K):
        """linear in log-strike on mark IVs; flat extrapolation."""
        pts = self.cur[cur]["smiles"][et]
        ks = [p[0] for p in pts]
        i = bisect.bisect_left(ks, K)
        if i == 0:
            return pts[0][1]
        if i >= len(pts):
            return pts[-1][1]
        k0, v0 = pts[i - 1]
        k1, v1 = pts[i]
        x = (math.log(K) - math.log(k0)) / (math.log(k1) - math.log(k0))
        return v0 + x * (v1 - v0)

    def _bracket(self, cur, T_ts):
        exps = self.cur[cur]["expiries"]
        i = bisect.bisect_left(exps, T_ts)
        lo = exps[i - 1] if i > 0 else None
        hi = exps[i] if i < len(exps) else None
        return lo, hi

    def var_at(self, cur, K_usd, T_ts, now_ts):
        """total variance w = sigma^2 * tau at strike K_usd for target time T_ts."""
        tau = max(T_ts - now_ts, 60.0) / (365.25 * 86400)
        lo, hi = self._bracket(cur, T_ts)
        def w_of(et):
            t = max(et - now_ts, 60.0) / (365.25 * 86400)
            iv = self._iv_at_expiry(cur, et, K_usd)
            return iv * iv * t, t
        if lo is None:            # before first expiry: scale first smile's variance
            w1, t1 = w_of(hi)
            return w1 * tau / t1, tau
        if hi is None:            # beyond last expiry: flat IV extrapolation
            w0, t0 = w_of(lo)
            return w0 * tau / t0, tau
        w0, t0 = w_of(lo)
        w1, t1 = w_of(hi)
        x = (tau - t0) / (t1 - t0) if t1 > t0 else 0.0
        return w0 + x * (w1 - w0), tau

    def fwd_at(self, cur, T_ts, now_ts):
        c = self.cur[cur]
        S = c["spot_usd"]
        exps = [et for et in c["expiries"] if et in c["fwd"]]
        lo, hi = None, None
        for et in exps:
            if et <= T_ts: lo = et
            if et >= T_ts and hi is None: hi = et
        pts = [(now_ts, S)]
        if lo: pts.append((lo, c["fwd"][lo]))
        if hi and hi != lo: pts.append((hi, c["fwd"][hi]))
        pts.sort()
        # linear in ln F over time
        if T_ts <= pts[0][0]:
            return pts[0][1]
        for (t0, f0), (t1, f1) in zip(pts, pts[1:]):
            if t0 <= T_ts <= t1:
                x = (T_ts - t0) / (t1 - t0) if t1 > t0 else 0
                return math.exp(math.log(f0) + x * (math.log(f1) - math.log(f0)))
        # beyond last point: extrapolate last drift
        (t0, f0), (t1, f1) = pts[-2], pts[-1]
        r = (math.log(f1) - math.log(f0)) / (t1 - t0)
        return math.exp(math.log(f1) + r * (T_ts - t1))

    # ---- fair values ----
    def digital(self, cur, K_usdt, T_ts, now_ts):
        """P(S_T > K) with S the Binance USDT price; skew-adjusted."""
        c = self.cur[cur]
        K = K_usdt / c["usdt_ratio"]          # into Deribit-USD terms
        F = self.fwd_at(cur, T_ts, now_ts)
        w, tau = self.var_at(cur, K, T_ts, now_ts)
        sw = math.sqrt(w)
        d1 = (math.log(F / K) + 0.5 * w) / sw
        d2 = d1 - sw
        # skew slope dsigma/dK at (K, T)
        dK = K * 0.01
        w_up, _ = self.var_at(cur, K + dK, T_ts, now_ts)
        w_dn, _ = self.var_at(cur, K - dK, T_ts, now_ts)
        dsig_dK = (math.sqrt(w_up / tau) - math.sqrt(w_dn / tau)) / (2 * dK)
        vega = F * phi(d1) * math.sqrt(tau)   # undiscounted Black-76 vega
        p = N(d2) - vega * dsig_dK
        return min(1.0, max(0.0, p)), dict(F=F, K_usd=K, iv=sw / math.sqrt(tau),
                                           tau=tau, Nd2=N(d2), skew_adj=-vega * dsig_dK)

    def touch(self, cur, B_usdt, T_ts, now_ts, up=True, vol_ref="geo"):
        """P(hit barrier B before T), from now.
        vol_ref: which smile point drives the path vol —
          'barrier' (aggressive for skewed wings), 'atm', or 'geo' (sqrt(F*B),
          central estimate). Report barrier/atm as the uncertainty band."""
        c = self.cur[cur]
        B = B_usdt / c["usdt_ratio"]
        S = c["spot_usd"]
        if (up and S >= B) or ((not up) and S <= B):
            return 1.0, dict(note="barrier already beyond spot")
        F = self.fwd_at(cur, T_ts, now_ts)
        if vol_ref == "barrier":
            K_vol = B
        elif vol_ref == "atm":
            K_vol = F
        else:
            K_vol = math.sqrt(F * B)
        w, tau = self.var_at(cur, K_vol, T_ts, now_ts)
        sw = math.sqrt(w)
        sig2 = w / tau
        mT = math.log(F / S) - 0.5 * w        # total log-drift over horizon
        b = math.log(B / S)
        if up:
            p = N((mT - b) / sw) + math.exp(2 * mT * b / w) * N((-b - mT) / sw)
        else:
            p = N((b - mT) / sw) + math.exp(2 * mT * b / w) * N((b + mT) / sw)
        term = N(((mT if up else -mT) - abs(b)) / sw)  # terminal digital w/o skew (ref)
        return min(1.0, max(0.0, p)), dict(F=F, B_usd=B, iv=math.sqrt(sig2), tau=tau,
                                           terminal_ref=term)

    def touch_window(self, cur, B_usdt, t0_ts, t1_ts, now_ts, up=True, vol_ref="geo"):
        """P(touch B during [t0,t1]) when the window may start in the future.
        If t0<=now falls back to plain touch. Else integrates the plain-touch prob
        over the lognormal distribution of S_{t0} (flat vol both legs)."""
        if t0_ts <= now_ts:
            return self.touch(cur, B_usdt, t1_ts, now_ts, up=up, vol_ref=vol_ref)
        c = self.cur[cur]
        B = B_usdt / c["usdt_ratio"]
        S = c["spot_usd"]
        F0 = self.fwd_at(cur, t0_ts, now_ts)
        F1 = self.fwd_at(cur, t1_ts, now_ts)
        if vol_ref == "barrier":
            K_vol = B
        elif vol_ref == "atm":
            K_vol = F1
        else:
            K_vol = math.sqrt(F1 * B)
        w0, tau0 = self.var_at(cur, K_vol, t0_ts, now_ts)  # vol to window start
        w_tot, tau_tot = self.var_at(cur, K_vol, t1_ts, now_ts)
        w1 = max(w_tot - w0, 1e-8)
        drift1 = math.log(F1 / F0)                        # log-drift over window
        mT1 = drift1 - 0.5 * w1
        sw1 = math.sqrt(w1)
        # integrate over x = ln(S_t0/S): x ~ N(ln(F0/S) - w0/2, w0)
        mu0 = math.log(F0 / S) - 0.5 * w0
        sw0 = math.sqrt(w0)
        n, total = 400, 0.0
        for i in range(n):
            z = -5 + 10 * (i + 0.5) / n
            x = mu0 + sw0 * z
            wgt = phi(z) * (10 / n)
            s0 = S * math.exp(x)
            if (up and s0 >= B) or ((not up) and s0 <= B):
                total += wgt
                continue
            b = math.log(B / s0)
            if up:
                p = N((mT1 - b) / sw1) + math.exp(2 * mT1 * b / w1) * N((-b - mT1) / sw1)
            else:
                p = N((b - mT1) / sw1) + math.exp(2 * mT1 * b / w1) * N((b + mT1) / sw1)
            total += wgt * p
        return min(1.0, max(0.0, total)), dict(F0=F0, F1=F1, w0=w0, w1=w1, B_usd=B)

if __name__ == "__main__":
    import time
    ch = Chain()
    now = time.time()
    # smoke tests
    for cur in ("BTC", "ETH"):
        S = ch.cur[cur]["spot_usd"]
        print(cur, "spot", S, "usdt_ratio", round(ch.cur[cur]["usdt_ratio"], 5),
              "expiries", len(ch.cur[cur]["expiries"]))
        T = now + 7 * 86400
        for mult in (0.9, 1.0, 1.1):
            K = S * mult * ch.cur[cur]["usdt_ratio"]
            p, dbg = ch.digital(cur, K, T, now)
            pt, _ = ch.touch(cur, K, T, now, up=mult >= 1.0)
            print(f"  K={K:.0f} ({mult:.1f}x) 7d: digital={p:.4f} touch={pt:.4f} iv={dbg['iv']:.3f} skew_adj={dbg['skew_adj']:+.4f}")
