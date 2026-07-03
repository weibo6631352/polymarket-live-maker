#!/usr/bin/env python3
"""Shared helpers for the Polymarket daily-temperature pipeline (backtest/weather/).

READ-ONLY public APIs, zero orders, zero keys. Stdlib only (urllib), matching repo style.

What lives here:
  - cached/gentle HTTP (get_json / get_text / post_json) with on-disk cache under cache/http/
  - gamma discovery of a date's temperature cohort (tag_id=104596 "highest-temperature",
    paged by END-DATE WINDOW, not by raw offset — gamma offset caps are real)
  - parse_resolution_source(): the station that RESOLVES a market, parsed PER MARKET from the
    gamma description field. Never hardcode a guessed station — 44 cities resolve on
    Wunderground daily history at a named ICAO that is often NOT the obvious airport
    (NYC=KLGA, Denver=KBKF Buckley SFB, Paris=LFPB Le Bourget, London=EGLC City Airport,
    Panama City=MPMG); 4 cities resolve on NOAA weather.gov/wrh/timeseries (e.g. LTFM
    Istanbul, UUWW Moscow-Vnukovo, LLBG Tel Aviv); Hong Kong resolves on the HK
    Observatory "Absolute Daily Max" (0.1 degC precision -> FLOOR bucket semantics).
  - bucket parsing + bucket probability/membership semantics:
      US:   2-degF buckets ("98-99[F]"), tails "or below"/"or higher", source shows WHOLE degF
            -> displayed = round(T_F), bucket [lo,hi] == interval [lo-0.5, hi+0.5)
      intl: 1-degC buckets ("26C"), source shows WHOLE degC -> [lo-0.5, hi+0.5) in degC
      HK:   HKO reports 0.1 degC -> FLOOR semantics, bucket [lo,hi] == [lo, hi+1)
  - IEM (Iowa Environmental Mesonet) station metadata (lat/lon/tz) via /api/1/station/{id}
  - Brier / log-loss / normal CDF utilities

Verified live 2026-07-03 against gamma, CLOB, IEM, open-meteo.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BASE = Path(__file__).resolve().parent
CACHE = BASE / "cache"

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
IEM = "https://mesonet.agron.iastate.edu"
# NOTE: api.open-meteo.com itself was unreachable from some networks during the original
# build; the ensemble + historical-forecast hosts below worked and are the ones we use.
OM_ENSEMBLE = "https://ensemble-api.open-meteo.com"
OM_HISTFC = "https://historical-forecast-api.open-meteo.com"

TEMP_TAG_ID = 104596  # gamma tag "highest-temperature" (verified 2026-07-03)
UA = "Mozilla/5.0 (weather-backtest; read-only research)"

_last_fetch = [0.0]
_MIN_GAP_S = 0.15  # be gentle with public APIs


# --------------------------------------------------------------------------- HTTP + cache
def _throttle() -> None:
    dt = time.time() - _last_fetch[0]
    if dt < _MIN_GAP_S:
        time.sleep(_MIN_GAP_S - dt)
    _last_fetch[0] = time.time()


def _cache_path(url: str, cache_key: str | None, suffix: str) -> Path:
    key = cache_key or hashlib.sha1(url.encode()).hexdigest()[:24]
    p = CACHE / "http" / f"{key}{suffix}"
    p.parent.mkdir(parents=True, exist_ok=True)
    return p


def _fetch(url: str, tries: int = 3, pause: float = 0.7, body: bytes | None = None) -> str:
    last: Exception | None = None
    for i in range(tries):
        try:
            _throttle()
            req = urllib.request.Request(
                url,
                data=body,
                headers={"User-Agent": UA, "Accept": "application/json",
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=40) as r:
                return r.read().decode()
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(pause * (i + 1))
    raise RuntimeError(f"GET failed: {url} :: {last}")


def get_text(url: str, cache_key: str | None = None, ttl_s: float | None = None) -> str:
    """GET with optional on-disk cache (ttl_s=None -> cache forever once fetched)."""
    p = _cache_path(url, cache_key, ".txt")
    if p.exists() and (ttl_s is None or time.time() - p.stat().st_mtime < ttl_s):
        return p.read_text()
    txt = _fetch(url)
    p.write_text(txt)
    return txt


def get_json(url: str, cache_key: str | None = None, ttl_s: float | None = None):
    p = _cache_path(url, cache_key, ".json")
    if p.exists() and (ttl_s is None or time.time() - p.stat().st_mtime < ttl_s):
        return json.loads(p.read_text())
    txt = _fetch(url)
    d = json.loads(txt)
    p.write_text(txt)
    return d


def post_json(url: str, payload) -> object:
    """Uncached POST (used for CLOB batch /books and /midpoints)."""
    return json.loads(_fetch(url, body=json.dumps(payload).encode()))


# --------------------------------------------------------------------------- gamma cohort
def gamma_temp_events(date_iso: str) -> list[dict]:
    """All 'highest-temperature' events whose endDate falls on date_iso (YYYY-MM-DD).

    Temperature events carry endDate = <target date>T12:00:00Z, so an end-date window
    of the target day returns exactly that day's cohort (49 cities as of 2026-07).
    Paged defensively even though one page of 100 currently suffices.
    """
    out, offset = [], 0
    while True:
        q = urllib.parse.urlencode({
            "tag_id": TEMP_TAG_ID,
            "end_date_min": f"{date_iso}T00:00:00Z",
            "end_date_max": f"{date_iso}T23:59:59Z",
            "limit": 100,
            "offset": offset,
            "order": "slug",
            "ascending": "true",
        })
        batch = get_json(f"{GAMMA}/events?{q}", cache_key=f"gamma_temp_{date_iso}_{offset}",
                         ttl_s=900)
        out += [e for e in batch if e.get("slug", "").startswith("highest-temperature-in-")]
        if len(batch) < 100:
            return out
        offset += 100


def city_from_slug(event_slug: str) -> str:
    """'highest-temperature-in-hong-kong-on-july-3-2026' -> 'hong-kong'."""
    m = re.match(r"highest-temperature-in-(.+)-on-[a-z]+-\d+-\d{4}$", event_slug)
    return m.group(1) if m else event_slug


# --------------------------------------------------------------- resolution source parsing
def parse_resolution_source(description: str) -> dict:
    """Parse WHERE a market resolves from its gamma description. Returns
    {kind, icao, unit ('F'|'C'), semantics ('round'|'floor')}.

    kinds: 'wunderground' (44 cities, named ICAO in the URL path),
           'nws_timeseries' (NOAA weather.gov/wrh/timeseries?site=XXXX),
           'hko' (Hong Kong Observatory HQ; NOT an airport — we use VHHH METAR as a
                  ground-truth PROXY and HKO HQ coordinates for forecasts; see README).
    semantics: 'floor' when the source publishes 0.1-degree precision (HKO
               "Absolute Daily Max (deg. C)": 31.4 -> the 31C bucket), else 'round'
               (whole-degree sources: displayed value = half-up rounding).
    """
    d = description or ""
    unit = "F" if "degrees Fahrenheit" in d else "C"
    semantics = "floor" if "one decimal place" in d else "round"

    m = re.search(r"wunderground\.com/history/daily/(?:[a-z0-9\-]+/)+([A-Z][A-Za-z0-9]{3})", d)
    if m:
        return {"kind": "wunderground", "icao": m.group(1).upper(), "unit": unit,
                "semantics": semantics}
    m = re.search(r"weather\.gov/wrh/timeseries\?(?:site|sid)=([A-Za-z0-9]{4})", d)
    if m:
        return {"kind": "nws_timeseries", "icao": m.group(1).upper(), "unit": unit,
                "semantics": semantics}
    if "weather.gov.hk" in d:
        return {"kind": "hko", "icao": "VHHH", "unit": "C", "semantics": "floor"}
    raise ValueError(f"unrecognized resolution source in description: {d[:200]!r}")


# --------------------------------------------------------------------------- station meta
# HKO resolution station is Observatory HQ (Tsim Sha Tsui), not the VHHH airport grid point.
_META_OVERRIDES = {
    "hko": {"lat": 22.302, "lon": 114.174, "tz": "Asia/Hong_Kong",
            "name": "Hong Kong Observatory HQ (VHHH METAR used as outcome proxy)"},
}


def iem_station_id(icao: str) -> str:
    """IEM drops the leading K of US ICAOs (KLGA -> LGA); international IDs unchanged."""
    return icao[1:] if len(icao) == 4 and icao.startswith("K") else icao


def station_meta(icao: str, kind: str = "") -> dict:
    """lat/lon/tz for a station: IEM /api/1/station/{id}.json (cached), with HKO override."""
    if kind in _META_OVERRIDES:
        return dict(_META_OVERRIDES[kind], icao=icao)
    sid = iem_station_id(icao)
    d = get_json(f"{IEM}/api/1/station/{sid}.json", cache_key=f"iem_station_{sid}")
    row = d["data"][0]
    return {"icao": icao, "lat": row["latitude"], "lon": row["longitude"],
            "tz": row["tzname"], "name": row["name"]}


# --------------------------------------------------------------------------- buckets
_PATS = [
    (re.compile(r"(\d+)°([FC]) or below"), lambda m: (None, int(m.group(1)), m.group(2))),
    (re.compile(r"(\d+)°([FC]) or higher"), lambda m: (int(m.group(1)), None, m.group(2))),
    (re.compile(r"between (\d+)-(\d+)°([FC])"),
     lambda m: (int(m.group(1)), int(m.group(2)), m.group(3))),
    (re.compile(r"be (\d+)°([FC])"), lambda m: (int(m.group(1)), int(m.group(1)), m.group(2))),
]


def parse_bucket(question: str) -> dict:
    """Bucket from the market question. Returns {lo, hi, unit}; lo/hi None = open tail.

    US questions:   'be between 98-99°F', tails 'be 97°F or below' / 'be 116°F or higher'
    intl questions: 'be 26°C', tails 'be 25°C or below' / 'be 35°C or higher'
    """
    for pat, mk in _PATS:
        m = pat.search(question)
        if m:
            lo, hi, unit = mk(m)
            return {"lo": lo, "hi": hi, "unit": unit}
    raise ValueError(f"cannot parse bucket from question: {question!r}")


def bucket_interval(lo, hi, semantics: str) -> tuple[float | None, float | None]:
    """Bucket label -> half-open interval [a, b) on the CONTINUOUS max, in the native unit.

    'round' (whole-degree sources, displayed = half-up round): [lo-0.5, hi+0.5)
    'floor' (HKO 0.1 degC): [lo, hi+1)
    Open tails keep a/b = None (-inf/+inf).
    """
    if semantics == "floor":
        a = None if lo is None else float(lo)
        b = None if hi is None else float(hi) + 1.0
    else:
        a = None if lo is None else lo - 0.5
        b = None if hi is None else hi + 0.5
    return a, b


def bucket_contains(lo, hi, semantics: str, x_native: float) -> bool:
    a, b = bucket_interval(lo, hi, semantics)
    return (a is None or x_native >= a) and (b is None or x_native < b)


# --------------------------------------------------------------------------- math / units
def f_to_c(f: float) -> float:
    return (f - 32.0) * 5.0 / 9.0


def c_to_f(c: float) -> float:
    return c * 9.0 / 5.0 + 32.0


def normal_cdf(x: float, mu: float = 0.0, sd: float = 1.0) -> float:
    return 0.5 * (1.0 + math.erf((x - mu) / (sd * math.sqrt(2.0))))


def interval_prob_normal(a, b, mu: float, sd: float) -> float:
    """P(a <= X < b) for X ~ N(mu, sd); a/b None = open ends."""
    hi = 1.0 if b is None else normal_cdf(b, mu, sd)
    lo = 0.0 if a is None else normal_cdf(a, mu, sd)
    return max(0.0, hi - lo)


def brier(probs: list[float], win_idx: int) -> float:
    """Mean per-bucket squared error over the full outcome vector (the decisive metric)."""
    return sum((p - (1.0 if i == win_idx else 0.0)) ** 2 for i, p in enumerate(probs)) / len(probs)


def log_loss_winner(probs: list[float], win_idx: int, eps: float = 1e-4) -> float:
    return -math.log(max(probs[win_idx], eps))


def normalize(probs: list[float]) -> list[float]:
    s = sum(probs)
    return [p / s for p in probs] if s > 0 else probs
