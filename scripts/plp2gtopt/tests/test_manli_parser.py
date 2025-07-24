"""Unit tests for ManliParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..manli_parser import ManliParser
from .conftest import get_example_file


@pytest.fixture
def sample_manli_file():
    """Fixture providing path to sample line maintenance file."""
    return get_example_file("plpmanli.dat")


def test_manli_parser_initialization():
    """Test ManliParser initialization."""
    test_path = "test.dat"
    parser = ManliParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.manlis
    assert parser.num_manlis == 0


def test_get_manlis(tmp_path):
    """Test get_manlis returns properly structured maintenance data."""
    # Create a temporary test file
    test_file = tmp_path / "test_manli.dat"
    test_file.write_text(
        """1
'test_line'
3
001 0.0 0.0 F
002 0.0 0.0 F
003 0.0 0.0 F"""
    )

    parser = ManliParser(str(test_file))
    parser.parse()

    manlis = parser.manlis
    assert len(manlis) == 1
    manli = manlis[0]

    # Verify structure and types
    assert manli["name"] == "test_line"
    assert isinstance(manli["block"], np.ndarray)
    assert isinstance(manli["tmax_ab"], np.ndarray)
    assert isinstance(manli["tmax_ba"], np.ndarray)
    assert isinstance(manli["operational"], np.ndarray)
    assert manli["block"].dtype == np.int16
    assert manli["tmax_ab"].dtype == np.float64
    assert manli["tmax_ba"].dtype == np.float64
    assert manli["operational"].dtype == np.int8

    # Verify array contents
    np.testing.assert_array_equal(manli["block"], [1, 2, 3])
    np.testing.assert_array_equal(manli["tmax_ab"], [0.0, 0.0, 0.0])
    np.testing.assert_array_equal(manli["tmax_ba"], [0.0, 0.0, 0.0])
    np.testing.assert_array_equal(manli["operational"], [False, False, False])


def test_parse_sample_file(sample_manli_file):
    """Test parsing of the sample maintenance file."""
    parser = ManliParser(str(sample_manli_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_manlis == 2
    manlis = parser.manlis
    assert len(manlis) == 2

    # Verify all lines have required fields
    for maint in manlis:
        assert isinstance(maint["name"], str)
        assert maint["name"] != ""
        assert isinstance(maint["block"], np.ndarray)
        assert isinstance(maint["tmax_ab"], np.ndarray)
        assert isinstance(maint["tmax_ba"], np.ndarray)
        assert isinstance(maint["operational"], np.ndarray)
        assert len(maint["block"]) > 0
        assert len(maint["block"]) == len(maint["tmax_ab"])
        assert len(maint["block"]) == len(maint["tmax_ba"])
        assert len(maint["block"]) == len(maint["operational"])

        # Verify array types and values
        assert maint["block"].dtype == np.int16
        assert maint["tmax_ab"].dtype == np.float64
        assert maint["tmax_ba"].dtype == np.float64
        assert maint["operational"].dtype == np.int8
        assert np.all(maint["block"] > 0)
        assert np.all(maint["tmax_ab"] >= 0)
        assert np.all(maint["tmax_ba"] >= 0)

    # Verify first line data
    maint1 = manlis[0]
    assert maint1["name"] == "Antofag110->Desalant110"
    assert len(maint1["block"]) == 5
    assert maint1["block"][0] == 1
    assert maint1["tmax_ab"][0] == 0.0
    assert maint1["operational"][0] == 0

    # Verify second line data
    maint2 = manlis[1]
    assert maint2["name"] == "Capricornio110->ElNegro110"
    assert len(maint2["block"]) == 3
    assert maint2["block"][0] == 1
    assert maint2["tmax_ab"][0] == 0.0
    # assert maint2["operational"][0] is False


def test_get_manli_by_name(sample_manli_file):
    """Test getting maintenance by line name."""
    parser = ManliParser(str(sample_manli_file))
    parser.parse()

    # Test existing line
    manlis = parser.manlis
    first_line = manlis[0]["name"]
    line_data = parser.get_manli_by_name(first_line)
    assert line_data is not None
    assert line_data["name"] == first_line
    assert len(line_data["block"]) > 0
    assert len(line_data["tmax_ab"]) > 0

    # Test non-existent line
    missing = parser.get_manli_by_name("NonExistentLine")
    assert missing is None


def test_parse_empty_file(tmp_path):
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()

    parser = ManliParser(str(empty_file))
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path):
    """Test handling of malformed maintenance file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'LINE'\n2\n001 0.0")  # Missing tmax_ba and operational

    parser = ManliParser(str(bad_file))
    with pytest.raises(ValueError):
        parser.parse()
