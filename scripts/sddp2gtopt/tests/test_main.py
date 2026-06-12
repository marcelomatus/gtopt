"""Smoke tests for :mod:`sddp2gtopt.main` (CLI dispatch)."""

from __future__ import annotations

from pathlib import Path

import pytest

from sddp2gtopt.main import main, make_parser


def test_help_smoke(capsys: pytest.CaptureFixture[str]) -> None:
    parser = make_parser()
    with pytest.raises(SystemExit) as exc:
        parser.parse_args(["--help"])
    assert exc.value.code == 0
    captured = capsys.readouterr().out
    assert "sddp2gtopt" in captured
    assert "--info" in captured
    assert "--validate" in captured


def test_version_smoke(capsys: pytest.CaptureFixture[str]) -> None:
    parser = make_parser()
    with pytest.raises(SystemExit) as exc:
        parser.parse_args(["--version"])
    assert exc.value.code == 0
    captured = capsys.readouterr().out
    assert "0.1.0" in captured


def test_no_input_errors(capsys: pytest.CaptureFixture[str]) -> None:
    """``main`` without a case dir exits non-zero with a usage message."""
    with pytest.raises(SystemExit) as exc:
        main([])
    assert exc.value.code != 0
    err = capsys.readouterr().err
    assert "input directory" in err.lower()


def test_validate_case0_succeeds(case0_dir: Path) -> None:
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(case0_dir)])
    assert exc.value.code == 0


def test_info_case0_succeeds(
    case0_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    main(["--info", str(case0_dir)])
    out = capsys.readouterr().out
    assert "Thermal plants:" in out
    assert "Thermal 1" in out


def test_convert_default_succeeds(case_min_dir: Path, tmp_path: Path) -> None:
    """``main`` without ``--info`` / ``--validate`` runs the converter."""
    main([str(case_min_dir), "-o", str(tmp_path / "out")])
    assert (tmp_path / "out").is_dir()
    assert any((tmp_path / "out").glob("*.json"))


def test_validate_missing_dir_exits_nonzero(tmp_path: Path) -> None:
    bogus = tmp_path / "nope"
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(bogus)])
    assert exc.value.code != 0
