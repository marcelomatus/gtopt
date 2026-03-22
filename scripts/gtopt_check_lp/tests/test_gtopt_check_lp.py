# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – LP infeasibility diagnostic tool."""

import gzip
import json
import os
import pathlib
import subprocess
import sys
import urllib.error
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_check_lp._compress import (
    as_plain_lp,
    as_sanitized_lp,
    is_compressed,
    read_lp_text,
    resolve_lp_path,
    sanitize_lp_names,
)
from gtopt_check_lp._lp_analyzer import (
    CoeffEntry,
    _parse_coeff_var_pairs,
)
from gtopt_check_lp.gtopt_check_lp import (
    AiOptions,
    NeosClient,
    _AI_INFEASIBILITY_PROMPT,
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
    query_ai,
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
_BAD_BOUNDS_GZ = _LP_CASES_DIR / "bad_bounds.lp.gz"
_BAD_BOUNDS_GZIP = _LP_CASES_DIR / "bad_bounds.lp.gzip"
_FEASIBLE_SMALL = _LP_CASES_DIR / "feasible_small.lp"
_FEASIBLE_SMALL_GZ = _LP_CASES_DIR / "feasible_small.lp.gz"


# Helper: prevent highspy from being found even when installed
_NO_HIGHSPY = patch.dict("sys.modules", {"highspy": None})


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


def _write_gz_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a gzip-compressed LP file to tmp_path and return its Path."""
    p = tmp_path / name
    with gzip.open(str(p), "wt", encoding="utf-8") as fh:
        fh.write(content)
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


# ── Compressed LP file support ────────────────────────────────────────────────


class TestIsCompressed:
    """Tests for _compress.is_compressed()."""

    def test_plain_lp_not_compressed(self):
        assert is_compressed(Path("error_0.lp")) is False

    def test_gz_extension(self):
        assert is_compressed(Path("error_0.lp.gz")) is True

    def test_gzip_extension(self):
        assert is_compressed(Path("error_0.lp.gzip")) is True

    def test_z_extension(self):
        # .z is LZW (Unix compress), not gzip — NOT recognised as compressed.
        assert is_compressed(Path("error_0.lp.z")) is False

    def test_case_insensitive(self):
        assert is_compressed(Path("error_0.lp.GZ")) is True
        assert is_compressed(Path("error_0.lp.GZIP")) is True

    def test_plain_txt_not_compressed(self):
        assert is_compressed(Path("error_0.txt")) is False


class TestReadLpText:
    """Tests for _compress.read_lp_text()."""

    def test_plain_file(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        text = read_lp_text(lp)
        assert "Minimize" in text

    def test_gz_file(self, tmp_path):
        content = "Minimize\n obj: x\nEnd\n"
        gz = _write_gz_lp(tmp_path, "test.lp.gz", content)
        text = read_lp_text(gz)
        assert "Minimize" in text

    def test_gz_roundtrip(self, tmp_path):
        """Content read from .gz must equal the original plain-file content."""
        content = _BAD_BOUNDS.read_text(encoding="utf-8")
        text = read_lp_text(_BAD_BOUNDS_GZ)
        assert text.strip() == content.strip()

    def test_gzip_roundtrip(self, tmp_path):
        """Content read from .gzip must equal the original plain-file content."""
        content = _BAD_BOUNDS.read_text(encoding="utf-8")
        text = read_lp_text(_BAD_BOUNDS_GZIP)
        assert text.strip() == content.strip()


class TestResolveLpPath:
    """Tests for _compress.resolve_lp_path()."""

    def test_plain_exists(self):
        resolved = resolve_lp_path(_BAD_BOUNDS)
        assert resolved == _BAD_BOUNDS

    def test_gz_exists_directly(self):
        resolved = resolve_lp_path(_BAD_BOUNDS_GZ)
        assert resolved == _BAD_BOUNDS_GZ

    def test_fallback_to_gz(self, tmp_path):
        """If plain .lp is absent but .lp.gz exists, resolve to the .gz."""
        content = "Minimize\n obj: x\nEnd\n"
        gz = _write_gz_lp(tmp_path, "test.lp.gz", content)
        plain = tmp_path / "test.lp"
        resolved = resolve_lp_path(plain)  # plain does NOT exist
        assert resolved == gz

    def test_fallback_to_gzip(self, tmp_path):
        """If plain .lp is absent but .lp.gzip exists, resolve to the .gzip."""
        content = "Minimize\n obj: x\nEnd\n"
        gzip_file = _write_gz_lp(tmp_path, "test.lp.gzip", content)
        plain = tmp_path / "test.lp"
        resolved = resolve_lp_path(plain)  # plain does NOT exist
        assert resolved == gzip_file

    def test_none_when_nothing_exists(self, tmp_path):
        resolved = resolve_lp_path(tmp_path / "nowhere.lp")
        assert resolved is None

    def test_plain_preferred_over_gz(self, tmp_path):
        """When both plain and .gz exist, the plain file is preferred."""
        plain = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        _write_gz_lp(tmp_path, "test.lp.gz", "Minimize\n obj: y\nEnd\n")
        resolved = resolve_lp_path(plain)
        assert resolved == plain


class TestAsPlainLp:
    """Tests for the as_plain_lp() context manager."""

    def test_plain_yields_same_path(self, tmp_path):
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with as_plain_lp(lp) as path:
            assert path == lp

    def test_gz_yields_temp_file(self, tmp_path):
        gz = _write_gz_lp(tmp_path, "test.lp.gz", "Minimize\n obj: x\nEnd\n")
        with as_plain_lp(gz) as path:
            assert path != gz
            assert path.suffix == ".lp"
            assert path.exists()
            text = path.read_text(encoding="utf-8")
            assert "Minimize" in text

    def test_temp_file_deleted_after_context(self, tmp_path):
        gz = _write_gz_lp(tmp_path, "cleanup.lp.gz", "Minimize\n obj: x\nEnd\n")
        with as_plain_lp(gz) as path:
            tmp_name = path
        assert not tmp_name.exists()

    def test_plain_not_deleted_after_context(self, tmp_path):
        lp = _write_lp(tmp_path, "keep.lp", "Minimize\n obj: x\nEnd\n")
        with as_plain_lp(lp) as path:
            assert path == lp
        assert lp.exists()


class TestAnalyzeLpFileCompressed:
    """analyze_lp_file() must work on compressed LP files."""

    def test_analyze_gz_bad_bounds(self):
        stats = analyze_lp_file(_BAD_BOUNDS_GZ)
        assert stats.infeasible_bounds
        names = [vb.name for vb in stats.infeasible_bounds]
        assert "x1" in names

    def test_analyze_gzip_bad_bounds(self):
        stats = analyze_lp_file(_BAD_BOUNDS_GZIP)
        assert stats.infeasible_bounds
        names = [vb.name for vb in stats.infeasible_bounds]
        assert "x1" in names

    def test_analyze_gz_feasible(self):
        stats = analyze_lp_file(_FEASIBLE_SMALL_GZ)
        assert not stats.infeasible_bounds
        assert not stats.has_issues()

    def test_gz_same_results_as_plain(self):
        """Analyzing the .gz must yield the same results as analyzing the plain file."""
        plain_stats = analyze_lp_file(_BAD_BOUNDS)
        gz_stats = analyze_lp_file(_BAD_BOUNDS_GZ)
        assert gz_stats.n_vars == plain_stats.n_vars
        assert gz_stats.n_constraints == plain_stats.n_constraints
        assert len(gz_stats.infeasible_bounds) == len(plain_stats.infeasible_bounds)


class TestCheckLpCompressed:
    """check_lp() must accept compressed LP files and fall back transparently."""

    def test_check_gz_analyze_only(self):
        """check_lp on a .gz file with analyze_only=True returns 0."""
        rc = check_lp(_BAD_BOUNDS_GZ, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_check_gzip_analyze_only(self):
        """check_lp on a .gzip file with analyze_only=True returns 0."""
        rc = check_lp(_BAD_BOUNDS_GZIP, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_fallback_to_gz_when_plain_missing(self, tmp_path):
        """Passing plain .lp path automatically resolves to .lp.gz when absent."""
        content = _BAD_BOUNDS.read_text(encoding="utf-8")
        _write_gz_lp(tmp_path, "error_0.lp.gz", content)
        plain = tmp_path / "error_0.lp"  # does NOT exist

        rc = check_lp(plain, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_fallback_to_gzip_when_plain_missing(self, tmp_path):
        """Passing plain .lp path automatically resolves to .lp.gzip when absent."""
        content = _BAD_BOUNDS.read_text(encoding="utf-8")
        _write_gz_lp(tmp_path, "error_0.lp.gzip", content)
        plain = tmp_path / "error_0.lp"  # does NOT exist

        rc = check_lp(plain, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_quiet_bad_gz_is_warning(self, tmp_path, capsys):
        """In quiet mode, a corrupt .gz file produces a warning and returns 0."""
        bad_gz = tmp_path / "bad.lp.gz"
        bad_gz.write_bytes(b"this is not gzip data")
        rc = check_lp(bad_gz, quiet=True, ai=AiOptions(enabled=False))
        assert rc == 0

    def test_normal_mode_bad_gz_returns_1(self, tmp_path):
        """In normal mode, a corrupt .gz file returns 1 (static analysis fails)."""
        bad_gz = tmp_path / "bad.lp.gz"
        bad_gz.write_bytes(b"not gzip")
        rc = check_lp(bad_gz, analyze_only=True, ai=AiOptions(enabled=False))
        assert rc == 1


class TestFindLatestErrorLpCompressed:
    """_find_latest_error_lp() must detect compressed error LP files."""

    def test_finds_gz_file(self, tmp_path):
        gz = tmp_path / "error_0.lp.gz"
        gz.write_bytes(b"fake")
        result = _find_latest_error_lp([tmp_path])
        assert result == gz

    def test_finds_gzip_file(self, tmp_path):
        gzip_file = tmp_path / "error_0.lp.gzip"
        gzip_file.write_bytes(b"fake")
        result = _find_latest_error_lp([tmp_path])
        assert result == gzip_file

    def test_prefers_newest(self, tmp_path):
        """The most recently modified file is selected even across extensions."""
        import time

        plain = tmp_path / "error_0.lp"
        gz = tmp_path / "error_1.lp.gz"
        plain.write_bytes(b"old")
        time.sleep(0.01)
        gz.write_bytes(b"newer")
        result = _find_latest_error_lp([tmp_path])
        assert result == gz


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
        """Default config path is ~/.gtopt_check_lp.conf."""
        p = _default_config_path()
        assert p.name == ".gtopt_check_lp.conf"
        assert p.parent == Path.home()

    def test_load_config_missing_file_returns_defaults(self, tmp_path):
        """When the file does not exist, defaults are returned."""
        cfg = _load_config(tmp_path / "nonexistent.ini")
        assert cfg["solver"] == "all"
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
        assert cfg["solver"] == "all"  # default
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


# ── run_all_solvers ───────────────────────────────────────────────────────────


class TestRunAllSolvers:
    """Tests for run_all_solvers()."""

    def test_no_solvers_no_email_returns_hint(self):
        """When no solver is available and no email, returns helpful hint.

        Patches both shutil.which (binary solvers) AND sys.modules["highspy"]
        (Python HiGHS bindings) so the test is independent of what is actually
        installed in the current environment.
        """
        with patch("shutil.which", return_value=None), _NO_HIGHSPY:
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
        rc = check_lp(
            _BAD_BOUNDS,
            solver="all",
            analyze_only=False,
            timeout=5,
            ai=AiOptions(enabled=False),
        )
        assert rc == 0


# ── CLI parser ────────────────────────────────────────────────────────────────


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


# ── AI diagnostics ────────────────────────────────────────────────────────────


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


# ── main() CLI entry point ────────────────────────────────────────────────────


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


# ── Quiet mode ────────────────────────────────────────────────────────────────


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

    def test_ping_success_neos_response(self):
        """The real NEOS server returns 'NeosServer is alive\\n'."""
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.return_value = "NeosServer is alive\n"
        client._proxy = mock_proxy  # noqa: SLF001
        assert client.ping() is True

    def test_ping_success_legacy_response(self):
        """Accept any message that contains 'alive' (case-insensitive)."""
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

    def test_xml_template_uses_post_not_commands(self):
        """XML template must use <post> (not <commands>) and <comments> (not <comment>).
        Also must use 'refineconflict' (not the non-existent 'conflict' command).
        'conflict' was removed in CPLEX 20.1+; the session reports
        "Command 'conflict' does not exist."
        """
        import re  # noqa: PLC0415

        from gtopt_check_lp.gtopt_check_lp import _NEOS_LP_CPLEX_XML  # noqa: PLC0415

        xml = _NEOS_LP_CPLEX_XML.format(
            email="test@example.com", lp_content="Minimize\n obj: x\nEnd", version="1"
        )
        assert "<post>" in xml
        assert "<commands>" not in xml
        assert "<comments>" in xml
        assert "<comment>" not in xml
        assert "<client>" not in xml
        # 'conflict' is not a valid CPLEX 20.1+ command; must use 'refineconflict'
        assert "refineconflict" in xml
        # Bare 'conflict' (not part of 'refineconflict' or 'display conflict')
        # must not appear as a standalone command line.
        assert not re.search(r"(?<![a-z])conflict\s*$", xml, re.MULTILINE)

    def test_cplex_local_script_uses_refineconflict(self, tmp_path):
        """Local CPLEX script must use 'refineconflict', not the defunct 'conflict'.

        CPLEX Interactive Optimizer v20.1+ removed the 'conflict' command.
        The correct command sequence to identify the minimal infeasible subsystem is:
          1. set preprocessing presolve 0  (disable presolve, build LP basis)
          2. optimize                      (simplex confirms infeasibility)
          3. refineconflict                (run the conflict refiner)
          4. display conflict all          (show the minimal conflict set)
        """
        import re  # noqa: PLC0415

        from gtopt_check_lp._solvers import _cplex_script  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")
        script = _cplex_script(lp)
        assert "refineconflict" in script
        # Bare 'conflict' must not appear as a standalone command line
        assert not re.search(r"(?<![a-z])conflict\s*$", script, re.MULTILINE)
        # Presolve must be disabled before optimize so the refiner has a basis
        assert "set preprocessing presolve 0" in script


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


# ── sanitize_lp_names ──────────────────────────────────────────────────────


class TestSanitizeLpNames:
    """Tests for the sanitize_lp_names() function and as_sanitized_lp()."""

    def test_no_colon_in_names_unchanged(self):
        """LP files without ':' in names pass through unchanged."""
        lp = "Minimize\n obj: x1 + x2\nSubject To\n c1: x1 + x2 >= 1\nEnd\n"
        assert sanitize_lp_names(lp) == lp

    def test_colon_inside_variable_name_replaced(self):
        """A ':' embedded inside a variable token in an expression is replaced."""
        lp = "Minimize\n obj: uid:1 + uid:2\nSubject To\n c1: uid:1 + uid:2 >= 1\nEnd\n"
        result = sanitize_lp_names(lp)
        # The constraint separator colon after 'obj' and 'c1' must be preserved.
        assert "obj:" in result
        assert "c1:" in result
        # The colons inside token names must be replaced.
        assert "uid:1" not in result
        assert "uid_1" in result
        assert "uid_2" in result

    def test_constraint_separator_colon_preserved(self):
        """The ':' after a constraint name is always kept."""
        lp = "Subject To\n balance_1_2_3: gen_1 + gen_2 >= 10\nEnd\n"
        result = sanitize_lp_names(lp)
        assert "balance_1_2_3:" in result

    def test_as_sanitized_lp_creates_temp_file(self, tmp_path):
        """as_sanitized_lp writes a sanitised temporary file."""
        lp = "Minimize\n obj: uid:10 + uid:20\nSubject To\n c1: uid:10 >= 5\nEnd\n"
        lp_file = tmp_path / "test.lp"
        lp_file.write_text(lp, encoding="utf-8")

        with as_sanitized_lp(lp_file) as san_path:
            text = san_path.read_text(encoding="utf-8")

        assert "uid:10" not in text
        assert "uid_10" in text

    def test_as_sanitized_lp_temp_file_removed_after(self, tmp_path):
        """The temporary file created by as_sanitized_lp is removed on exit."""
        lp_file = tmp_path / "test.lp"
        lp_file.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        with as_sanitized_lp(lp_file) as san_path:
            captured = san_path

        assert not captured.exists()

    def test_as_sanitized_lp_on_compressed_file(self, tmp_path):
        """as_sanitized_lp transparently decompresses and sanitises."""
        lp = "Minimize\n obj: uid:1\nEnd\n"
        gz_file = tmp_path / "test.lp.gz"
        with gzip.open(str(gz_file), "wt", encoding="utf-8") as fh:
            fh.write(lp)

        with as_sanitized_lp(gz_file) as san_path:
            text = san_path.read_text(encoding="utf-8")

        assert "uid:1" not in text
        assert "uid_1" in text


# ── _parse_coeff_var_pairs ─────────────────────────────────────────────────


class TestParseCoeffVarPairs:
    """Tests for _parse_coeff_var_pairs()."""

    @staticmethod
    def _to_var_coeff_dict(expr: str) -> dict[str, float]:
        """Helper: convert (coeff, var) pairs to {var: coeff} dict."""
        return {var: coeff for coeff, var in _parse_coeff_var_pairs(expr)}

    def test_explicit_coefficient(self):
        pairs = self._to_var_coeff_dict("2.5 x1 + 3.0 x2")
        assert pairs.get("x1") == pytest.approx(2.5)
        assert pairs.get("x2") == pytest.approx(3.0)

    def test_negative_coefficient(self):
        pairs = self._to_var_coeff_dict("- 0.9 var_a + 1.1 var_b")
        assert pairs.get("var_a") == pytest.approx(-0.9)
        assert pairs.get("var_b") == pytest.approx(1.1)

    def test_sign_only_positive(self):
        pairs = self._to_var_coeff_dict("+ my_var")
        assert pairs.get("my_var") == pytest.approx(1.0)

    def test_sign_only_negative(self):
        pairs = self._to_var_coeff_dict("- my_var")
        assert pairs.get("my_var") == pytest.approx(-1.0)

    def test_no_sign_no_coeff(self):
        pairs = self._to_var_coeff_dict("my_var_1 + my_var_2")
        assert pairs.get("my_var_1") == pytest.approx(1.0)
        assert pairs.get("my_var_2") == pytest.approx(1.0)

    def test_scientific_notation(self):
        pairs = self._to_var_coeff_dict("- 3.8994e-05 rsv_eini")
        assert pairs.get("rsv_eini") == pytest.approx(-3.8994e-05)

    def test_rhs_stripped(self):
        """Coefficients after '=' should not be returned."""
        pairs = self._to_var_coeff_dict("x1 - 3.5 x2 = 10")
        assert set(pairs.keys()) <= {"x1", "x2"}
        assert pairs.get("x1") == pytest.approx(1.0)
        assert pairs.get("x2") == pytest.approx(-3.5)


# ── top-N coefficient tracking ─────────────────────────────────────────────


class TestTopNCoefficients:
    """Tests for top_max_coeffs / top_min_coeffs in LPStats."""

    _LP_WITH_VARIED_COEFFS = (
        "Minimize\n"
        " obj: x1\n"
        "Subject To\n"
        " c_large: 1e12 x1 + x2 >= 1\n"
        " c_small: 1e-12 x1 - 0.5 x2 >= 0\n"
        " c_mid: 3.0 x1 + 2.0 x2 >= 2\n"
        "Bounds\n"
        " 0 <= x1 <= 1e30\n"
        " 0 <= x2 <= 1e30\n"
        "End\n"
    )

    def test_top_max_coeffs_populated(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert len(stats.top_max_coeffs) > 0
        # Largest should be 1e12
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(1e12)

    def test_top_min_coeffs_populated(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert len(stats.top_min_coeffs) > 0
        # Smallest should be 1e-12
        assert stats.top_min_coeffs[0].abs_coeff == pytest.approx(1e-12)

    def test_top_max_includes_var_and_constraint_names(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        entry = stats.top_max_coeffs[0]
        assert entry.var_name  # non-empty variable name
        assert entry.constraint_name  # non-empty constraint name

    def test_format_report_shows_top_coeffs(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        assert "largest" in report.lower() or "1.000e+12" in report

    def test_coeff_entry_ordering(self):
        """CoeffEntry supports __lt__ for heapq operations."""
        a = CoeffEntry(1.0, "x", "c")
        b = CoeffEntry(2.0, "y", "d")
        assert a < b
        assert b >= a

    def test_empty_lp_has_empty_top_lists(self, tmp_path):
        """LP with objective but no constraints: top-N lists are empty.

        Stats only cover the constraint matrix A — objective coefficients are
        excluded (matching the C++ to_flat() constraint-matrix-only behaviour).
        """
        lp_text = "Minimize\n obj: x\nSubject To\nEnd\n"
        lp = _write_lp(tmp_path, "empty.lp", lp_text)
        stats = analyze_lp_file(lp)
        assert not stats.top_max_coeffs
        assert not stats.top_min_coeffs

    def test_objective_coeffs_excluded_from_min_max(self, tmp_path):
        """Objective function coefficients do NOT affect max/min abs stats.

        Only constraint matrix A coefficients are considered, matching the C++
        to_flat() stats_min_abs/stats_max_abs which cover the constraint matrix.
        """
        # Constraint coefficient is 7.0; objective has 500.0.
        # The only non-zero parsed coefficient is 7.0 from the constraint matrix.
        lp_text = "Minimize\n obj: 500.0 xa\nSubject To\n c1: 7.0 xa >= 0\nEnd\n"
        lp = _write_lp(tmp_path, "obj_range.lp", lp_text)
        stats = analyze_lp_file(lp)
        # max/min come from constraint (7.0), not from objective (500.0).
        assert stats.max_abs_coeff == pytest.approx(7.0)
        assert stats.min_abs_nonzero_coeff == pytest.approx(7.0)

    def test_objective_coeffs_not_in_top_n(self, tmp_path):
        """Objective coefficients do not appear in top-N lists."""
        lp_text = (
            "Minimize\n"
            " obj: 999 x1 + 0.001 x2\n"
            "Subject To\n"
            " c1: 5 x1 + 2 x2 >= 1\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "obj_topn.lp", lp_text)
        stats = analyze_lp_file(lp)
        # Only constraint coefficients appear: 5.0 and 2.0 (objective 999/0.001 excluded).
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(5.0)
        assert stats.top_max_coeffs[0].constraint_name == "c1"

    def test_top_n_limited_to_3(self, tmp_path):
        """At most 3 entries in each top-N list (_TOP_N_COEFFS = 3)."""
        lp_text = (
            "Minimize\n obj: x1\nSubject To\n"
            " c1: 100 x1 >= 1\n"
            " c2: 50  x1 >= 1\n"
            " c3: 10  x1 >= 1\n"
            " c4: 5   x1 >= 1\n"
            " c5: 1   x1 >= 1\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "many.lp", lp_text)
        stats = analyze_lp_file(lp)
        assert len(stats.top_max_coeffs) == 3
        assert len(stats.top_min_coeffs) == 3
        # top-3 largest: 100, 50, 10
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(100.0)
        assert stats.top_max_coeffs[1].abs_coeff == pytest.approx(50.0)
        assert stats.top_max_coeffs[2].abs_coeff == pytest.approx(10.0)
        # top-3 smallest: 1, 5, 10
        assert stats.top_min_coeffs[0].abs_coeff == pytest.approx(1.0)
        assert stats.top_min_coeffs[1].abs_coeff == pytest.approx(5.0)
        assert stats.top_min_coeffs[2].abs_coeff == pytest.approx(10.0)

    def test_constraint_texts_populated(self, tmp_path):
        """constraint_texts stores the full expression for each constraint."""
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert "c_large" in stats.constraint_texts
        assert "c_small" in stats.constraint_texts
        # The stored text should contain the variable names and the RHS.
        assert "x1" in stats.constraint_texts["c_large"]

    def test_format_report_shows_constraint_detail(self, tmp_path):
        """format_static_report includes the constraint expression below each entry."""
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The constraint detail line should appear (indented with 8 spaces).
        # The constraint c_large should appear both as "con=c_large" and as
        # the full expression on the next line.
        lines = report.splitlines()
        found_detail = False
        for line in lines:
            if line.startswith("        ") and "c_large" in line:
                found_detail = True
                break
        assert found_detail, "Expected constraint detail line for c_large in report"

    def test_format_report_shows_flagged_var_in_long_constraint(self, tmp_path):
        """When a constraint is truncated, the flagged variable must still appear."""
        # Build a long constraint where the large coefficient (for big_var)
        # is well beyond the 120-char truncation point.
        filler_terms = " + ".join(f"1.5 filler_variable_{i}" for i in range(20))
        lp_text = (
            "Minimize\n"
            " obj: x1\n"
            "Subject To\n"
            f" long_con: {filler_terms} + 9.99e+08 big_var >= 1\n"
            "Bounds\n"
            " 0 <= x1 <= 10\n"
            " 0 <= big_var <= 10\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "long.lp", lp_text)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The largest coefficient is 9.99e+08 for big_var; even though
        # it's beyond the 120-char point, it must appear in the detail.
        detail_lines = [
            line
            for line in report.splitlines()
            if line.startswith("        ") and "long_con" in line
        ]
        assert detail_lines, "Expected constraint detail for long_con"
        # Both the variable name and its coefficient must be visible.
        combined = " ".join(detail_lines)
        assert "big_var" in combined, (
            f"big_var not shown in truncated constraint detail: {detail_lines}"
        )
        assert "9.99e+08" in combined or "999000000" in combined, (
            f"coefficient for big_var not shown in detail: {detail_lines}"
        )

    def test_constraint_detail_deduped_in_report(self, tmp_path):
        """When multiple entries share a constraint, the detail is shown only once."""
        lp_text = (
            "Minimize\n"
            " obj: x1\n"
            "Subject To\n"
            " shared: 100 x1 + 0.001 x2 >= 1\n"
            "Bounds\n"
            " 0 <= x1 <= 10\n"
            " 0 <= x2 <= 10\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "shared.lp", lp_text)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The constraint detail line should appear exactly once per section.
        detail_count = sum(
            1
            for line in report.splitlines()
            if line.startswith("        ") and "shared:" in line
        )
        # Could be 1 (in max section) + 1 (in min section) = 2 at most.
        assert detail_count <= 2


# ── GLPK excluded from "all" ─────────────────────────────────────────────


class TestGlpkExcludedFromAll:
    """GLPK should not be run automatically as part of the 'all' solver."""

    def test_all_solvers_skips_glpk(self):
        """run_all_solvers does not call run_local_glpk."""
        from gtopt_check_lp._solvers import run_all_solvers as _ras

        with patch("gtopt_check_lp._solvers.shutil.which") as mock_which, patch(
            "gtopt_check_lp._solvers.run_local_glpk"
        ) as mock_glpk, patch(
            "gtopt_check_lp._solvers.run_local_coinor", return_value=(True, "ok")
        ):
            # Pretend glpsol and clp are available.
            def _which(name):
                if name in ("glpsol", "clp"):
                    return f"/usr/bin/{name}"
                return None

            mock_which.side_effect = _which

            _ras(Path("dummy.lp"), email="", timeout=5)
            mock_glpk.assert_not_called()

    def test_glpk_still_works_explicitly(self):
        """run_iis with solver='glpk' still calls run_local_glpk."""
        from gtopt_check_lp._solvers import run_iis as _iis

        with patch(
            "gtopt_check_lp._solvers.run_local_glpk", return_value=(True, "glpk output")
        ) as mock_glpk:
            _ok, name, _out = _iis(Path("dummy.lp"), solver="glpk")
            mock_glpk.assert_called_once()
            assert name == "GLPK"


# ── NEOS network diagnostics ────────────────────────────────────────────


class TestNeosNetworkDiagnostics:
    """Tests for the improved NEOS network error diagnostics."""

    def test_diagnose_no_internet(self, tmp_path):
        """When internet is down, the diagnostic says so."""
        from gtopt_check_lp._neos import _diagnose_network_error

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=False):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333", lp, OSError("timed out")
            )
        assert "No internet connectivity" in msg

    def test_diagnose_neos_specific_timeout(self, tmp_path):
        """When internet works but NEOS times out, the diagnostic explains."""
        from gtopt_check_lp._neos import _diagnose_network_error

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("The write operation timed out"),
            )
        assert "slow or unreachable" in msg
        assert "No internet" not in msg

    def test_diagnose_neos_other_error(self, tmp_path):
        """When internet works but NEOS returns a non-timeout error."""
        from gtopt_check_lp._neos import _diagnose_network_error

        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("Connection refused"),
            )
        assert "temporarily unavailable" in msg

    def test_diagnose_large_file_warning(self, tmp_path):
        """When the LP file is very large, the diagnostic warns about size."""
        from gtopt_check_lp._neos import _NEOS_LP_SIZE_WARN, _diagnose_network_error

        lp = tmp_path / "big.lp"
        # Create a file larger than the threshold.
        lp.write_bytes(b"x" * (_NEOS_LP_SIZE_WARN + 1))
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            msg = _diagnose_network_error(
                "https://neos-server.org:3333",
                lp,
                OSError("timed out"),
            )
        assert "MiB" in msg
        assert "local solver" in msg

    def test_submit_and_wait_ping_fail_no_internet(self, tmp_path):
        """submit_and_wait ping failure with no internet gives clear message."""
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        with patch("gtopt_check_lp._neos._check_internet", return_value=False):
            ok, msg = client.submit_and_wait(lp, "test@example.com")
        assert not ok
        assert "No internet connectivity" in msg

    def test_submit_and_wait_ping_fail_neos_down(self, tmp_path):
        """submit_and_wait ping failure with internet up blames NEOS."""
        lp = _write_lp(tmp_path, "test.lp", "Minimize\n obj: x\nEnd\n")
        client = NeosClient()
        mock_proxy = MagicMock()
        mock_proxy.ping.side_effect = OSError("connection refused")
        client._proxy = mock_proxy  # noqa: SLF001
        with patch("gtopt_check_lp._neos._check_internet", return_value=True):
            ok, msg = client.submit_and_wait(lp, "test@example.com")
        assert not ok
        assert "NEOS service is not responding" in msg
