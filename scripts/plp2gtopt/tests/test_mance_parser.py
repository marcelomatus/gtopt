"""Unit tests for ManceParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..mance_parser import ManceParser
from .conftest import get_example_file


@pytest.fixture
def sample_mance_file():
    """Fixture providing path to sample maintenance file."""
    return get_example_file("plpmance.dat")


def test_mance_parser_initialization():
    """Test ManceParser initialization."""
    test_path = "test.dat"
    parser = ManceParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.mances
    assert parser.num_mances == 0


def test_get_mances(tmp_path):
    """Test get_mances returns properly structured maintenance data."""
    # Create a temporary test file
    test_file = tmp_path / "test_mance.dat"
    test_file.write_text(
        """1
'test'
2
03 001 1 5.0 69.75
03 002 1 5.0 69.75"""
    )

    parser = ManceParser(str(test_file))
    parser.parse()

    mances = parser.mances
    assert len(mances) == 1
    mance = mances[0]

    # Verify structure and types
    assert mance["name"] == "test"
    assert isinstance(mance["block"], np.ndarray)
    assert isinstance(mance["pmin"], np.ndarray)
    assert isinstance(mance["pmax"], np.ndarray)
    assert mance["block"].dtype == np.int16
    assert mance["pmin"].dtype == np.float64
    assert mance["pmax"].dtype == np.float64

    # Verify array contents
    np.testing.assert_array_equal(mance["block"], [1, 2])
    np.testing.assert_array_equal(mance["pmin"], [5.0, 5.0])
    np.testing.assert_array_equal(mance["pmax"], [69.75, 69.75])


def test_parse_sample_file(sample_mance_file):
    """Test parsing of the sample maintenance file."""
    parser = ManceParser(str(sample_mance_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_mances == 2
    mances = parser.mances
    assert len(mances) == 2

    # Verify all centrals have required fields
    for maint in mances:
        assert isinstance(maint["name"], str)
        assert maint["name"] != ""
        assert isinstance(maint["block"], np.ndarray)
        assert isinstance(maint["pmin"], np.ndarray)
        assert isinstance(maint["pmax"], np.ndarray)
        assert len(maint["block"]) > 0
        assert len(maint["block"]) == len(maint["pmin"])
        assert len(maint["block"]) == len(maint["pmax"])

        # Verify array types and values
        assert maint["block"].dtype == np.int16
        assert maint["pmin"].dtype == np.float64
        assert maint["pmax"].dtype == np.float64
        assert np.all(maint["block"] > 0)
        assert np.all(maint["pmin"] >= 0)
        assert np.all(maint["pmax"] >= 0)

    # Verify first central data
    maint1 = mances[0]
    assert maint1["name"] == "ABANICO"
    assert len(maint1["block"]) == 4
    assert maint1["block"][0] == 1
    assert maint1["pmin"][0] == 5.0
    assert maint1["pmax"][0] == 69.75

    # Verify second central data
    maint2 = mances[1]
    assert maint2["name"] == "ABASTIBLE_CONCON_FV"
    assert len(maint2["block"]) == 2
    assert maint2["block"][0] == 1
    assert maint2["pmin"][0] == 0.0
    assert maint2["pmax"][0] == 0.0


def test_real_file_parsing():
    """Test parsing of the real plpmance.dat file."""
    real_file = (
        Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpmance.dat"
    )
    parser = ManceParser(str(real_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_mances == 2
    mances = parser.mances
    assert len(mances) == 2

    # Verify first central data
    maint1 = mances[0]
    assert maint1["name"] == "ABANICO"
    assert len(maint1["block"]) == 4
    assert maint1["block"][0] == 1
    assert maint1["pmin"][0] == pytest.approx(5.0)
    assert maint1["pmax"][0] == pytest.approx(69.75)

    # Verify all blocks for ABANICO
    assert maint1["block"].tolist() == [1, 2, 3, 4]
    assert maint1["pmin"].tolist() == [5.0, 5.0, 5.0, 5.0]
    assert maint1["pmax"].tolist() == [69.75, 69.75, 69.75, 69.75]

    # Verify second central data
    maint2 = mances[1]
    assert maint2["name"] == "ABASTIBLE_CONCON_FV"
    assert len(maint2["block"]) == 2
    assert maint2["block"][0] == 1
    assert maint2["pmin"][0] == pytest.approx(0.0)
    assert maint2["pmax"][0] == pytest.approx(0.0)

    # Verify all blocks for ABASTIBLE_CONCON_FV
    assert maint2["block"].tolist() == [1, 2]
    assert maint2["pmin"].tolist() == [0.0, 0.0]
    assert maint2["pmax"].tolist() == [0.0, 0.0]


def test_parse_with_whitespace_variations(tmp_path):
    """Test parsing handles different whitespace formats."""
    test_file = tmp_path / "test_mance.dat"
    test_file.write_text(
        """2
'CENTRAL1'
4                 01
03      001        1     5.00    69.75
03      002        1     5.00    69.75
03      003        1     5.00    69.75
03      004        1     5.00    69.75
'CENTRAL2'
2                 01
03      001        1     0.00     0.00
03      002        1     0.00     0.00"""
    )

    parser = ManceParser(str(test_file))
    parser.parse()

    assert parser.num_mances == 2
    assert len(parser.mances[0]["block"]) == 4
    assert len(parser.mances[1]["block"]) == 2


def test_parse_with_comments(tmp_path):
    """Test parsing handles commented lines."""
    test_file = tmp_path / "test_mance.dat"
    test_file.write_text(
        """# Header comment
2
# Central comment
'CENTRAL1'
# Block count comment
4                 01
# Data comment
03      001        1     5.00    69.75
03      002        1     5.00    69.75
03      003        1     5.00    69.75
03      004        1     5.00    69.75
'CENTRAL2'
2                 01
03      001        1     0.00     0.00
03      002        1     0.00     0.00"""
    )

    parser = ManceParser(str(test_file))
    parser.parse()

    assert parser.num_mances == 2
    assert parser.mances[0]["name"] == "CENTRAL1"
    assert parser.mances[1]["name"] == "CENTRAL2"


def test_get_mance_by_name(sample_mance_file):
    """Test getting maintenance by central name."""
    parser = ManceParser(str(sample_mance_file))
    parser.parse()

    # Test existing central
    mances = parser.mances
    first_cen = mances[0]["name"]
    cen_data = parser.get_mance_by_name(first_cen)
    assert cen_data is not None
    assert cen_data["name"] == first_cen
    assert len(cen_data["block"]) > 0
    assert len(cen_data["pmin"]) > 0
    assert len(cen_data["pmax"]) > 0

    # Test another existing central if available
    if len(mances) > 1:
        second_cen = mances[1]["name"]
        cen_data = parser.get_mance_by_name(second_cen)
        assert cen_data is not None
        assert cen_data["name"] == second_cen
        assert len(cen_data["block"]) > 0
        assert len(cen_data["pmin"]) > 0
        assert len(cen_data["pmax"]) > 0

    # Test non-existent central
    missing = parser.get_mance_by_name("NonExistentCen")
    assert missing is None


def test_parse_empty_file(tmp_path):
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()

    parser = ManceParser(str(empty_file))
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path):
    """Test handling of malformed maintenance file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'CENTRAL'\n2\n03 001 1 5.0")  # Missing p_max

    parser = ManceParser(str(bad_file))
    with pytest.raises(ValueError):
        parser.parse()
