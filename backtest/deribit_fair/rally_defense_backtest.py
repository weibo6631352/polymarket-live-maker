#!/usr/bin/env python3
"""Rally-defense backtest for the tail-vendor short.

Question (user, 2026-07-12): our only rally defense is the LAGGING regime gate (30d vol>100% / 20d
mom>+50%) = L3, too late. Should we add an ACTIVE 'trim/exit filled positions when leading danger
signals fire' (L2)? Trimming a cold-book tail short costs the given-up premium + spread on false alarms
(whipsaw). This backtest measures HOLD-to-resolution vs DEFENSE(exit-on-signal) over real BTC/ETH paths,
split by regime (the 2021 blow-up vs calmer years), so we only automate trimming if it saves more than
it bleeds.

Model: every day t0 we sell a ~p0 tail (strike K = S*exp(z*sigma*sqrt(T)) with z=N^-1(1-p0)), hold T days.
Tail value along the path = physical lognormal P(S_T>K | S_t, 30d realized vol, T-t left), zero drift
(conservative for an UP-tail short — assuming no drift understates rally risk slightly, which is fine: we
want the defense to prove itself even without help). HOLD pnl/$1: +p0 if S_T<=K else -(1-p0). DEFENSE:
if a danger signal fires on day t, close the short at p_t (pnl = p0 - p_t), else hold to T.

Danger signals (L2 = the expensive-action tier): spot within DIST of strike, or 3d>MOM3, or 7d>MOM7.

  python3 rally_defense_backtest.py            # BTC+ETH, default params
"""
import urllib.request, json, math, statistics, sys
from datetime import datetime, timezone

Z_TAIL = 0.05          # entry tail probability (~5c premium, our band center)
T_DAYS = 3             # holding horizon (our d=1-6, ~3d typical)
VOL_WIN = 30           # realized-vol window (days)
DIST_TH = 0.03         # danger: spot within 3% of strike
MOM3_TH = 0.10         # danger: 3d return > +10%
MOM7_TH = 0.15         # danger: 7d return > +15%
SPREAD = 0.01          # exit costs 1c of spread (cold book) — charged on every DEFENSE exit

