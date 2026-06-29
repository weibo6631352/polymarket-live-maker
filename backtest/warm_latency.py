#!/usr/bin/env python3
"""CORRECTED latency measurement: WARM (persistent keep-alive) PM order-path RTT + fast EU crypto feeds
+ AWS-region triangulation to locate PM's origin. ZERO real money — measurement only, no orders.

Fixes the prior error: http_ttfb opened a fresh TCP+TLS per sample (cold) -> inflated PM by a full
handshake. A real bot holds ONE persistent TLS connection and sees the WARM per-request RTT. Also tests
FAST EU crypto WS feeds (Kraken/OKX/Bybit) instead of Binance-Tokyo (BTC is arbitraged ~identical across
venues, so use the nearest feed as the price signal).
Run on the box: python3 backtest/warm_latency.py
"""
import socket
import ssl
import time
import statistics


def pctl(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float("nan")


def warm_rtts(host, port, path, n=20):
    """One persistent TLS conn; sequential keep-alive GETs; per-request send->first-byte RTT (warm).
    Returns (rtts_ms, cf_ray)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    raw.settimeout(8)
    raw.connect((host, port))
    ss = ctx.wrap_socket(raw, server_hostname=host)
    req = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: lat/1.0\r\n"
           f"Connection: keep-alive\r\nAccept: */*\r\n\r\n").encode()
    out, cf = [], ""
    for i in range(n + 2):
        t0 = time.perf_counter()
        ss.sendall(req)
        first = ss.recv(8192)
        t1 = time.perf_counter()
        if i >= 2:
            out.append((t1 - t0) * 1000)
        # parse + drain the full response so keep-alive can reuse the conn
        buf = first
        while b"\r\n\r\n" not in buf:
            c = ss.recv(8192)
            if not c:
                break
            buf += c
        head, _, rest = buf.partition(b"\r\n\r\n")
        hl = head.decode(errors="replace")
        if not cf:
            for ln in hl.split("\r\n"):
                if ln.lower().startswith("cf-ray:"):
                    cf = ln.split(":", 1)[1].strip()
        cl = None
        chunked = "transfer-encoding: chunked" in hl.lower()
        for ln in hl.split("\r\n"):
            if ln.lower().startswith("content-length:"):
                cl = int(ln.split(":")[1].strip())
        body = rest
        if cl is not None:
            while len(body) < cl:
                c = ss.recv(8192)
                if not c:
                    break
                body += c
        elif chunked:
            while not body.endswith(b"0\r\n\r\n"):
                c = ss.recv(8192)
                if not c:
                    break
                body += c
        time.sleep(0.05)
    ss.close()
    return out, cf


def tcp_rtt(host, port, n=12):
    out = []
    for _ in range(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(6)
        t0 = time.perf_counter()
        try:
            s.connect((host, port))
            out.append((time.perf_counter() - t0) * 1000)
        except Exception:  # noqa: BLE001
            pass
        finally:
            s.close()
        time.sleep(0.06)
    return out


def line(name, host, port):
    t = tcp_rtt(host, port)
    if t:
        print(f"   {name:24s} {host}:{port}  TCP RTT min={min(t):.1f} median={statistics.median(t):.1f}ms")
    else:
        print(f"   {name:24s} {host}:{port}  UNREACHABLE")
    return statistics.median(t) if t else None


def main():
    print("# CORRECTED warm latency probe (read-only, no orders)\n")
    print("## PM CLOB order-path WARM RTT (persistent keep-alive conn, per-request send->first-byte):")
    try:
        rtts, cf = warm_rtts("clob.polymarket.com", 443, "/time")
        print(f"   clob.polymarket.com/time  WARM RTT  min={min(rtts):.1f}  median={statistics.median(rtts):.1f}"
              f"  p90={pctl(rtts,0.9):.1f}ms  (n={len(rtts)})   cf-ray PoP={cf[-3:] if cf else '?'}")
    except Exception as e:  # noqa: BLE001
        print(f"   warm measure failed: {e}")

    print("\n## FAST EU crypto WS feeds (TCP RTT; one-way signal ~= RTT/2):")
    line("Kraken WS", "ws.kraken.com", 443)
    line("OKX WS", "ws.okx.com", 8443)
    line("OKX WS(443)", "ws.okx.com", 443)
    line("Bybit WS", "stream.bybit.com", 443)
    line("Coinbase WS", "ws-feed.exchange.coinbase.com", 443)
    line("Binance WS(Tokyo)", "stream.binance.com", 9443)

    print("\n## AWS-region triangulation (TCP RTT to dynamodb.<region> = Ireland<->region network):")
    for reg in ("eu-west-1", "eu-west-2", "eu-central-1", "us-east-1", "us-east-2"):
        line(reg, f"dynamodb.{reg}.amazonaws.com", 443)
    print("\n# -> compare PM WARM RTT above to these regional RTTs to locate PM's origin region.")


if __name__ == "__main__":
    main()
