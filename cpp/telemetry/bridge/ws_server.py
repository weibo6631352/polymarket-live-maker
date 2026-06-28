#!/usr/bin/env python3
"""ws_server.py — telemetry stdin -> WebSocket fanout for the pmm dashboard.

Reads JSON lines from stdin (produced by telemetry_tap) and broadcasts each line
VERBATIM to every connected WebSocket client on 0.0.0.0:8787 path /telemetry.

Design / guarantees:
  - stdin is read in a dedicated thread; broadcasting is scheduled onto the asyncio
    loop via call_soon_threadsafe, so a slow/blocked client NEVER blocks stdin.
  - Each client has a bounded outbound asyncio.Queue; when it overflows the line is
    dropped for that client (best-effort telemetry, no back-pressure / no unbounded RAM).
  - A small ring buffer (last ~200 lines) is replayed to each newly-connected client so
    the dashboard populates immediately on connect.

Deps: stdlib + `websockets` (pip install websockets). Tested across websockets 10-14.
"""
import asyncio
import sys
import threading
from collections import deque

import websockets

HOST = "0.0.0.0"
PORT = 8787
PATH = "/telemetry"
RING_MAX = 200          # replay buffer for late joiners
CLIENT_QUEUE_MAX = 1000  # per-client outbound backlog before we start dropping

ring = deque(maxlen=RING_MAX)
clients = set()          # set of asyncio.Queue, one per connected client
loop = None              # set in main()


def _client_path(ws) -> str:
    """Best-effort path extraction across websockets versions."""
    for attr in ("path",):
        p = getattr(ws, attr, None)
        if p:
            return p
    req = getattr(ws, "request", None)
    if req is not None:
        return getattr(req, "path", "") or ""
    return ""


async def handler(ws, *args):
    # websockets <11 passes (ws, path); >=11 passes (ws,) with ws.request.path.
    path = args[0] if args else _client_path(ws)
    if PATH and path and path.rstrip("/") not in (PATH, PATH.rstrip("/")):
        await ws.close(code=1008, reason="unknown path")
        return

    q: asyncio.Queue = asyncio.Queue(maxsize=CLIENT_QUEUE_MAX)
    # Replay the ring buffer first so the dashboard fills immediately.
    for line in list(ring):
        try:
            q.put_nowait(line)
        except asyncio.QueueFull:
            break
    clients.add(q)
    try:
        while True:
            line = await q.get()
            await ws.send(line)
    except Exception:
        pass  # client gone / send error -> drop it
    finally:
        clients.discard(q)


def broadcast(line: str):
    """Called on the event loop thread. Fan a line out to every client queue."""
    ring.append(line)
    for q in list(clients):
        try:
            q.put_nowait(line)
        except asyncio.QueueFull:
            pass  # slow client: drop this line for them, never block


def stdin_reader():
    """Blocking stdin read in its own thread; hand each line to the loop."""
    for raw in sys.stdin:
        line = raw.rstrip("\n")
        if not line:
            continue
        if loop is not None:
            loop.call_soon_threadsafe(broadcast, line)
    # stdin closed (upstream tap exited): stop the server.
    if loop is not None:
        loop.call_soon_threadsafe(loop.stop)


async def main():
    global loop
    loop = asyncio.get_running_loop()
    t = threading.Thread(target=stdin_reader, daemon=True)
    t.start()
    async with websockets.serve(handler, HOST, PORT):
        print(f"ws_server: listening on ws://{HOST}:{PORT}{PATH}", file=sys.stderr, flush=True)
        # Run until stdin_reader stops the loop (or Ctrl-C).
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (KeyboardInterrupt, RuntimeError):
        pass
