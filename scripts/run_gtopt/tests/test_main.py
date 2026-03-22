# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the main CLI entry point."""

from pathlib import Path
from unittest.mock import patch

import pytest

from run_gtopt.main import main, make_parser


def test_parser_defaults():
    """Parser creates valid defaults."""
    parser = make_parser()
    args = parser.parse_args([])
    assert args.case is None
    assert args.threads is None
    assert args.compression == "zstd"
    assert args.check is True


def test_parser_plp_args():
    """--plp-args is parsed correctly."""
    parser = make_parser()
    args = parser.parse_args(["plp_dir", "--plp-args", "-y 1 -s 5"])
    assert args.case == "plp_dir"
    assert args.plp_args == "-y 1 -s 5"


def test_dry_run_passthrough(tmp_path: Path, capsys):
    """--dry-run with a JSON file prints the gtopt command."""
    json_file = tmp_path / "plan.json"
    json_file.write_text("{}")

    with patch("run_gtopt.main.find_gtopt_binary", return_value="/usr/bin/gtopt"):
        main([str(json_file), "--dry-run"])

    captured = capsys.readouterr()
    assert "[dry-run]" in captured.out
    assert "/usr/bin/gtopt" in captured.out


def test_dry_run_plp_case(tmp_path: Path, capsys):
    """--dry-run with a PLP directory prints the plp2gtopt command."""
    plp_dir = tmp_path / "plp_test"
    plp_dir.mkdir()
    (plp_dir / "plpblo.dat").write_text("")
    (plp_dir / "plpbar.dat").write_text("")

    with patch("run_gtopt.main.find_plp2gtopt", return_value="/usr/bin/plp2gtopt"):
        main([str(plp_dir), "--dry-run", "--convert-only"])

    captured = capsys.readouterr()
    assert "[dry-run]" in captured.out
    assert "plp2gtopt" in captured.out
    assert "gtopt_test" in captured.out


def test_no_case_no_cwd_detection(tmp_path: Path):
    """No args and CWD is not a case → exit 2."""
    import os

    orig = os.getcwd()
    os.chdir(tmp_path)
    try:
        with pytest.raises(SystemExit) as exc_info:
            main([])
        assert exc_info.value.code == 2
    finally:
        os.chdir(orig)
