#!/usr/bin/env python3
"""Measure OUR latency from the box to the endpoints that matter for crypto price-latency scalping.
ZERO real money — pure network measurement, no orders, no trading, PM_TRADER_LIVE untouched.

Measures, from this host: DNS, TCP-connect RTT, TLS-handshake add, and HTTP round-trip (TTFB) to:
  - PM CLOB order/matching host (clob.polymarket.com) + data/gamma
  - Binance (stream.binance.com WS:9443, api.binance.com, data-api) — the crypto price source
  - Coinbase (ws-feed.exchange.coinbase.com, api.exchange.coinbase.com)
Plus a crypto-exchange server-time round-trip (reveals total signal latency). Reports min/median/p90.
The geography matters: this box is AWS eu-west-1 (Ireland); PM is Cloudflare-fronted (origin likely US),
Binance's matching is Tokyo (ap-northeast-1). Print resolved IPs to infer hosting.
Run on the box: python3 backtest/latency_probe.py
"""
import socket
import ssl
import time
import statistics
import sys


def pct(xs, p):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def resolve(host):
    try:
        _, _, ips = socket.gethostbyname_ex(host)
        return ips[:4]
    except Exception as e:  # noqa: BLE001
        return [f"resolve-fail:{e}"]


def tcp_rtt(host, port, n=12):
    out = []
    for _ in range(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        t0 = time.perf_counter()
        try:
            s.connect((host, port))
            out.append((time.perf_counter() - t0) * 1000)
        except Exception:  # noqa: BLE001
            pass
        finally:
            s.close()
        time.sleep(0.08)
    return out


def tls_rtt(host, port, n=6):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    out = []
    for _ in range(n):
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.settimeout(6)
        try:
            raw.connect((host, port))
            t0 = time.perf_counter()
            ss = ctx.wrap_socket(raw, server_hostname=host)
            out.append((time.perf_counter() - t0) * 1000)
            ss.close()
        except Exception:  # noqa: BLE001
            try:
                raw.close()
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.08)
    return out


def http_ttfb(host, port, path="/", n=8, host_header=None):
    """Full TLS GET, time to first response byte (reused-conn would be lower; this is cold-ish)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = (f"GET {path} HTTP/1.1\r\nHost: {host_header or host}\r\n"
           f"User-Agent: lat/1.0\r\nConnection: close\r\nAccept: */*\r\n\r\n").encode()
    out = []
    for _ in range(n):
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.settimeout(6)
        try:
            raw.connect((host, port))
            ss = ctx.wrap_socket(raw, server_hostname=host)
            t0 = time.perf_counter()
            ss.sendall(req)
            ss.recv(1)
            out.append((time.perf_counter() - t0) * 1000)
            ss.close()
        except Exception:  # noqa: BLE001
            try:
                raw.close()
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.1)
    return out


def report(name, host, port, do_tls=True, do_http=True, path="/"):
    ips = resolve(host)
    t = tcp_rtt(host, port)
    line = f"\n## {name}  {host}:{port}\n   IPs: {', '.join(ips)}"
    if t:
        line += f"\n   TCP-connect RTT  min={min(t):.1f}ms  median={statistics.median(t):.1f}ms  p90={pct(t,0.9):.1f}ms  (n={len(t)})"
    else:
        line += "\n   TCP-connect: FAILED (blocked/unreachable)"
    if do_tls and t:
        tl = tls_rtt(host, port)
        if tl:
            line += f"\n   TLS-handshake add median={statistics.median(tl):.1f}ms"
    if do_http and t:
        h = http_ttfb(host, port, path)
        if h:
            line += f"\n   HTTPS GET TTFB  min={min(h):.1f}ms  median={statistics.median(h):.1f}ms (full cold conn: TCP+TLS+req+1st-byte)"
    print(line, flush=True)
    return statistics.median(t) if t else None


def main():
    print("# latency probe from this host (read-only, no orders)")
    print(f"# local time epoch={int(time.time())}")
    report("PM CLOB (order/matching host)", "clob.polymarket.com", 443, path="/time")
    report("PM data-api", "data-api.polymarket.com", 443, path="/")
    report("PM CLOB WS", "ws-subscriptions-clob.polymarket.com", 443, do_http=False)
    report("Binance WS", "stream.binance.com", 9443, do_http=False)
    report("Binance API", "api.binance.com", 443, path="/api/v3/time")
    report("Binance data WS", "data-stream.binance.vision", 443, do_http=False)
    report("Coinbase WS", "ws-feed.exchange.coinbase.com", 443, do_http=False)
    report("Coinbase API", "api.exchange.coinbase.com", 443, path="/time")


if __name__ == "__main__":
    main()
