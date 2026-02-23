"""Unit tests for ManbessParser class."""

from pathlib import Path
import pytest
import numpy as np

from ..manbess_parser import ManbessParser


def test_manbess_parser_initialization():
    """Test ManbessParser initialization."""
    parser = ManbessParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.manbesses
    assert parser.num_manbesses == 0


def test_parse_zero_manbesses(tmp_path):
    """Test parsing a file with 0 BESS maintenance entries."""
    f = tmp_path / "plpmanbess.dat"
    f.write_text("# Numero de BESS con mantenimiento\n 0\n")
    parser = ManbessParser(f)
    parser.parse()
    assert parser.num_manbesses == 0
    assert not parser.manbesses


def test_parse_single_manbess(tmp_path):
    """Test parsing a single BESS maintenance block."""
    f = tmp_path / "plpmanbess.dat"
    f.write_text(
        "# Header\n"
        " 1\n"
        "'BESS1'\n"
        "  2\n"
        "   01    001    0.0    0.0\n"
        "   02    002   50.0   50.0\n"
    )
    parser = ManbessParser(f)
    parser.parse()

    assert parser.num_manbesses == 1
    m = parser.manbesses[0]
    assert m["name"] == "BESS1"
    assert isinstance(m["stage"], np.ndarray)
    assert isinstance(m["pmax_charge"], np.ndarray)
    assert isinstance(m["pmax_discharge"], np.ndarray)
    np.testing.assert_array_equal(m["stage"], [1, 2])
    np.testing.assert_array_almost_equal(m["pmax_charge"], [0.0, 50.0])
    np.testing.assert_array_almost_equal(m["pmax_discharge"], [0.0, 50.0])


def test_parse_multiple_manbesses(tmp_path):
    """Test parsing multiple BESS maintenance blocks."""
    f = tmp_path / "plpmanbess.dat"
    f.write_text(
        " 2\n"
        "'BESS1'\n"
        "  1\n"
        "   01    001    0.0    0.0\n"
        "'BESS2'\n"
        "  3\n"
        "   01    001   10.0   10.0\n"
        "   02    002   20.0   20.0\n"
        "   03    003   30.0   30.0\n"
    )
    parser = ManbessParser(f)
    parser.parse()

    assert parser.num_manbesses == 2
    m1 = parser.manbesses[0]
    m2 = parser.manbesses[1]

    assert m1["name"] == "BESS1"
    assert len(m1["stage"]) == 1

    assert m2["name"] == "BESS2"
    assert len(m2["stage"]) == 3
    np.testing.assert_array_equal(m2["stage"], [1, 2, 3])
    np.testing.assert_array_almost_equal(m2["pmax_charge"], [10.0, 20.0, 30.0])
    np.testing.assert_array_almost_equal(m2["pmax_discharge"], [10.0, 20.0, 30.0])


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpmanbess.dat"
    f.write_text(
        "# Top comment\n"
        " 1\n"
        "# BESS name\n"
        "'MyBESS'\n"
        "# Stage count\n"
        "  2\n"
        "# Data\n"
        "   01    001    5.0    5.0\n"
        "   01    002    5.0    5.0\n"
    )
    parser = ManbessParser(f)
    parser.parse()
    assert parser.num_manbesses == 1
    assert parser.manbesses[0]["name"] == "MyBESS"


def test_get_manbess_by_name(tmp_path):
    """Test lookup by BESS name."""
    f = tmp_path / "plpmanbess.dat"
    f.write_text(" 1\n'Alpha'\n  1\n   01    001   25.0   25.0\n")
    parser = ManbessParser(f)
    parser.parse()
    manbess_alpha = parser.get_manbess_by_name("Alpha")
    assert manbess_alpha is not None
    assert manbess_alpha["name"] == "Alpha"
    assert parser.get_manbess_by_name("NoSuch") is None


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpmanbess.dat"
    f.touch()
    parser = ManbessParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_entry_raises(tmp_path):
    """Test that a malformed maintenance entry raises ValueError."""
    f = tmp_path / "plpmanbess.dat"
    # Only 3 fields (need 4: Mes Etapa PMaxC PMaxD)
    f.write_text(" 1\n'BESS1'\n  1\n   01    001    0.0\n")
    parser = ManbessParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError on parse."""
    parser = ManbessParser("/nonexistent/plpmanbess.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
