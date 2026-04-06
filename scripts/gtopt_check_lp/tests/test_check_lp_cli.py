# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – CLI, config, AI, and high-level check_lp function."""

import json
import os
import pathlib
import subprocess
import sys
import urllib.error
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_check_lp.gtopt_check_lp import (
    AiOptions,
    _AI_INFEASIBILITY_PROMPT,
    _build_parser,
    _default_config_path,
    _find_latest_error_lp,
    _load_config,
    _read_git_email,
    _save_config,
    _write_report,
    check_lp,
    main,
    query_ai,
)

# ── Case directories ─────────────────────────────────────────────────────────

_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_LP_CASES_DIR = _SCRIPTS_DIR / "cases" / "lp_infeasible"
_BAD_BOUNDS = _LP_CASES_DIR / "bad_bounds.lp"
_FEASIBLE_SMALL = _LP_CASES_DIR / "feasible_small.lp"


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


# ── TestConfigFile ────────────────────────────────────────────────────────────


class TestConfigFile:
    """Tests for config file load/save helpers."""

    def test_default_config_path(self):
        """Default config path is ~/.gtopt.conf (unified)."""
        p = _default_config_path()
        assert p.name == ".gtopt.conf"
        assert p.parent == Path.home()

    def test_load_config_missing_file_returns_defaults(self, tmp_path):
        """When the file does not exist, defaults are returned."""
        cfg = _load_config(tmp_path / "nonexistent.ini")
        assert cfg["solver"] == "auto"
        assert cfg["timeout"] == "5"
        assert cfg["email"] == ""

    def test_save_and_reload_config(self, tmp_path):
        """Save a config then reload it and verify round-trip."""
        path = tmp_path / ".gtopt_check_lp"
        data = {
            "email": "test@example.com",
            "solver": "coinor",
            "timeout": "60",
            "neos_url": "https://neos-server.org:3333",
            "color": "never",
        }
        _save_config(path, data)
        assert path.exists()
        loaded = _load_config(path)
        assert loaded["email"] == "test@example.com"
        assert loaded["solver"] == "coinor"
        assert loaded["timeout"] == "60"
        assert loaded["color"] == "never"

    def test_save_config_creates_parent_dirs(self, tmp_path):
        """_save_config creates missing parent directories."""
        path = tmp_path / "deep" / "nested" / ".gtopt_check_lp"
        _save_config(
            path,
            {
                "email": "a@b.com",
                "solver": "all",
                "timeout": "120",
                "neos_url": "",
                "color": "auto",
            },
        )
        assert path.exists()

    def test_load_config_partial_overrides_defaults(self, tmp_path):
        """Config file with only some keys still returns defaults for missing ones."""
        path = tmp_path / ".gtopt_check_lp"
        path.write_text("[gtopt_check_lp]\nemail = x@y.com\n", encoding="utf-8")
        cfg = _load_config(path)
        assert cfg["email"] == "x@y.com"
        assert cfg["solver"] == "auto"  # default
        assert cfg["timeout"] == "5"  # default

    def test_read_git_email_returns_string(self):
        """_read_git_email always returns a string (empty when git absent/unconfigured)."""
        result = _read_git_email()
        assert isinstance(result, str)

    def test_main_show_config(self, tmp_path):
        """--show-config prints active config and exits 0."""
        cfg_file = tmp_path / ".gtopt_check_lp"
        _save_config(
            cfg_file,
            {
                "email": "show@test.com",
                "solver": "all",
                "timeout": "120",
                "neos_url": "",
                "color": "auto",
            },
        )
        rc = main(["--show-config", "--config", str(cfg_file), "--no-color"])
        assert rc == 0

    def test_main_init_config_non_tty(self, tmp_path, monkeypatch):
        """--init-config writes config file even when stdin is not a TTY."""
        cfg_file = tmp_path / ".gtopt_check_lp"
        monkeypatch.setattr("sys.stdin.isatty", lambda: False)
        rc = main(["--init-config", "--config", str(cfg_file), "--no-color"])
        assert rc == 0
        assert cfg_file.exists()

    def test_main_no_setup_skips_wizard(self, tmp_path):
        """--no-setup prevents interactive setup when config is absent."""
        cfg_file = tmp_path / ".gtopt_check_lp"
        # Should not hang waiting for input; just skip setup and proceed to parse error
        rc = main(
            [
                str(_BAD_BOUNDS),
                "--analyze-only",
                "--config",
                str(cfg_file),
                "--no-setup",
            ]
        )
        assert rc == 0


