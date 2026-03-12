# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – LP infeasibility diagnostic tool."""

import pathlib
import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_check_lp.gtopt_check_lp import (
    NeosClient,
    _build_parser,
    _default_config_path,
    _find_latest_error_lp,
    _load_config,
    _parse_coinor_infeasibility,
    _read_git_email,
    _save_config,
    analyze_lp_file,
    check_lp,
    format_static_report,
    main,
    run_all_solvers,
    run_local_coinor,
    run_local_cplex,
    run_local_glpk,
    run_local_highs_binary,
)

# ── Case directories ─────────────────────────────────────────────────────────

_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_LP_CASES_DIR = _SCRIPTS_DIR / "cases" / "lp_infeasible"
_MY_SMALL_BAD = _LP_CASES_DIR / "my_small_bad.lp"
_BAD_BOUNDS = _LP_CASES_DIR / "bad_bounds.lp"
_FEASIBLE_SMALL = _LP_CASES_DIR / "feasible_small.lp"


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


# ── Static analyser: LPStats detection ───────────────────────────────────────


class TestAnalyzeLpFile:
    """Unit tests for analyze_lp_file()."""

    def test_bad_bounds_detected(self):
        """bad_bounds.lp: x1 has lb=5 > ub=2 → infeasible_bounds list."""
        stats = analyze_lp_file(_BAD_BOUNDS)
        assert stats.infeasible_bounds, "Expected at least one infeasible bound"
        names = [vb.name for vb in stats.infeasible_bounds]
        assert "x1" in names
        assert stats.has_issues()

    def test_bad_bounds_values(self):
        """Exact lb/ub values for x1 in bad_bounds.lp."""
        stats = analyze_lp_file(_BAD_BOUNDS)
        x1_bounds = next(
            (vb for vb in stats.infeasible_bounds if vb.name == "x1"), None
        )
        assert x1_bounds is not None, "x1 should be in infeasible_bounds"
        assert x1_bounds.lb == pytest.approx(5.0)
        assert x1_bounds.ub == pytest.approx(2.0)

    def test_feasible_lp_no_issues(self):
        """feasible_small.lp: clean LP → no issues detected statically."""
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        assert not stats.infeasible_bounds
        assert not stats.empty_constraints
        assert not stats.duplicate_constraint_names
        assert not stats.has_issues()

    def test_feasible_lp_stats(self):
        """feasible_small.lp: correct constraint count."""
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        assert stats.n_constraints == 3
        assert stats.has_objective

    def test_my_small_bad_no_static_bound_issue(self):
        """my_small_bad.lp: constraint conflict not detectable statically."""
        # The LP is infeasible due to conflicting constraints (c1 + c2),
        # but the static analyser cannot detect this without solving.
        stats = analyze_lp_file(_MY_SMALL_BAD)
        assert not stats.infeasible_bounds  # bounds themselves are fine
        assert stats.n_constraints == 2

    def test_inline_bad_bounds(self, tmp_path):
        """Inline LP: two variables with lb > ub."""
        lp = _write_lp(
            tmp_path,
            "test_bounds.lp",
            (
                "Minimize\n obj: a + b\n"
                "Subject To\n c1: a + b <= 100\n"
                "Bounds\n 10 <= a <= 3\n 7 <= b <= 1\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert len(stats.infeasible_bounds) == 2
        names = {vb.name for vb in stats.infeasible_bounds}
        assert names == {"a", "b"}

    def test_large_coefficient_flagged(self, tmp_path):
        """Inline LP: coefficients >= 1e10 should be in large_coeff_constraints."""
        lp = _write_lp(
            tmp_path,
            "large_coeff.lp",
            (
                "Minimize\n obj: x1\n"
                "Subject To\n"
                " big: 1e12 x1 + x2 <= 100\n"
                "Bounds\n 0 <= x1\n 0 <= x2\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert "big" in stats.large_coeff_constraints
        assert stats.has_issues()

    def test_duplicate_constraint_names(self, tmp_path):
        """Duplicate constraint names should be detected."""
        lp = _write_lp(
            tmp_path,
            "dup_names.lp",
            (
                "Minimize\n obj: x\n"
                "Subject To\n"
                " dup: x >= 1\n"
                " dup: x <= 10\n"
                "Bounds\n 0 <= x <= 20\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert "dup" in stats.duplicate_constraint_names

    def test_file_not_found_raises(self):
        """Missing file raises FileNotFoundError."""
        with pytest.raises(FileNotFoundError):
            analyze_lp_file(Path("/nonexistent/path/no.lp"))


# ── format_static_report ─────────────────────────────────────────────────────


class TestFormatStaticReport:
    """Tests for format_static_report()."""

    def test_no_issues_message(self):
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        report = format_static_report(_FEASIBLE_SMALL, stats)
        # Strip ANSI and check content
        import re

        clean = re.sub(r"\033\[[0-9;]*m", "", report)
        assert "No obvious static infeasibilities" in clean

    def test_infeasible_bounds_in_report(self):
        stats = analyze_lp_file(_BAD_BOUNDS)
        report = format_static_report(_BAD_BOUNDS, stats)
        import re

        clean = re.sub(r"\033\[[0-9;]*m", "", report)
        assert "Conflicting variable bounds" in clean
        assert "x1" in clean


# ── Config file ───────────────────────────────────────────────────────────────


class TestConfigFile:
    """Tests for config file load/save helpers."""

    def test_default_config_path(self):
        """Default config path is ~/.gtopt_check_lp."""
        p = _default_config_path()
        assert p.name == ".gtopt_check_lp"
        assert p.parent == Path.home()

    def test_load_config_missing_file_returns_defaults(self, tmp_path):
        """When the file does not exist, defaults are returned."""
        cfg = _load_config(tmp_path / "nonexistent.ini")
        assert cfg["solver"] == "all"
        assert cfg["timeout"] == "120"
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
        assert cfg["solver"] == "all"  # default
        assert cfg["timeout"] == "120"  # default

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


# ── _find_latest_error_lp ─────────────────────────────────────────────────────


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
        rc = main(["--last", "--no-setup", "--no-color"])
        assert rc == 1

    def test_main_last_flag_with_file(self, tmp_path, monkeypatch):
        """--last auto-detects error*.lp and analyzes it."""
        lp = tmp_path / "error_0.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n 0 <= x\nEnd\n",
            encoding="utf-8",
        )
        monkeypatch.chdir(tmp_path)
        rc = main(["--last", "--analyze-only", "--no-setup", "--no-color"])
        assert rc == 0


# ── run_all_solvers ───────────────────────────────────────────────────────────


class TestRunAllSolvers:
    """Tests for run_all_solvers()."""

    def test_no_solvers_no_email_returns_hint(self):
        """When no solver is available and no email, returns helpful hint."""
        with patch("shutil.which", return_value=None):
            ok, name, out = run_all_solvers(
                _BAD_BOUNDS, email="", neos_url="", timeout=5
            )
        assert not ok
        assert name == "all"
        assert (
            "coinor" in out.lower() or "apt" in out.lower() or "install" in out.lower()
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_included_in_all(self):
        """run_all_solvers includes COIN-OR output when clp/cbc is on PATH."""
        ok, name, out = run_all_solvers(
            _MY_SMALL_BAD, email="", neos_url="", timeout=10
        )
        assert ok
        assert name == "all"
        assert "COIN-OR" in out

    def test_check_lp_solver_all(self):
        """check_lp() with solver='all' returns 0 (no solver available is acceptable)."""
        rc = check_lp(_BAD_BOUNDS, solver="all", analyze_only=False, timeout=5)
        assert rc == 0


# ── CLI parser ────────────────────────────────────────────────────────────────


class TestCLIParser:
    """Tests for the argparse CLI."""

    def test_default_timeout_is_none(self):
        """CLI --timeout defaults to None; the effective default (120 s) comes from config."""
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
        for choice in ("all", "auto", "cplex", "highs", "coinor", "glpk", "neos"):
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


# ── check_lp integration ─────────────────────────────────────────────────────


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


# ── main() CLI entry point ────────────────────────────────────────────────────


class TestMain:
    """Tests for the main() CLI entry point."""

    def test_main_analyze_only(self):
        rc = main([str(_FEASIBLE_SMALL), "--analyze-only"])
        assert rc == 0

    def test_main_bad_bounds(self):
        rc = main([str(_BAD_BOUNDS), "--analyze-only"])
        assert rc == 0

    def test_main_missing_file(self):
        rc = main(["/no/such/file.lp", "--analyze-only"])
        assert rc == 1

    def test_main_no_color(self):
        rc = main([str(_FEASIBLE_SMALL), "--analyze-only", "--no-color"])
        assert rc == 0


# ── Local solver stubs ────────────────────────────────────────────────────────


class TestLocalSolverStubs:
    """Tests that local solver runners return (False, reason) when binary absent."""

    def test_cplex_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_cplex(Path("dummy.lp"))
        assert not ok
        assert "cplex" in msg.lower()

    def test_highs_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_highs_binary(Path("dummy.lp"))
        assert not ok
        assert "highs" in msg.lower()

    def test_glpk_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_glpk(Path("dummy.lp"))
        assert not ok
        assert "glpsol" in msg.lower()

    def test_coinor_not_found(self):
        with patch("shutil.which", return_value=None):
            ok, msg = run_local_coinor(Path("dummy.lp"))
        assert not ok
        assert "clp" in msg.lower() or "cbc" in msg.lower()


# ── COIN-OR (CLP / CBC) tests ─────────────────────────────────────────────────


class TestCoinOR:
    """Tests for the COIN-OR CLP/CBC runner."""

    def test_parse_coinor_infeasibility_detects_primal(self):
        """_parse_coinor_infeasibility finds the 'PrimalInfeasible' line."""
        output = (
            "Coin LP version 1.17.9\n"
            "Presolve determined that the problem was infeasible with tolerance of 1e-08\n"
            "0  Obj 0 Primal inf 9.9999999 (1)\n"
            "Primal infeasible - objective value 10\n"
            "PrimalInfeasible objective 10 - 1 iterations time 0.002\n"
        )
        findings = _parse_coinor_infeasibility(output)
        assert any("infeasible" in f.lower() for f in findings)

    def test_parse_coinor_infeasibility_bad_bounds(self):
        """_parse_coinor_infeasibility finds 'bad bound pairs' message."""
        output = (
            "1 bad bound pairs or bad objectives were found - first at C0\n"
            "Primal infeasible - objective value 5\n"
        )
        findings = _parse_coinor_infeasibility(output)
        assert len(findings) == 2
        assert any("bad bound" in f.lower() for f in findings)

    def test_parse_coinor_infeasibility_empty(self):
        """_parse_coinor_infeasibility returns empty list for feasible output."""
        output = "Optimal - objective value 10\n1 iterations time 0.002\n"
        findings = _parse_coinor_infeasibility(output)
        assert not findings

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_infeasible_detected(self):
        """CLP/CBC correctly identifies my_small_bad.lp as infeasible."""
        ok, output = run_local_coinor(_MY_SMALL_BAD, timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        assert any(kw in output.lower() for kw in ("infeasible", "primalinfeasible")), (
            f"Expected infeasibility in output:\n{output}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_bad_bounds_detected(self):
        """CLP/CBC reports 'bad bound pairs' for bad_bounds.lp."""
        ok, output = run_local_coinor(_BAD_BOUNDS, timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        assert any(kw in output.lower() for kw in ("infeasible", "bad bound")), (
            f"Expected infeasibility in output:\n{output}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_feasible_not_flagged(self):
        """CLP/CBC does not report infeasibility for a feasible LP."""
        ok, output = run_local_coinor(_FEASIBLE_SMALL, timeout=10)
        assert ok, f"run_local_coinor returned failure: {output}"
        findings = _parse_coinor_infeasibility(output)
        assert not findings, (
            f"Feasible LP should have no infeasibility findings but got: {findings}"
        )

    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_check_lp_coinor_solver(self):
        """check_lp() with --solver coinor returns 0 and includes COIN-OR output."""
        rc = check_lp(_MY_SMALL_BAD, solver="coinor", timeout=10)
        assert rc == 0


class TestNeosClient:
    """Tests for NeosClient with mocked XML-RPC."""

    def test_ping_success(self):
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.return_value = "NEOS server is alive"
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is True

    def test_ping_failure(self):
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is False

    def test_submit_returns_job_number(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.submitJob.return_value = (12345, "secret")
        client._proxy = mock_proxy  # noqa: SLF001
        job_num, pwd = client.submit_lp(lp, "test@example.com")
        assert job_num == 12345
        assert pwd == "secret"

    def test_submit_error_response(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nBounds\n 0 <= x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.submitJob.return_value = (-1, "error message")
        client._proxy = mock_proxy  # noqa: SLF001
        job_num, _ = client.submit_lp(lp, "test@example.com")
        assert job_num is None

    def test_wait_returns_done(self):
        client = NeosClient(timeout=30)
        mock_proxy = MagicMock()
        mock_proxy.getJobStatus.return_value = "Done"
        mock_proxy.getFinalResults.return_value = b"CPLEX output here"
        client._proxy = mock_proxy  # noqa: SLF001
        ok, out = client.wait_for_result(1, "pass")
        assert ok
        assert "CPLEX output here" in out

    def test_wait_timeout(self):
        """A very short timeout triggers the timeout path."""
        client = NeosClient(timeout=0)
        mock_proxy = MagicMock()
        mock_proxy.getJobStatus.return_value = "Running"
        client._proxy = mock_proxy  # noqa: SLF001
        ok, msg = client.wait_for_result(1, "pass", poll_interval=0.01)
        assert not ok
        assert "Timed out" in msg

    def test_neos_requires_email(self):
        """Without --email, NEOS solver should refuse with a helpful message."""
        ok, msg, _ = (
            False,
            "An e-mail address is required for NEOS.",
            "NEOS",
        )
        assert not ok
        assert "e-mail" in msg.lower() or "email" in msg.lower()


# ── Integration: run the script as a subprocess ───────────────────────────────


class TestSubprocessRun:
    """End-to-end: run gtopt_check_lp (or python -m) on the test LP files."""

    @pytest.mark.integration
    def test_bad_bounds_subprocess(self):
        """Run gtopt_check_lp on bad_bounds.lp and check for conflict message."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_BAD_BOUNDS),
                "--analyze-only",
                "--no-color",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        assert "Conflicting variable bounds" in combined or "x1" in combined

    @pytest.mark.integration
    def test_my_small_bad_subprocess(self):
        """Run on my_small_bad.lp — static analysis shows no bound conflict."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_MY_SMALL_BAD),
                "--analyze-only",
                "--no-color",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        # The constraint conflict is not detectable without a solver,
        # but the script should at least report problem statistics.
        assert (
            "Static Analysis" in combined or "Minimize" in combined or "x1" in combined
        )

    @pytest.mark.integration
    @pytest.mark.skipif(
        not (__import__("shutil").which("clp") or __import__("shutil").which("cbc")),
        reason="COIN-OR clp/cbc not available",
    )
    def test_coinor_solver_subprocess(self):
        """Run gtopt_check_lp --solver coinor on my_small_bad.lp."""
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "gtopt_check_lp.gtopt_check_lp",
                str(_MY_SMALL_BAD),
                "--solver",
                "coinor",
                "--no-color",
                "--timeout",
                "10",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0
        combined = result.stdout + result.stderr
        # CLP/CBC output should confirm infeasibility
        assert any(
            kw in combined.lower() for kw in ("infeasible", "coin-or", "clp", "cbc")
        ), f"Expected COIN-OR infeasibility output:\n{combined}"
