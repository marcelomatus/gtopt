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
    """Test parsing maintenance for a single battery."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text(
        "1\n"
        "BESS1\n"
        "2\n"
        "1  1  45.0  50.0\n"
        "1  2  30.0  40.0\n"
    )
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 1
    m = parser.manbats[0]
    assert m["name"] == "BESS1"
    assert len(m["stage"]) == 2
    np.testing.assert_array_almost_equal(m["pmax_charge"], [45.0, 30.0])
    np.testing.assert_array_almost_equal(m["pmax_discharge"], [50.0, 40.0])
    assert m["stage"][0] == 1
    assert m["stage"][1] == 2


def test_parse_multiple_batteries(tmp_path):
    """Test parsing multiple batteries."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text(
        "2\n"
        "BAT_A\n"
        "1\n"
        "1  1  40.0  45.0\n"
        "BAT_B\n"
        "2\n"
        "1  1  20.0  25.0\n"
        "1  2  15.0  20.0\n"
    )
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 2
    assert parser.manbats[0]["name"] == "BAT_A"
    assert parser.manbats[1]["name"] == "BAT_B"
    np.testing.assert_array_almost_equal(parser.manbats[1]["pmax_charge"], [20.0, 15.0])


def test_get_manbat_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpmanbat.dat"
    f.write_text(
        "1\n"
        "MyBat\n"
        "1\n"
        "1  1  30.0  35.0\n"
    )
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
        "# stages\n"
        "1\n"
        "# Mes Etapa PMaxC PMaxD\n"
        "1  3  50.0  55.0\n"
    )
    parser = ManbatParser(f)
    parser.parse()

    assert parser.num_manbats == 1
    assert parser.manbats[0]["name"] == "BESS_Test"
    assert parser.manbats[0]["stage"][0] == 3


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
