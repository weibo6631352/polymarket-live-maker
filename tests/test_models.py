"""Tests for the OrderBook helper methods."""

from __future__ import annotations

from live_maker.models import OrderBook, OrderBookLevel


def test_midpoint_full_book():
    b = OrderBook(bids=[OrderBookLevel(0.49, 10)], asks=[OrderBookLevel(0.51, 10)])
    assert b.best_bid() == 0.49 and b.best_ask() == 0.51 and b.midpoint() == 0.50


def test_midpoint_one_sided_none():
    assert OrderBook(bids=[OrderBookLevel(0.49, 10)]).midpoint() is None
    assert OrderBook().best_bid() is None
    assert OrderBook().best_ask() is None