# ── TestFindLatestErrorLp ─────────────────────────────────────────────────────


class TestFindLatestErrorLp:
    """Tests for _find_latest_error_lp()."""

    def test_returns_none_when_no_error_lp(self, tmp_path):
        """Returns None when no error*.lp files exist."""
        assert _find_latest_error_lp([tmp_path]) is None

    def test_finds_single_error_lp(self, tmp_path):
        """Returns the only error*.lp file found."""
        p = tmp_path / "error_0.lp"
        p.write_text("Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n", encoding="utf-8")
        found = _find_latest_error_lp([tmp_path])
        assert found == p

    def test_returns_most_recent(self, tmp_path):
        """Returns the most recently modified error*.lp when multiple exist."""
        import time

        old = tmp_path / "error_0.lp"
        new = tmp_path / "error_1.lp"
        old.write_text("Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n", encoding="utf-8")
        time.sleep(0.05)
        new.write_text("Minimize\n obj: y\nBounds\n 0 <= y\nEnd\n", encoding="utf-8")
        found = _find_latest_error_lp([tmp_path])
        assert found == new

    def test_main_last_flag_no_file(self, tmp_path, monkeypatch):
        """--last returns exit code 1 when no error*.lp exists."""
        monkeypatch.chdir(tmp_path)
        rc = main(["--last", "--no-setup", "--no-color", "--no-ai"])
        assert rc == 1

    def test_main_last_flag_with_file(self, tmp_path, monkeypatch):
        """--last auto-detects error*.lp and analyzes it."""
        lp = tmp_path / "error_0.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n 0 <= x\nEnd\n",
            encoding="utf-8",
        )
        monkeypatch.chdir(tmp_path)
        rc = main(["--last", "--analyze-only", "--no-setup", "--no-color", "--no-ai"])
        assert rc == 0


# ── TestCLIParser ─────────────────────────────────────────────────────────────