def norm_cdf(x): return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))
def norm_ppf(p):
    # Acklam approximation
    a=[-3.969683028665376e+01,2.209460984245205e+02,-2.759285104469687e+02,1.383577518672690e+02,-3.066479806614716e+01,2.506628277459239e+00]
    b=[-5.447609879822406e+01,1.615858368580409e+02,-1.556989798598866e+02,6.680131188771972e+01,-1.328068155288572e+01]
    c=[-7.784894002430293e-03,-3.223964580411365e-01,-2.400758277161838e+00,-2.549732539343734e+00,4.374664141464968e+00,2.938163982698783e+00]
    d=[7.784695709041462e-03,3.224671290700398e-01,2.445134137142996e+00,3.754408661907416e+00]
    pl=0.02425
    if p<pl:
        q=math.sqrt(-2*math.log(p)); return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5])/((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if p<=1-pl:
        q=p-0.5; r=q*q; return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q/(((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)
    q=math.sqrt(-2*math.log(1-p)); return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5])/((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)

def tail_prob(S, K, sigma_ann, days_left):
    """Physical P(S_T > K), lognormal, zero drift."""
    if days_left <= 0: return 1.0 if S >= K else 0.0
    T = days_left/365.0
    vt = sigma_ann*math.sqrt(T)
    if vt <= 1e-9: return 1.0 if S >= K else 0.0
    d2 = (math.log(S/K) - 0.5*vt*vt)/vt
    return norm_cdf(d2)   # P(S_T > K), lognormal zero-drift

def klines(sym, limit=1000, end=None):
    url=f'https://api.binance.com/api/v3/klines?symbol={sym}&interval=1d&limit={limit}'
    if end: url+=f'&endTime={end}'
    req=urllib.request.Request(url, headers={'User-Agent':'Mozilla/5.0'})
    return json.load(urllib.request.urlopen(req, timeout=20))

def full_history(sym):
    """Page back to get ~2020-2026 daily closes."""
    out=[]; end=None
    for _ in range(6):
        k=klines(sym, 1000, end)
        if not k: break
        out = k + out
        end = k[0][0]-1
        if len(k)<1000: break
    # dedup + sort by open time
    seen={};
    for r in out: seen[r[0]]=r
    rows=[seen[t] for t in sorted(seen)]
    return rows

def run(sym, label):
    rows=full_history(sym)
    ts=[datetime.fromtimestamp(r[0]/1000, timezone.utc) for r in rows]
    close=[float(r[4]) for r in rows]; high=[float(r[2]) for r in rows]
    logret=[math.log(close[i]/close[i-1]) for i in range(1,len(close))]
    n=len(close)
    z=norm_ppf(1-Z_TAIL)
    def rvol(i):  # annualized realized vol using prior VOL_WIN days ending at i
        seg=logret[max(0,i-VOL_WIN):i]
        if len(seg)<5: return None
        return statistics.pstdev(seg)*math.sqrt(365)
    def ret(i,n_):  # n_-day return ending at i
        j=i-n_
        return close[i]/close[j]-1 if j>=0 else None
    buckets={'2021':[], 'other':[]}   # (hold_pnl, defense_pnl, exited, entry_gated)
    def rvol_long(i):
        seg=logret[max(0,i-60):i]
        return statistics.pstdev(seg)*math.sqrt(365) if len(seg)>=20 else None
    for t0 in range(VOL_WIN+7, n-T_DAYS):
        sigma=rvol(t0)
        if sigma is None or sigma<=0: continue
        S0=close[t0]
        K=S0*math.exp(z*sigma*math.sqrt(T_DAYS/365.0))
        p0=tail_prob(S0,K,sigma,T_DAYS)   # ~Z_TAIL
        # HOLD outcome
        ST=close[t0+T_DAYS]
        hold = p0 if ST<=K else -(1.0-p0)
        # DEFENSE: walk the path, exit on first danger signal
        exited=False; defense=hold
        for t in range(t0+1, t0+T_DAYS+1):
            St=close[t]
            dist=K/St-1.0
            r3=ret(t,3); r7=ret(t,7)
            danger = (dist<DIST_TH) or (r3 is not None and r3>MOM3_TH) or (r7 is not None and r7>MOM7_TH)
            if danger:
                sig=rvol(t) or sigma
                pt=tail_prob(St,K,sig,t0+T_DAYS-t)
                defense = p0 - pt - SPREAD   # close short at pt, pay spread
                exited=True
                break
        # ENTRY-side gate: would we AVOID selling this tail? (leading danger already elevated at entry)
        r7e=ret(t0,7); r3e=ret(t0,3); vl=rvol_long(t0)
        entry_gated = (r7e is not None and r7e>0.10) or (r3e is not None and r3e>0.07) or \
                      (vl is not None and vl>0 and sigma/vl>1.5)
        yr='2021' if ts[t0].year==2021 else 'other'
        buckets[yr].append((hold,defense,exited,entry_gated))
    def stats(rowset, name):
        if not rowset: print(f'  [{name}] no data'); return
        h=[r[0] for r in rowset]; d=[r[1] for r in rowset]
        ex=sum(1 for r in rowset if r[2])
        # cumulative + max drawdown
        def maxdd(series):
            cum=0.0; peak=0.0; mdd=0.0
            for x in series:
                cum+=x; peak=max(peak,cum); mdd=min(mdd,cum-peak)
            return mdd
        print(f'  [{name}] n={len(rowset)}  exit_rate={ex/len(rowset)*100:.0f}%')
        print(f'      HOLD(no defense) mean/pos={statistics.mean(h)*100:+.2f}c  total={sum(h):+.1f}  worst={min(h)*100:.0f}c  maxDD={maxdd(h)*100:.0f}c')
        print(f'      TRIM-on-signal   mean/pos={statistics.mean(d)*100:+.2f}c  total={sum(d):+.1f}  worst={min(d)*100:.0f}c  maxDD={maxdd(d)*100:.0f}c   Δ={sum(d)-sum(h):+.1f}')
        # ENTRY-GATE: keep only non-gated entries
        kept=[r[0] for r in rowset if not r[3]]; skip=[r[0] for r in rowset if r[3]]
        gr=int(len(skip)/len(rowset)*100)
        if kept:
            print(f'      ENTRY-GATE       kept={len(kept)}({100-gr}%) mean/pos={statistics.mean(kept)*100:+.2f}c total={sum(kept):+.1f} maxDD={maxdd(kept)*100:.0f}c  | SKIPPED {len(skip)}({gr}%) mean={statistics.mean(skip)*100 if skip else 0:+.2f}c total={sum(skip):+.1f}')
    print(f'\n=== {label}  (days {ts[0].date()}..{ts[-1].date()}, {n} bars; entry tail p0≈{Z_TAIL:.0%}, T={T_DAYS}d) ===')
    stats(buckets['2021'],'2021 blow-up regime')
    stats(buckets['other'],'other years (calm)')
    stats(buckets['2021']+buckets['other'],'ALL')

for sym,label in [('BTCUSDT','BTC'),('ETHUSDT','ETH')]:
    try: run(sym,label)
    except Exception as e:
        import traceback; print(label,'ERR',e); traceback.print_exc()
