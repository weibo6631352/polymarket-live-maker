# Deploy on the host (systemd)

Run the maker as a service so it survives reboots and crashes, with a graceful
stop that cancels all resting orders.

```bash
# code lives at /root/polymarket-live-maker with .venv built and .env in place
sudo cp deploy/polymarket-live-maker.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now polymarket-live-maker      # start + start-on-boot

# watch it
journalctl -u live-maker -f                            # or: tail -f state/runner.log

# graceful stop (SIGTERM -> cancels all orders + flattens, then exits)
sudo systemctl stop polymarket-live-maker

# emergency kill-switch (no restart needed): the runner sees the KILL file,
# cancels everything and stands down
touch /root/polymarket-live-maker/state/KILL
```

Notes:
- `Restart=always` + restart safety in code (`_rehydrate` + `_reconcile_broker_orders`)
  make a reboot/crash recoverable. Keep `state/` on the persistent disk (default).
- Go-live is still gated by `PM_TRADER_LIVE=1` in `.env` — the service does NOT flip it.
- On this host (Ireland, direct to Polymarket) **no proxy is needed**: leave
  `ALL_PROXY` / `LM_WS_PROXY` unset.
- Logs: httpx/websocket per-request logging is silenced to WARNING in code, and the
  per-poll event heartbeat is throttled (`LM_EVENT_POLL_EVERY_S`), so a 1s cadence
  does not flood `state/runner.log` or `state/events/`.