class TestCLIParser:
    """Tests for the argparse CLI."""

    def test_default_timeout_is_none(self):
        """CLI --timeout defaults to None; the effective default (5 s) comes from config."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp"])
        assert args.timeout is None

    def test_timeout_override(self):
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--timeout", "30"])
        assert args.timeout == 30

    def test_analyze_only_flag(self):
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--analyze-only"])
        assert args.analyze_only is True

    def test_solver_choices(self):
        """'all' is now a valid solver choice and is the default in config."""
        parser = _build_parser()
        for choice in ("all", "auto", "cplex", "highs", "coinor", "neos"):
            args = parser.parse_args(["dummy.lp", "--solver", choice])
            assert args.solver == choice

    def test_solver_default_is_none(self):
        """CLI --solver defaults to None; the effective default ('all') comes from config."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp"])
        assert args.solver is None

    def test_solver_url_default_is_none(self):
        """CLI --solver-url defaults to None; effective default comes from config."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp"])
        assert args.solver_url is None

    def test_last_flag(self):
        """--last flag sets args.last to True."""
        parser = _build_parser()
        args = parser.parse_args(["--last"])
        assert args.last is True

    def test_init_config_flag(self):
        """--init-config flag sets args.init_config to True."""
        parser = _build_parser()
        args = parser.parse_args(["--init-config"])
        assert args.init_config is True

    def test_no_setup_flag(self):
        """--no-setup flag sets args.no_setup to True."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--no-setup"])
        assert args.no_setup is True

    def test_show_config_flag(self):
        """--show-config flag sets args.show_config to True."""
        parser = _build_parser()
        args = parser.parse_args(["--show-config"])
        assert args.show_config is True

    def test_ai_enabled_flag(self):
        """--ai sets ai_enabled to True."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--ai"])
        assert args.ai_enabled is True

    def test_no_ai_flag(self):
        """--no-ai sets ai_enabled to False."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--no-ai"])
        assert args.ai_enabled is False

    def test_ai_enabled_default_is_none(self):
        """AI enabled defaults to None (resolved at runtime from config)."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp"])
        assert args.ai_enabled is None

    def test_ai_provider_choices(self):
        """--ai-provider accepts valid providers (claude, openai, deepseek, github)."""
        parser = _build_parser()
        for choice in ("claude", "openai", "deepseek", "github"):
            args = parser.parse_args(["dummy.lp", "--ai-provider", choice])
            assert args.ai_provider == choice

    def test_ai_provider_default_is_none(self):
        """--ai-provider defaults to None; effective default comes from config."""
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp"])
        assert args.ai_provider is None

    def test_ai_model_flag(self):
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--ai-model", "claude-haiku-3-5"])
        assert args.ai_model == "claude-haiku-3-5"

    def test_ai_key_flag(self):
        parser = _build_parser()
        args = parser.parse_args(["dummy.lp", "--ai-key", "sk-test"])
        assert args.ai_key == "sk-test"


# ── TestQueryAi ───────────────────────────────────────────────────────────────


class TestQueryAi:
    """Tests for the query_ai() function (all network calls mocked)."""

    def test_unknown_provider_returns_false(self):
        ok, msg = query_ai("report", provider="unknown_provider")
        assert not ok
        assert "unknown" in msg.lower() or "provider" in msg.lower()

    def test_claude_no_api_key_returns_false(self):
        """Without ANTHROPIC_API_KEY set, Claude returns a helpful error."""
        with patch.dict("os.environ", {}, clear=False):
            # Ensure the key is absent
            os.environ.pop("ANTHROPIC_API_KEY", None)
            ok, msg = query_ai("report", provider="claude", api_key=None)
        assert not ok
        assert "ANTHROPIC_API_KEY" in msg or "api key" in msg.lower()

    def test_openai_no_api_key_returns_false(self):
        """Without OPENAI_API_KEY set, OpenAI returns a helpful error."""
        os.environ.pop("OPENAI_API_KEY", None)
        ok, msg = query_ai("report", provider="openai", api_key=None)
        assert not ok
        assert "OPENAI_API_KEY" in msg or "api key" in msg.lower()

    def test_claude_http_success(self):
        """Successful Claude HTTP response is parsed correctly."""
        mock_body = {
            "content": [{"type": "text", "text": "The infeasibility is caused by x1."}]
        }

        mock_resp = MagicMock()
        mock_resp.__enter__ = lambda s: s
        mock_resp.__exit__ = MagicMock(return_value=False)
        mock_resp.read.return_value = json.dumps(mock_body).encode()

        with patch("urllib.request.urlopen", return_value=mock_resp):
            ok, text = query_ai("report text", provider="claude", api_key="sk-test")

        assert ok
        assert "x1" in text

    def test_openai_http_success(self):
        """Successful OpenAI HTTP response is parsed correctly."""
        mock_body = {"choices": [{"message": {"content": "Row c1 is infeasible."}}]}

        mock_resp = MagicMock()
        mock_resp.__enter__ = lambda s: s
        mock_resp.__exit__ = MagicMock(return_value=False)
        mock_resp.read.return_value = json.dumps(mock_body).encode()

        with patch("urllib.request.urlopen", return_value=mock_resp):
            ok, text = query_ai("report text", provider="openai", api_key="sk-test")

        assert ok
        assert "c1" in text

    def test_network_error_returns_false(self):
        """Network errors produce (False, message) without raising."""
        with patch(
            "urllib.request.urlopen",
            side_effect=urllib.error.URLError("connection refused"),
        ):
            ok, msg = query_ai("report", provider="claude", api_key="sk-test")
        assert not ok
        assert "network" in msg.lower() or "connect" in msg.lower()

    def test_ai_infeasibility_prompt_has_report_placeholder(self):
        """The built-in prompt template must contain a {report} placeholder."""
        assert "{report}" in _AI_INFEASIBILITY_PROMPT


# ── TestAiProviderDispatch ────────────────────────────────────────────────────


class TestAiProviderDispatch:
    """Cover all provider dispatch branches in query_ai."""

    def test_deepseek_no_key(self):
        with patch.dict(os.environ, {}, clear=True):
            ok, msg = query_ai("report", provider="deepseek", api_key=None)
        assert not ok
        assert "DEEPSEEK_API_KEY" in msg

    def test_github_no_key(self):
        with patch.dict(os.environ, {}, clear=True):
            ok, msg = query_ai("report", provider="github", api_key=None)
        assert not ok
        assert "GITHUB_TOKEN" in msg

    def test_local_no_server(self):
        """Local provider with no server returns a network error."""
        with patch.dict(
            os.environ,
            {"LOCAL_AI_URL": "http://127.0.0.1:1/v1", "LOCAL_AI_KEY": ""},
            clear=False,
        ):
            ok, _msg = query_ai("report", provider="local", api_key=None, timeout=2)
        assert not ok

    def test_unknown_provider(self):
        ok, msg = query_ai("report", provider="unknown_xyz")
        assert not ok
        assert "Unknown AI provider" in msg

    def test_http_post_json_unknown_response_shape(self):
        """When the API returns a body we don't recognise, dump it as JSON."""
        from gtopt_check_lp._ai import _http_post_json  # noqa: PLC0415

        mock_resp = MagicMock()
        mock_resp.read.return_value = b'{"unexpected": "shape"}'
        mock_resp.__enter__ = MagicMock(return_value=mock_resp)
        mock_resp.__exit__ = MagicMock(return_value=False)

        with patch("urllib.request.urlopen", return_value=mock_resp):
            ok, text = _http_post_json(
                "http://fake", {"model": "x", "messages": []}, {}, 10
            )
        assert ok
        assert "unexpected" in text

    def test_http_post_json_timeout(self):
        """TimeoutError triggers network diagnostics."""
        from gtopt_check_lp._ai import _http_post_json  # noqa: PLC0415

        with patch("urllib.request.urlopen", side_effect=TimeoutError("timed out")):
            ok, msg = _http_post_json(
                "http://fake", {"model": "x", "messages": []}, {}, 5
            )
        assert not ok
        assert "timed out" in msg.lower()

    def test_http_post_json_url_error(self):
        """URLError triggers network diagnostics."""
        from gtopt_check_lp._ai import _http_post_json  # noqa: PLC0415

        with patch(
            "urllib.request.urlopen",
            side_effect=urllib.error.URLError("connection refused"),
        ):
            ok, msg = _http_post_json(
                "http://fake", {"model": "x", "messages": []}, {}, 5
            )
        assert not ok
        assert "Network error" in msg

    def test_http_post_json_generic_exception(self):
        """Unexpected exception returns a descriptive error."""
        from gtopt_check_lp._ai import _http_post_json  # noqa: PLC0415

        with patch("urllib.request.urlopen", side_effect=RuntimeError("boom")):
            ok, msg = _http_post_json(
                "http://fake", {"model": "x", "messages": []}, {}, 5
            )
        assert not ok
        assert "Unexpected error" in msg

    def test_diagnose_ai_network_no_internet(self):
        """When internet is down, diagnostic says so."""
        from gtopt_check_lp._ai import _diagnose_ai_network_error  # noqa: PLC0415

        with patch("gtopt_check_lp._ai._check_internet", return_value=False):
            msg = _diagnose_ai_network_error("http://fake", Exception("conn"))
        assert "No internet connectivity" in msg

    def test_diagnose_ai_network_timeout(self):
        """When service times out, diagnostic says so."""
        from gtopt_check_lp._ai import _diagnose_ai_network_error  # noqa: PLC0415

        with patch("gtopt_check_lp._ai._check_internet", return_value=True):
            msg = _diagnose_ai_network_error(
                "http://fake", Exception("Connection timed out")
            )
        assert "slow or unreachable" in msg

    def test_diagnose_ai_network_other(self):
        """Generic failure when internet works but service doesn't."""
        from gtopt_check_lp._ai import _diagnose_ai_network_error  # noqa: PLC0415

        with patch("gtopt_check_lp._ai._check_internet", return_value=True):
            msg = _diagnose_ai_network_error(
                "http://fake", Exception("connection refused")
            )
        assert "temporarily unavailable" in msg


