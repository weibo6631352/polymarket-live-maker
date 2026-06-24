"""Tests for config + the operator real-money gate + the .env reader."""

from __future__ import annotations

import pytest

from live_maker.config import Config, load_dotenv


class TestDotenv:
    def test_reads_pairs(self, tmp_path, monkeypatch):
        monkeypatch.delenv("FOO_X", raising=False)
        p = tmp_path / ".env"
        p.write_text('FOO_X="bar"\n# comment\n\nBAZ=qux\n')
        parsed = load_dotenv(p)
        assert parsed["FOO_X"] == "bar" and parsed["BAZ"] == "qux"

    def test_missing_file_noop(self, tmp_path):
        assert load_dotenv(tmp_path / "nope.env") == {}

    def test_does_not_override_env(self, tmp_path, monkeypatch):
        monkeypatch.setenv("FOO_Y", "already")
        (tmp_path / ".env").write_text("FOO_Y=fromfile\n")
        load_dotenv(tmp_path / ".env")
        import os
        assert os.environ["FOO_Y"] == "already"


class TestConfigGate:
    def test_default_is_dry_run(self, monkeypatch):
        monkeypatch.delenv("PM_LIVE", raising=False)
        cfg = Config.from_env(dotenv=None)
        assert cfg.live is False

    def test_pm_live_1_enables(self, monkeypatch):
        monkeypatch.setenv("PM_LIVE", "1")
        cfg = Config.from_env(dotenv=None)
        assert cfg.live is True

    def test_pm_live_other_values_stay_off(self, monkeypatch):
        for v in ("0", "true", "yes", "TRUE", "2"):
            monkeypatch.setenv("PM_LIVE", v)
            assert Config.from_env(dotenv=None).live is False

    def test_require_key_raises_without_key(self, monkeypatch):
        monkeypatch.delenv("POLYMARKET_PRIVATE_KEY", raising=False)
        cfg = Config.from_env(dotenv=None)
        with pytest.raises(RuntimeError):
            cfg.require_key()

    def test_env_overrides_numbers(self, monkeypatch):
        monkeypatch.setenv("LM_CAPITAL", "500")
        monkeypatch.setenv("LM_MAX_POOLS", "7")
        cfg = Config.from_env(dotenv=None)
        assert cfg.capital == 500.0 and cfg.max_pools == 7

    def test_bad_number_falls_back(self, monkeypatch):
        monkeypatch.setenv("LM_CAPITAL", "notanumber")
        cfg = Config.from_env(dotenv=None)
        assert cfg.capital == 200.0

    def test_banner_mentions_mode(self, monkeypatch):
        monkeypatch.delenv("PM_LIVE", raising=False)
        assert "DRY-RUN" in Config.from_env(dotenv=None).banner()
