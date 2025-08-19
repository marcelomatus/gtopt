"""Unit tests for ManemParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..manem_parser import ManemParser
from .conftest import get_example_file


@pytest.fixture
def sample_manem_file():
    """Fixture providing path to sample reservoir maintenance file."""
    return get_example_file("plpmanem.dat")


def test_manem_parser_initialization():
    """Test ManemParser initialization."""
    test_path = "test.dat"
    parser = ManemParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.manems
    assert parser.num_manems == 0


def test_get_manems(tmp_path):
    """Test get_manems returns properly structured maintenance data."""
    # Create a temporary test file
    test_file = tmp_path / "test_manem.dat"
    test_file.write_text(
        """1
'test'
2
03 001 0.5 6.75
03 002 0.5 6.75"""
    )

    parser = ManemParser(str(test_file))
    parser.parse()

    manems = parser.manems
    assert len(manems) == 1
    manem = manems[0]

    # Verify structure and types
    assert manem["name"] == "test"
    assert isinstance(manem["block"], np.ndarray)
    assert isinstance(manem["volmin"], np.ndarray)
    assert isinstance(manem["volmax"], np.ndarray)
    assert manem["block"].dtype == np.int32
    assert manem["volmin"].dtype == np.float64
    assert manem["volmax"].dtype == np.float64

    # Verify array contents
    np.testing.assert_array_equal(manem["block"], [1, 2])
    np.testing.assert_array_equal(manem["volmin"], [0.5, 0.5])
    np.testing.assert_array_equal(manem["volmax"], [6.75, 6.75])


def test_parse_sample_file(sample_manem_file):
    """Test parsing of the sample reservoir maintenance file."""
    parser = ManemParser(str(sample_manem_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_manems == 2
    manems = parser.manems
    assert len(manems) == 2

    # Verify all reservoirs have required fields
    for maint in manems:
        assert isinstance(maint["name"], str)
        assert maint["name"] != ""
        assert isinstance(maint["block"], np.ndarray)
        assert isinstance(maint["volmin"], np.ndarray)
        assert isinstance(maint["volmax"], np.ndarray)
        assert len(maint["block"]) > 0
        assert len(maint["block"]) == len(maint["volmin"])
        assert len(maint["block"]) == len(maint["volmax"])

        # Verify array types and values
        assert maint["block"].dtype == np.int32
        assert maint["volmin"].dtype == np.float64
        assert maint["volmax"].dtype == np.float64
        assert np.all(maint["block"] > 0)
        assert np.all(maint["volmin"] >= 0)
        assert np.all(maint["volmax"] >= 0)

    # Verify first reservoir data
    maint1 = manems[0]
    assert maint1["name"] == "COLBUN"
    assert len(maint1["block"]) == 5
    assert maint1["block"][0] == 1
    assert maint1["volmin"][0] == 0.3816243
    assert maint1["volmax"][0] == 1.5043824

    # Verify second reservoir data
    maint2 = manems[1]
    assert maint2["name"] == "RAPEL"
    assert len(maint2["block"]) == 3
    assert maint2["block"][0] == 1
    assert maint2["volmin"][0] == 0.2723049
    assert maint2["volmax"][0] == 0.5632124


def test_real_file_parsing():
    """Test parsing of the real plpmanem.dat file."""
    real_file = (
        Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpmanem.dat"
    )
    parser = ManemParser(str(real_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_manems == 2
    manems = parser.manems
    assert len(manems) == 2

    # Verify first reservoir data
    maint1 = manems[0]
    assert maint1["name"] == "COLBUN"
    assert len(maint1["block"]) == 5
    assert maint1["block"][0] == 1
    assert maint1["volmin"][0] == pytest.approx(0.3816243)
    assert maint1["volmax"][0] == pytest.approx(1.5043824)

    # Verify all blocks for COLBUN
    assert maint1["block"].tolist() == [1, 2, 3, 4, 5]
    assert maint1["volmin"].tolist() == [0.3816243] * 5
    assert maint1["volmax"].tolist() == [1.5043824] * 5

    # Verify second reservoir data
    maint2 = manems[1]
    assert maint2["name"] == "RAPEL"
    assert len(maint2["block"]) == 3
    assert maint2["block"][0] == 1
    assert maint2["volmin"][0] == pytest.approx(0.2723049)
    assert maint2["volmax"][0] == pytest.approx(0.5632124)

    # Verify all blocks for RAPEL
    assert maint2["block"].tolist() == [1, 2, 3]
    assert maint2["volmin"].tolist() == [0.2723049] * 3
    assert maint2["volmax"].tolist() == [0.5632124] * 3


def test_parse_with_whitespace_variations(tmp_path):
    """Test parsing handles different whitespace formats."""
    test_file = tmp_path / "test_manem.dat"
    test_file.write_text(
        """2
'RESERVOIR1'
4
03      001        0.5    6.75
03      002        0.5    6.75
03      003        0.5    6.75
03      004        0.5    6.75
'RESERVOIR2'
2
03      001        0.2     0.5
03      002        0.2     0.5"""
    )

    parser = ManemParser(str(test_file))
    parser.parse()

    assert parser.num_manems == 2
    assert len(parser.manems[0]["block"]) == 4
    assert len(parser.manems[1]["block"]) == 2


def test_parse_with_comments(tmp_path):
    """Test parsing handles commented lines."""
    test_file = tmp_path / "test_manem.dat"
    test_file.write_text(
        """# Header comment
2
# Reservoir comment
'RESERVOIR1'
# Block count comment
4
# Data comment
03      001        0.5    6.75
03      002        0.5    6.75
03      003        0.5    6.75
03      004        0.5    6.75
'RESERVOIR2'
2
03      001        0.2    0.5
03      002        0.2    0.5"""
    )

    parser = ManemParser(str(test_file))
    parser.parse()

    assert parser.num_manems == 2
    assert parser.manems[0]["name"] == "RESERVOIR1"
    assert parser.manems[1]["name"] == "RESERVOIR2"


def test_get_manem_by_name(sample_manem_file):
    """Test getting maintenance by reservoir name."""
    parser = ManemParser(str(sample_manem_file))
    parser.parse()

    # Test existing reservoir
    manems = parser.manems
    first_res = manems[0]["name"]
    res_data = parser.get_manem_by_name(first_res)
    assert res_data is not None
    assert res_data["name"] == first_res
    assert len(res_data["block"]) > 0
    assert len(res_data["volmin"]) > 0
    assert len(res_data["volmax"]) > 0

    # Test another existing reservoir if available
    if len(manems) > 1:
        second_res = manems[1]["name"]
        res_data = parser.get_manem_by_name(second_res)
        assert res_data is not None
        assert res_data["name"] == second_res
        assert len(res_data["block"]) > 0
        assert len(res_data["volmin"]) > 0
        assert len(res_data["volmax"]) > 0

    # Test non-existent reservoir
    missing = parser.get_manem_by_name("NonExistentRes")
    assert missing is None


def test_parse_empty_file(tmp_path):
    """Test handling of empty input file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.touch()

    parser = ManemParser(str(empty_file))
    with pytest.raises(ValueError):
        parser.parse()


def test_parse_malformed_file(tmp_path):
    """Test handling of malformed maintenance file."""
    bad_file = tmp_path / "bad.dat"
    bad_file.write_text("1\n'RESERVOIR'\n2\n03 001 0.5")  # Missing vol_max

    parser = ManemParser(str(bad_file))
    with pytest.raises(ValueError):
        parser.parse()