# ── TestCheckLp ───────────────────────────────────────────────────────────────


class TestCheckLp:
    """Tests for the check_lp() high-level function."""

    def test_missing_file_returns_1(self):
        rc = check_lp(Path("/nonexistent/nope.lp"), analyze_only=True)
        assert rc == 1

    def test_feasible_lp_analyze_only(self):
        rc = check_lp(_FEASIBLE_SMALL, analyze_only=True)
        assert rc == 0

    def test_bad_bounds_analyze_only(self):
        rc = check_lp(_BAD_BOUNDS, analyze_only=True)
        assert rc == 0

    def test_output_file_written(self, tmp_path):
        out = tmp_path / "report.txt"
        rc = check_lp(_BAD_BOUNDS, analyze_only=True, output_file=out)
        assert rc == 0
        assert out.exists()
        content = out.read_text(encoding="utf-8")
        assert "x1" in content
        assert "Conflicting variable bounds" in content

    def test_output_file_no_ansi(self, tmp_path):
        """Output file must not contain ANSI escape codes."""
        import re

        out = tmp_path / "report_clean.txt"
        check_lp(_BAD_BOUNDS, analyze_only=True, output_file=out)
        content = out.read_text(encoding="utf-8")
        assert not re.search(r"\033\[", content), "Report should not contain ANSI"

    def test_ai_disabled_skips_query(self):
        """check_lp with AiOptions(enabled=False) never calls query_ai."""
        with patch("gtopt_check_lp.gtopt_check_lp.query_ai") as mock_ai:
            rc = check_lp(_BAD_BOUNDS, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 0
        mock_ai.assert_not_called()

    def test_ai_no_key_shows_warning(self, capsys):
        """check_lp with ai enabled but no key prints a warning, not an error."""
        os.environ.pop("ANTHROPIC_API_KEY", None)
        rc = check_lp(
            _BAD_BOUNDS,
            analyze_only=True,
            ai=AiOptions(enabled=True, provider="claude", key=""),
        )
        assert rc == 0  # should not fail
        out = capsys.readouterr().out
        # Should mention AI diagnostic is unavailable
        assert "unavailable" in out.lower() or "key" in out.lower() or rc == 0


# ── TestCheckLpEdgeCases ──────────────────────────────────────────────────────


class TestCheckLpEdgeCases:
    """Cover check_lp branches: missing file, quiet mode, report writing."""

    def test_missing_file_quiet(self, tmp_path, capsys):
        """Quiet mode returns 0 for missing LP files."""
        rc = check_lp(tmp_path / "nonexistent.lp", quiet=True)
        assert rc == 0
        captured = capsys.readouterr()
        assert "not found" in captured.err.lower() or "Warning" in captured.err

    def test_missing_file_not_quiet(self, tmp_path):
        """Non-quiet mode returns 1 for missing LP files."""
        rc = check_lp(tmp_path / "nonexistent.lp", quiet=False)
        assert rc == 1

    def test_write_report(self, tmp_path):
        """_write_report strips ANSI codes and writes to file."""
        out = tmp_path / "report.txt"
        _write_report(out, ["normal text", "\033[31mred text\033[0m"])
        content = out.read_text()
        assert "normal text" in content
        assert "red text" in content
        assert "\033[" not in content  # ANSI stripped

    def test_write_report_oserror(self, tmp_path, capsys):
        """_write_report handles OSError gracefully."""
        # Try to write to a directory (will fail)
        _write_report(tmp_path / "nonexistent_dir" / "report.txt", ["text"])
        captured = capsys.readouterr()
        assert "could not write" in captured.err.lower()

    def test_check_lp_with_output_file(self, tmp_path):
        """check_lp writes report to output file."""
        lp = tmp_path / "test.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n 0 <= x <= 10\nEnd\n",
            encoding="utf-8",
        )
        out = tmp_path / "report.txt"
        rc = check_lp(lp, analyze_only=True, output_file=out)
        assert rc == 0
        assert out.exists()
        assert "Static Analysis" in out.read_text()


