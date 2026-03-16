# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_compress_lp."""

from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import patch

import pytest

from gtopt_compress_lp._config import (
    default_config_path,
    get_compressor_status,
    load_config,
    pick_auto_compressor,
    save_config,
)
from gtopt_compress_lp.gtopt_compress_lp import (
    _build_parser,
    _run_compressor,
    compress_lp,
    main,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_lp(tmp_path: Path, name: str = "test.lp") -> Path:
    """Write a minimal LP file to *tmp_path* and return its path."""
    lp = tmp_path / name
    lp.write_text(
        "\\Problem name: test\n"
        "Minimize\n obj: x\n"
        "Subject To\n c1: x >= 1\n"
        "Bounds\n x >= 0\n"
        "End\n",
        encoding="utf-8",
    )
    return lp


# ---------------------------------------------------------------------------
# Config tests
# ---------------------------------------------------------------------------


def test_default_config_path_is_home_dot_conf() -> None:
    path = default_config_path()
    assert path.name == ".gtopt_compress_lp.conf"
    assert path.parent == Path.home()


def test_load_config_returns_defaults_when_missing(tmp_path: Path) -> None:
    cfg = load_config(tmp_path / "nonexistent.conf")
    assert cfg["compressor"] == "auto"
    assert cfg["extra_args"] == ""
    assert cfg["color"] == "auto"


def test_save_and_reload_config(tmp_path: Path) -> None:
    path = tmp_path / "test.conf"
    cfg = {"compressor": "zstd", "extra_args": "-3", "color": "always"}
    save_config(path, cfg)
    assert path.exists()
    loaded = load_config(path)
    assert loaded["compressor"] == "zstd"
    assert loaded["extra_args"] == "-3"
    assert loaded["color"] == "always"


def test_save_config_creates_parent_dirs(tmp_path: Path) -> None:
    path = tmp_path / "nested" / "dir" / "test.conf"
    save_config(path, {"compressor": "gzip", "extra_args": "", "color": "auto"})
    assert path.exists()


# ---------------------------------------------------------------------------
# Compressor status tests
# ---------------------------------------------------------------------------


def test_get_compressor_status_returns_list() -> None:
    status = get_compressor_status()
    assert isinstance(status, list)
    assert len(status) > 0
    binary, label, speed, installed, _hint, notes = status[0]
    assert isinstance(binary, str)
    assert isinstance(label, str)
    assert isinstance(speed, str)
    assert isinstance(installed, bool)
    assert isinstance(notes, str)


def test_pick_auto_compressor_returns_string_or_none() -> None:
    result = pick_auto_compressor()
    assert result is None or isinstance(result, str)


# ---------------------------------------------------------------------------
# _run_compressor tests
# ---------------------------------------------------------------------------


def test_run_compressor_missing_binary_returns_none(tmp_path: Path) -> None:
    lp = _write_lp(tmp_path)
    result = _run_compressor("__nonexistent_tool__", [], lp, quiet=True)
    assert result is None
    assert lp.exists()  # original untouched


@pytest.mark.skipif(sys.platform == "win32", reason="gzip not guaranteed on Windows")
def test_run_compressor_gzip_quiet(tmp_path: Path) -> None:
    lp = _write_lp(tmp_path)
    result = _run_compressor("gzip", [], lp, quiet=True)
    if result is None:
        pytest.skip("gzip not available on this system")
    assert result.suffix == ".gz" or str(result).endswith(".gz")


# ---------------------------------------------------------------------------
# compress_lp tests
# ---------------------------------------------------------------------------


def test_compress_lp_missing_file_quiet(tmp_path: Path) -> None:
    cfg = {"compressor": "auto", "extra_args": "", "color": "auto"}
    result = compress_lp(tmp_path / "ghost.lp", cfg, quiet=True)
    assert result is None


def test_compress_lp_no_compressor_available(tmp_path: Path) -> None:
    """When no compressor is available, compress_lp returns None without failing."""
    lp = _write_lp(tmp_path)
    cfg = {"compressor": "auto", "extra_args": "", "color": "auto"}

    with patch(
        "gtopt_compress_lp.gtopt_compress_lp.pick_auto_compressor",
        return_value=None,
    ):
        result = compress_lp(lp, cfg, quiet=True)

    assert result is None
    assert lp.exists()  # original preserved


def test_compress_lp_explicit_bad_compressor(tmp_path: Path) -> None:
    lp = _write_lp(tmp_path)
    cfg = {
        "compressor": "__no_such_tool__",
        "extra_args": "",
        "color": "auto",
    }
    with patch(
        "gtopt_compress_lp.gtopt_compress_lp.pick_auto_compressor",
        return_value=None,
    ):
        result = compress_lp(lp, cfg, quiet=True)
    assert result is None
    assert lp.exists()


# ---------------------------------------------------------------------------
# CLI (main) tests
# ---------------------------------------------------------------------------


def test_main_no_args_returns_0() -> None:
    assert main([]) == 0


def test_main_list_tools_returns_0(capsys: pytest.CaptureFixture[str]) -> None:
    rc = main(["--list-tools"])
    assert rc == 0
    out = capsys.readouterr().out
    # Should mention at least one compression tool
    assert any(tool in out for tool in ("gzip", "zstd", "lz4", "bzip2", "xz"))


def test_main_version_exits(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        main(["--version"])
    assert exc_info.value.code == 0


def test_main_init_config(tmp_path: Path) -> None:
    cfg_path = tmp_path / "test.conf"
    # Patch run_interactive_setup to avoid actual interactive input
    with patch(
        "gtopt_compress_lp.gtopt_compress_lp.run_interactive_setup",
        return_value={"compressor": "gzip", "extra_args": "", "color": "auto"},
    ) as mock_setup:
        rc = main(["--init-config", "--config", str(cfg_path)])
    assert rc == 0
    mock_setup.assert_called_once()


def test_main_missing_file_quiet(tmp_path: Path) -> None:
    rc = main(["--quiet", str(tmp_path / "ghost.lp")])
    assert rc == 0


def test_main_quiet_never_fails_on_bad_compressor(tmp_path: Path) -> None:
    lp = _write_lp(tmp_path)
    cfg_path = tmp_path / "test.conf"
    save_config(
        cfg_path,
        {"compressor": "__no_such_tool__", "extra_args": "", "color": "never"},
    )
    with patch(
        "gtopt_compress_lp.gtopt_compress_lp.pick_auto_compressor",
        return_value=None,
    ):
        rc = main(["--quiet", "--config", str(cfg_path), str(lp)])
    assert rc == 0  # quiet mode never returns non-zero


def test_main_first_run_no_tty_quiet(tmp_path: Path) -> None:
    """Without a config and in quiet mode, the tool proceeds without prompting."""
    lp = _write_lp(tmp_path)
    cfg_path = tmp_path / "noconf.conf"
    with patch(
        "gtopt_compress_lp.gtopt_compress_lp.pick_auto_compressor",
        return_value=None,
    ):
        rc = main(["--quiet", "--config", str(cfg_path), str(lp)])
    assert rc == 0


def test_build_parser_has_required_flags() -> None:
    parser = _build_parser()
    # Verify key arguments exist
    actions = {a.dest for a in parser._actions}  # noqa: SLF001
    assert "quiet" in actions
    assert "init_config" in actions
    assert "list_tools" in actions
    assert "compressor" in actions
    assert "config" in actions
