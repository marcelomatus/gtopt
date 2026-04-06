# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – compressed LP file support."""

import gzip
import pathlib
from pathlib import Path

from gtopt_check_lp._compress import (
    as_plain_lp,
    as_sanitized_lp,
    is_compressed,
    read_lp_text,
    resolve_lp_path,
)
from gtopt_check_lp.gtopt_check_lp import (
    AiOptions,
    _find_latest_error_lp,
    analyze_lp_file,
    check_lp,
)

# ── Case directories ─────────────────────────────────────────────────────────

_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_LP_CASES_DIR = _SCRIPTS_DIR / "cases" / "lp_infeasible"
_BAD_BOUNDS = _LP_CASES_DIR / "bad_bounds.lp"
_BAD_BOUNDS_GZ = _LP_CASES_DIR / "bad_bounds.lp.gz"
_BAD_BOUNDS_GZIP = _LP_CASES_DIR / "bad_bounds.lp.gzip"
_FEASIBLE_SMALL_GZ = _LP_CASES_DIR / "feasible_small.lp.gz"


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


# ── TestIsCompressed ──────────────────────────────────────────────────────────


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


# ── TestReadLpText ────────────────────────────────────────────────────────────


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


# ── TestResolveLpPath ─────────────────────────────────────────────────────────


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


# ── TestAsPlainLp ─────────────────────────────────────────────────────────────


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


# ── TestAnalyzeLpFileCompressed ───────────────────────────────────────────────


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


# ── TestCheckLpCompressed ─────────────────────────────────────────────────────


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


# ── TestFindLatestErrorLpCompressed ──────────────────────────────────────────


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


# ── TestSanitizeLpNames (compressed variant) ──────────────────────────────────


class TestSanitizeLpNamesCompressed:
    """Tests for as_sanitized_lp() on compressed files."""

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