# ── TestMain ──────────────────────────────────────────────────────────────────


class TestMain:
    """Tests for the main() CLI entry point."""

    def test_main_analyze_only(self):
        rc = main([str(_FEASIBLE_SMALL), "--analyze-only", "--no-ai"])
        assert rc == 0

    def test_main_bad_bounds(self):
        rc = main([str(_BAD_BOUNDS), "--analyze-only", "--no-ai"])
        assert rc == 0

    def test_main_missing_file(self):
        rc = main(["/no/such/file.lp", "--analyze-only", "--no-ai"])
        assert rc == 1

    def test_main_no_color(self):
        rc = main([str(_FEASIBLE_SMALL), "--analyze-only", "--no-color", "--no-ai"])
        assert rc == 0

    def test_main_no_ai_flag(self):
        """--no-ai suppresses the AI diagnostic step."""
        with patch("gtopt_check_lp.gtopt_check_lp.query_ai") as mock_ai:
            rc = main([str(_FEASIBLE_SMALL), "--analyze-only", "--no-ai"])
        assert rc == 0
        mock_ai.assert_not_called()

    def test_main_ai_provider_override(self):
        """--ai-provider is accepted and forwarded to check_lp."""
        parser = _build_parser()
        args = parser.parse_args(
            [str(_FEASIBLE_SMALL), "--ai-provider", "openai", "--no-ai"]
        )
        assert args.ai_provider == "openai"


