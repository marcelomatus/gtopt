"""Unit tests for ManbatParser class."""

from pathlib import Path
import pytest
import numpy as np

from ..manbat_parser import ManbatParser


def test_manbat_parser_initialization():
    """Test ManbatParser initialization."""
    parser = ManbatParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.manbats
    assert parser.num_manbats == 0


def test_parse_zero_batteries(tmp_path):
    """Test parsing a file with 0 battery maintenance entries."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text("0\n")
    parser = ManbatParser(f)
    parser.parse()
    assert parser.num_manbats == 0


def test_parse_single_battery_maintenance(tmp_path):
    """Test parsing maintenance for a single battery.

    Fortran LeeManBat reads: IBind EMin EMax  (3 fields per line).
    """
    f = tmp_path / "plpmanbat.dat"
    f.write_text("1\nBESS1\n2\n1  0.0  180.0\n2  10.0  150.0\n")
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 1
    m = parser.manbats[0]
    assert m["name"] == "BESS1"
    assert len(m["block_index"]) == 2
    np.testing.assert_array_equal(m["block_index"], [1, 2])
    np.testing.assert_array_almost_equal(m["emin"], [0.0, 10.0])
    np.testing.assert_array_almost_equal(m["emax"], [180.0, 150.0])


def test_parse_multiple_batteries(tmp_path):
    """Test parsing multiple batteries."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text(
        "2\nBAT_A\n1\n5  0.0  100.0\n"
        "BAT_B\n2\n3  0.0  200.0\n4  50.0  200.0\n"
    )
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 2
    assert parser.manbats[0]["name"] == "BAT_A"
    assert parser.manbats[1]["name"] == "BAT_B"
    np.testing.assert_array_almost_equal(parser.manbats[1]["emin"], [0.0, 50.0])
    np.testing.assert_array_almost_equal(parser.manbats[1]["emax"], [200.0, 200.0])


def test_get_manbat_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text("1\nMyBat\n1\n1  0.0  100.0\n")
    parser = ManbatParser(f)
    parser.parse()

    m = parser.get_manbat_by_name("MyBat")
    assert m is not None
    assert m["name"] == "MyBat"
    assert parser.get_manbat_by_name("Other") is None


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text(
        "# Battery maintenance\n"
        "1\n"
        "# battery name\n"
        "BESS_Test\n"
        "# num blocks\n"
        "1\n"
        "# IBind EMin EMax\n"
        "3  0.0  120.0\n"
    )
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 1
    assert parser.manbats[0]["name"] == "BESS_Test"
    assert parser.manbats[0]["block_index"][0] == 3


def test_parse_negative_keeps_default(tmp_path):
    """Fortran uses -1 to mean 'keep default'. Parser stores the value as-is."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text("1\nBAT1\n1\n5  -1.0  100.0\n")
    parser = ManbatParser(f)
    parser.parse()

    m = parser.manbats[0]
    np.testing.assert_array_almost_equal(m["emin"], [-1.0])
    np.testing.assert_array_almost_equal(m["emax"], [100.0])


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpmanbat.dat"
    f.touch()
    parser = ManbatParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError."""
    parser = ManbatParser("/nonexistent/plpmanbat.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
