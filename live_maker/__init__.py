"""polymarket-live-maker — autonomous live liquidity-rewards market maker.

Built on the official unified SDK ``polymarket-client`` (``AsyncSecureClient``).
WebSocket-driven, one async process, no LLM in the hot path. Real-money
submission is gated behind the operator's explicit ``PM_LIVE=1`` switch; the
default is dry-run (compute + log every action, send nothing).

The validated decision/reward logic (``reward_math``, ``scanner``, ``discovery``,
``portfolio``, ``strategy``) is migrated verbatim from the paper-trader research
build; ``execution``/``feed``/``runner`` are the new live layer.
"""

__version__ = "0.1.0"
