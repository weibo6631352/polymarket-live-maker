#!/usr/bin/env python3
"""Pin PM's matching-engine origin region behind Cloudflare via RTT triangulation. ZERO real money —
read-only network probing, no orders.

All PM subdomains are Cloudflare-proxied (no direct origin IP). So localize the origin by matching the
WARM, CACHE-BUSTED (uncacheable = order-like) round-trip to clob.polymarket.com against fine-grained
AWS-region reference RTTs (TCP to dynamodb.<region>). The CF edge is ~2ms (Dublin); the warm-busted RTT
minus that ~= edge->origin->edge, which pins the origin metro. Reports the PM RTT distribution (clusters
=> multi-region) + the closest region.
Run on the box: python3 backtest/pm_origin.py
"""
import socket
import ssl
import time
import statistics


def pctl(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs)-1, int(p*len(xs)))] if xs else float("nan")


def warm_busted(host, port, path, n=80):
    """Persistent TLS keep-alive conn; per-request send->first-byte RTT with a unique query each time
    (forces Cloudflare cache-MISS => origin every request)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    raw.settimeout(8)
    raw.connect((host, port))
    ss = ctx.wrap_socket(raw, server_hostname=host)
    out = []
    for i in range(n + 3):
        p = f"{path}?cb={i}_{int(time.time()*1e6)}"
        req = (f"GET {p} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: o/1\r\n"
               f"Connection: keep-alive\r\nCache-Control: no-cache\r\nAccept: */*\r\n\r\n").encode()
        t0 = time.perf_counter()
        ss.sendall(req)
        first = ss.recv(8192)
        out.append((time.perf_counter() - t0) * 1000) if i >= 3 else None
        # drain full response for keep-alive reuse
        buf = first
        while b"\r\n\r\n" not in buf:
            c = ss.recv(8192)
            if not c:
                break
            buf += c
        head, _, rest = buf.partition(b"\r\n\r\n")
        hl = head.decode(errors="replace").lower()
        cl = next((int(x.split(":")[1]) for x in hl.split("\r\n") if x.startswith("content-length:")), None)
        body = rest
        if cl is not None:
            while len(body) < cl:
                c = ss.recv(8192)
                if not c:
                    break
                body += c
        elif "transfer-encoding: chunked" in hl:
            while not body.endswith(b"0\r\n\r\n"):
                c = ss.recv(8192)
                if not c:
                    break
                body += c
        time.sleep(0.04)
    ss.close()
    return [x for x in out if x is not None]


def tcp_rtt(host, port, n=20):
    out = []
    for _ in range(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        t0 = time.perf_counter()
        try:
            s.connect((host, port))
            out.append((time.perf_counter()-t0)*1000)
        except Exception:  # noqa: BLE001
            pass
        finally:
            s.close()
        time.sleep(0.05)
    return out


def main():
    print("# PM origin RTT-triangulation (read-only)\n")
    print("## AWS region reference RTTs (TCP to dynamodb.<region>) from this box:")
    regions = [("eu-west-1", "Ireland"), ("eu-west-2", "London"), ("eu-west-3", "Paris"),
               ("eu-central-1", "Frankfurt"), ("eu-central-2", "Zurich"), ("eu-north-1", "Stockholm"),
               ("eu-south-1", "Milan"), ("us-east-1", "Virginia")]
    refs = {}
    for reg, city in regions:
        t = tcp_rtt(f"dynamodb.{reg}.amazonaws.com", 443)
        if t:
            refs[reg] = (min(t), statistics.median(t))
            print(f"   {reg:14s} {city:10s} min={min(t):5.1f}  median={statistics.median(t):5.1f}ms")
        else:
            print(f"   {reg:14s} {city:10s} UNREACHABLE")

    print("\n## PM clob.polymarket.com WARM cache-busted (order-like, uncacheable) RTT:")
    try:
        r = warm_busted("clob.polymarket.com", 443, "/time", n=100)
        print(f"   n={len(r)}  min={min(r):.1f}  p10={pctl(r,0.1):.1f}  p25={pctl(r,0.25):.1f}  "
              f"median={statistics.median(r):.1f}  p75={pctl(r,0.75):.1f}  p90={pctl(r,0.9):.1f}ms")
        # crude cluster split at the gap
        rs = sorted(r)
        gaps = [(rs[i+1]-rs[i], i) for i in range(len(rs)-1)]
        gmax, gi = max(gaps) if gaps else (0, 0)
        if gmax > 15:
            lo = rs[:gi+1]; hi = rs[gi+1:]
            print(f"   -> BIMODAL: cluster A median={statistics.median(lo):.1f}ms ({len(lo)}), "
                  f"cluster B median={statistics.median(hi):.1f}ms ({len(hi)}) -> likely MULTI-REGION origin")
            pm_origin_rtt = statistics.median(lo)
        else:
            pm_origin_rtt = statistics.median(r)
            print(f"   -> single cluster, origin RTT ~ {pm_origin_rtt:.1f}ms")
    except Exception as e:  # noqa: BLE001
        print(f"   measure failed: {e}"); return

    # CF edge is ~2ms (Dublin). origin network leg ~= pm_rtt - edge_overhead(~4-6ms server+tls-resume)
    print(f"\n## LOCALIZATION (PM order-path RTT min={min(r):.1f}, primary-cluster ~{pm_origin_rtt:.1f}ms):")
    best = sorted(refs.items(), key=lambda kv: abs(kv[1][0] - min(r)))
    for reg, (mn, md) in best[:4]:
        print(f"   {reg:14s} region-min={mn:.1f}ms  | PM-min {min(r):.1f}ms diff={min(r)-mn:+.1f}ms")
    print(f"   -> closest region to PM origin: {best[0][0]} (the PM min should be region-RTT + ~few ms CF/proc)")


if __name__ == "__main__":
    main()
