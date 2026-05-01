"""Tests for :mod:`sddp2gtopt.info_display`."""

from __future__ import annotations

from pathlib import Path

import pytest

from sddp2gtopt.info_display import display_sddp_info


def test_display_case0(case0_dir: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """``--info`` on case0 prints the section headers and known names."""
    display_sddp_info({"input_dir": case0_dir})
    out = capsys.readouterr().out
    # Section headers
    assert "SDDP case:" in out
    assert "Collections:" in out
    assert "Study:" in out
    assert "Systems:" in out
    assert "Demands:" in out
    assert "Fuels:" in out
    assert "Thermal plants:" in out
    assert "Hydro plants:" in out
    assert "Gauging stations:" in out
    # Known content from case0
    assert "PSRStudy" in out
    assert "PSRThermalPlant" in out
    assert "Thermal 1" in out
    assert "Thermal 2" in out
    assert "Thermal 3" in out
    assert "Hydro 1" in out
    assert "Fuel 1" in out
    assert "Fuel 2" in out
    assert "System 1" in out


def test_display_missing_dir_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        display_sddp_info({"input_dir": tmp_path / "nope"})
