# SPDX-License-Identifier: BSD-3-Clause
"""Tests for case type detection."""

from pathlib import Path

from run_gtopt._detect import CaseType, detect_case_type, infer_gtopt_dir


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("")


def test_detect_plp_case(tmp_path: Path):
    """Directory with plpblo.dat + plpbar.dat is a PLP case."""
    _touch(tmp_path / "plpblo.dat")
    _touch(tmp_path / "plpbar.dat")
    assert detect_case_type(tmp_path) == CaseType.PLP


def test_detect_plp_needs_indicator(tmp_path: Path):
    """plpblo.dat alone is not enough — need at least one indicator."""
    _touch(tmp_path / "plpblo.dat")
    assert detect_case_type(tmp_path) != CaseType.PLP


def test_detect_gtopt_case(tmp_path: Path):
    """Directory with dir_name.json is a gtopt case."""
    case_dir = tmp_path / "my_case"
    case_dir.mkdir()
    _touch(case_dir / "my_case.json")
    assert detect_case_type(case_dir) == CaseType.GTOPT


def test_detect_passthrough_file(tmp_path: Path):
    """A regular file is passthrough."""
    f = tmp_path / "plan.json"
    f.write_text("{}")
    assert detect_case_type(f) == CaseType.PASSTHROUGH


def test_detect_passthrough_empty_dir(tmp_path: Path):
    """An empty directory is passthrough."""
    assert detect_case_type(tmp_path) == CaseType.PASSTHROUGH


def test_detect_nonexistent():
    """Non-existent path is passthrough."""
    assert detect_case_type(Path("/does/not/exist")) == CaseType.PASSTHROUGH


def test_infer_gtopt_dir_plp_prefix():
    """plp_case_2y → gtopt_case_2y."""
    assert infer_gtopt_dir(Path("plp_case_2y")) == Path("gtopt_case_2y")


def test_infer_gtopt_dir_plp_prefix_nested():
    """/data/plp_foo → /data/gtopt_foo."""
    result = infer_gtopt_dir(Path("/data/plp_foo"))
    assert result == Path("/data/gtopt_foo")


def test_infer_gtopt_dir_no_prefix():
    """my_case → gtopt_my_case."""
    assert infer_gtopt_dir(Path("my_case")) == Path("gtopt_my_case")
