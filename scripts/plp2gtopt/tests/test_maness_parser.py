"""Unit tests for ManessParser class."""

from pathlib import Path

import numpy as np
import pytest

from ..maness_parser import ManessParser


def test_maness_parser_initialization():
    """Test ManessParser initialization."""
    parser = ManessParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.manesses
    assert parser.num_manesses == 0


def test_parse_zero_manesses(tmp_path):
    """Test parsing a file with 0 ESS maintenance entries."""
    f = tmp_path / "plpmaness.dat"
    f.write_text("# Numero de ESS con mantenimiento\n 0\n")
    parser = ManessParser(f)
    parser.parse()
    assert parser.num_manesses == 0
    assert not parser.manesses


def test_parse_single_maness(tmp_path):
    """Test parsing a single ESS maintenance block.

    Fortran LeeManEss reads: IBind Emin Emax DCMin DCMax [DCMod]  (5-6 fields).
    """
    f = tmp_path / "plpmaness.dat"
    f.write_text(
        "# Header\n"
        " 1\n"
        "'ESS1'\n"
        "  2\n"
        "   1    0.0   200.0   0.0   50.0   1\n"
        "   2    0.0   180.0   0.0   45.0   1\n"
    )
    parser = ManessParser(f)
    parser.parse()

    assert parser.num_manesses == 1
    m = parser.manesses[0]
    assert m["name"] == "ESS1"
    assert isinstance(m["block_index"], np.ndarray)
    assert isinstance(m["emin"], np.ndarray)
    assert isinstance(m["emax"], np.ndarray)
    assert isinstance(m["dcmin"], np.ndarray)
    assert isinstance(m["dcmax"], np.ndarray)
    assert isinstance(m["dcmod"], np.ndarray)
    np.testing.assert_array_equal(m["block_index"], [1, 2])
    np.testing.assert_array_almost_equal(m["emin"], [0.0, 0.0])
    np.testing.assert_array_almost_equal(m["emax"], [200.0, 180.0])
    np.testing.assert_array_almost_equal(m["dcmin"], [0.0, 0.0])
    np.testing.assert_array_almost_equal(m["dcmax"], [50.0, 45.0])
    np.testing.assert_array_equal(m["dcmod"], [1, 1])


def test_parse_multiple_manesses(tmp_path):
    """Test parsing multiple ESS maintenance blocks."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(
        " 2\n"
        "'ESS1'\n"
        "  1\n"
        "   1    0.0   100.0   0.0   30.0   0\n"
        "'ESS2'\n"
        "  3\n"
        "   1    0.0   200.0   0.0   50.0   1\n"
        "   2    0.0   180.0   0.0   40.0   1\n"
        "   3   10.0   150.0   5.0   35.0   1\n"
    )
    parser = ManessParser(f)
    parser.parse()

    assert parser.num_manesses == 2
    m2 = parser.manesses[1]
    assert m2["name"] == "ESS2"
    assert len(m2["block_index"]) == 3
    np.testing.assert_array_equal(m2["block_index"], [1, 2, 3])
    np.testing.assert_array_almost_equal(m2["emax"], [200.0, 180.0, 150.0])
    np.testing.assert_array_almost_equal(m2["dcmax"], [50.0, 40.0, 35.0])


def test_parse_skip_zero_blocks(tmp_path):
    """Test that entries with num_blocks <= 0 are skipped."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(
        " 2\n"
        "'ESS_SKIP'\n"
        "  0\n"
        "'ESS_OK'\n"
        "  1\n"
        "   1    0.0   100.0   0.0   30.0   1\n"
    )
    parser = ManessParser(f)
    parser.parse()
    assert parser.num_manesses == 1
    assert parser.manesses[0]["name"] == "ESS_OK"


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(
        "# Top comment\n"
        " 1\n"
        "# ESS name\n"
        "'MyESS'\n"
        "# Block count\n"
        "  2\n"
        "# Data\n"
        "   1    0.0   100.0   0.0   25.0   1\n"
        "   2    0.0   100.0   0.0   25.0   1\n"
    )
    parser = ManessParser(f)
    parser.parse()
    assert parser.num_manesses == 1
    assert parser.manesses[0]["name"] == "MyESS"


def test_parse_without_dcmod(tmp_path):
    """Test that DCMod is optional (defaults to 1 like Fortran fallback)."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(
        " 1\n"
        "'ESS_NOMOD'\n"
        "  1\n"
        "   5    0.0   200.0   0.0   60.0\n"
    )
    parser = ManessParser(f)
    parser.parse()
    m = parser.manesses[0]
    np.testing.assert_array_equal(m["dcmod"], [1])  # Fortran default


def test_get_maness_by_name(tmp_path):
    """Test lookup by ESS name."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(" 1\n'Alpha'\n  1\n   1    0.0   100.0   0.0   25.0   0\n")
    parser = ManessParser(f)
    parser.parse()
    m = parser.get_maness_by_name("Alpha")
    assert m is not None
    assert m["name"] == "Alpha"
    assert parser.get_maness_by_name("NoSuch") is None


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpmaness.dat"
    f.touch()
    parser = ManessParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_entry_raises(tmp_path):
    """Test that a line with fewer than 5 fields raises ValueError."""
    f = tmp_path / "plpmaness.dat"
    f.write_text(" 1\n'ESS1'\n  1\n   1    0.0   100.0   0.0\n")
    parser = ManessParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError on parse."""
    parser = ManessParser("/nonexistent/plpmaness.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