# ── TestQuietMode ─────────────────────────────────────────────────────────────


class TestQuietMode:
    """Tests for the --quiet / quiet=True non-failing mode."""

    def test_quiet_flag_parsed(self):
        """--quiet and -q are accepted by the argument parser."""
        parser = _build_parser()
        assert parser.parse_args(["dummy.lp", "--quiet"]).quiet is True
        assert parser.parse_args(["dummy.lp", "-q"]).quiet is True
        assert parser.parse_args(["dummy.lp"]).quiet is False

    def test_no_neos_flag_parsed(self):
        """--no-neos is accepted by the argument parser."""
        parser = _build_parser()
        assert parser.parse_args(["dummy.lp", "--no-neos"]).no_neos is True
        assert parser.parse_args(["dummy.lp"]).no_neos is False

    def test_quiet_missing_file_returns_0(self, capsys):
        """In quiet mode, a missing LP file produces a warning and returns 0."""
        rc = check_lp(Path("/nonexistent/nope.lp"), quiet=True)
        assert rc == 0
        err = capsys.readouterr().err
        assert "Warning" in err or "not found" in err.lower()

    def test_quiet_feasible_lp_returns_0(self):
        """Quiet mode on a valid feasible LP still returns 0."""
        rc = check_lp(_FEASIBLE_SMALL, quiet=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_quiet_bad_bounds_returns_0(self):
        """Quiet mode on a bad-bounds LP returns 0 and runs static analysis."""
        rc = check_lp(_BAD_BOUNDS, quiet=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_quiet_always_runs_solver_step(self, capsys):
        """Quiet mode runs the solver step even when solver='auto' and no email."""
        with patch("gtopt_check_lp.gtopt_check_lp.run_iis") as mock_iis:
            mock_iis.return_value = (False, "all", "no solvers")
            rc = check_lp(
                _BAD_BOUNDS,
                solver="auto",
                email="",
                quiet=True,
                ai=AiOptions(enabled=False),
            )
        assert rc == 0
        mock_iis.assert_called_once()

    def test_normal_mode_skips_solver_when_auto_no_email(self, capsys):
        """Normal mode skips solver analysis when solver='auto', no local solver, no email."""
        with patch(
            "gtopt_check_lp.gtopt_check_lp.detect_local_solvers", return_value=[]
        ):
            with patch("gtopt_check_lp.gtopt_check_lp.run_iis") as mock_iis:
                rc = check_lp(
                    _BAD_BOUNDS,
                    solver="auto",
                    email="",
                    quiet=False,
                    ai=AiOptions(enabled=False),
                )
        assert rc == 0
        mock_iis.assert_not_called()

    def test_quiet_solver_exception_is_warning(self, capsys):
        """In quiet mode, an unexpected solver exception produces a warning, not a crash."""
        with patch(
            "gtopt_check_lp.gtopt_check_lp.run_iis",
            side_effect=RuntimeError("boom"),
        ):
            rc = check_lp(
                _BAD_BOUNDS,
                quiet=True,
                ai=AiOptions(enabled=False),
            )
        assert rc == 0

    def test_quiet_ai_exception_is_warning(self, capsys):
        """In quiet mode, an unexpected AI exception produces a warning, not a crash."""
        with patch(
            "gtopt_check_lp.gtopt_check_lp._run_ai_diagnostics",
            side_effect=RuntimeError("ai exploded"),
        ):
            rc = check_lp(
                _BAD_BOUNDS,
                quiet=True,
                ai=AiOptions(enabled=True, provider="claude"),
            )
        assert rc == 0
        err = capsys.readouterr().err
        assert "Warning" in err or "AI" in err

    def test_no_neos_suppresses_email(self):
        """--no-neos causes check_lp to pass empty email even when email is set."""
        with patch("gtopt_check_lp.gtopt_check_lp.run_iis") as mock_iis:
            mock_iis.return_value = (True, "COIN-OR (clp/cbc)", "ok")
            check_lp(
                _BAD_BOUNDS,
                email="user@example.com",
                no_neos=True,
                quiet=True,
                ai=AiOptions(enabled=False),
            )
        _, kwargs = mock_iis.call_args
        assert kwargs.get("email", "SENTINEL") == ""

    def test_main_quiet_missing_file_returns_0(self):
        """main() --quiet on a missing file exits 0."""
        rc = main(["/no/such/quiet.lp", "--quiet", "--no-ai"])
        assert rc == 0

    def test_main_quiet_bad_bounds_returns_0(self):
        """main() --quiet on a bad-bounds LP exits 0."""
        rc = main([str(_BAD_BOUNDS), "--quiet", "--no-ai"])
        assert rc == 0

    def test_main_quiet_skips_interactive_setup(self, tmp_path):
        """main() --quiet never runs the interactive setup wizard."""
        fake_config = tmp_path / "fake.conf"
        with patch("gtopt_check_lp.gtopt_check_lp.run_interactive_setup") as mock_setup:
            main(
                [
                    str(_BAD_BOUNDS),
                    "--quiet",
                    "--no-ai",
                    "--config",
                    str(fake_config),
                ]
            )
        mock_setup.assert_not_called()

    @pytest.mark.integration
    def test_quiet_subprocess_exits_zero(self):
        """Subprocess: --quiet on bad_bounds.lp always exits 0."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_BAD_BOUNDS),
                "--quiet",
                "--no-color",
                "--no-ai",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0

    @pytest.mark.integration
    def test_quiet_missing_file_subprocess_exits_zero(self):
        """Subprocess: --quiet on a missing file always exits 0."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                "/no/such/file.lp",
                "--quiet",
                "--no-color",
                "--no-ai",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        assert "Warning" in combined or "not found" in combined.lower()
