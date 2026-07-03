#!/usr/bin/env python3
"""Pull Deribit option chains (book summaries incl mark_iv) for BTC+ETH; cache."""
import json, os, time
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
D = "https://www.deribit.com/api/v2/public"
S = requests.Session()

def get(path, **params):
    r = S.get(f"{D}/{path}", params=params, timeout=30)
    r.raise_for_status()
    return r.json()["result"]

out = {"ts": time.time()}
for cur in ("BTC", "ETH"):
    summ = get("get_book_summary_by_currency", currency=cur, kind="option")
    inst = get("get_instruments", currency=cur, kind="option", expired="false")
    idx_usd = get("get_index_price", index_name=f"{cur.lower()}_usd")["index_price"]
    idx_usdt = get("get_index_price", index_name=f"{cur.lower()}_usdt")["index_price"]
    out[cur] = {"summary": summ, "instruments": inst,
                "index_usd": idx_usd, "index_usdt": idx_usdt}
    print(cur, len(summ), "options, index_usd", idx_usd, "index_usdt", idx_usdt)
    time.sleep(0.3)

path = os.path.join(HERE, "deribit_chain.json")
with open(path, "w") as f:
    json.dump(out, f)
print(path)
