"""Unit tests for ExtracParser class."""

from pathlib import Path
import pytest
from ..extrac_parser import ExtracParser
from .conftest import get_example_file


@pytest.fixture
def sample_extrac_file():
    """Fixture providing path to sample extraction file."""
    return get_example_file("plpextrac.dat")


def test_extrac_parser_initialization():
    """Test ExtracParser initialization."""
    test_path = "test.dat"
    parser = ExtracParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.extracs
    assert parser.num_extracs == 0


def test_parse_sample_file(sample_extrac_file):
    """Test parsing of the sample extraction file."""
    parser = ExtracParser(str(sample_extrac_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_extracs == 3
    extracs = parser.extracs
    assert len(extracs) == 3

    # Verify all extractions have required fields
    for extrac in extracs:
        assert isinstance(extrac["name"], str)
        assert extrac["name"] != ""
        assert isinstance(extrac["max_extrac"], float)
        assert isinstance(extrac["downstream"], str)
        assert extrac["downstream"] != ""

    # Verify first extraction data
    extrac1 = extracs[0]
    assert extrac1["name"] == "COLBUN"
    assert extrac1["max_extrac"] == 24.0
    assert extrac1["downstream"] == "CHIBURGO"

    # Verify second extraction data
    extrac2 = extracs[1]
    assert extrac2["name"] == "COLBUN"
    assert extrac2["max_extrac"] == 50.0
    assert extrac2["downstream"] == "BPretilCol"

    # Verify third extraction data
    extrac3 = extracs[2]
    assert extrac3["name"] == "RALCO"
    assert extrac3["max_extrac"] == 27.1
    assert extrac3["downstream"] == "PALMUCHO"


def test_get_extrac_by_name(sample_extrac_file):
    """Test getting extraction by central name."""
    parser = ExtracParser(str(sample_extrac_file))
    parser.parse()

    # Test existing central
    extrac_data = parser.get_extrac_by_name("COLBUN")
    assert extrac_data is not None
    assert extrac_data["name"] == "COLBUN"
    assert isinstance(extrac_data["max_extrac"], float)
    assert isinstance(extrac_data["downstream"], str)

    # Test non-existent central
    missing = parser.get_extrac_by_name("NonExistentCen")
    assert missing is None


def test_parse_empty_file(tmp_path):
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()

    parser = ExtracParser(str(empty_file))
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path):
    """Test handling of malformed extraction file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'CENTRAL'\n24.0")  # Missing downstream

    parser = ExtracParser(str(bad_file))
    with pytest.raises(IndexError):
        parser.parse()
