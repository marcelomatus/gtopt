"""Unit tests for CostParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..cost_parser import CostParser
from .conftest import get_example_file


@pytest.fixture
def sample_costs_file():
    """Fixture providing path to sample cost file."""
    return get_example_file("plpcosce.dat")


def test_costs_parser_initialization():
    """Test CostParser initialization."""
    test_path = "test.dat"
    parser = CostParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.costs
    assert parser.num_costs == 0


def test_get_costs(tmp_path):
    """Test get_costs returns properly structured cost data."""
    # Create a temporary test file
    test_file = tmp_path / "test_cost.dat"
    test_file.write_text(
        """1
'test'
2
04 004 157.9
04 005 157.9"""
    )

    parser = CostParser(str(test_file))
    parser.parse()

    costs = parser.costs
    assert len(costs) == 1
    cost = costs[0]

    # Verify structure and types
    assert cost["name"] == "test"
    assert isinstance(cost["stages"], np.ndarray)
    assert isinstance(cost["costs"], np.ndarray)
    assert cost["stages"].dtype == np.int32
    assert cost["costs"].dtype == np.float64

    # Verify array contents
    np.testing.assert_array_equal(cost["stages"], [4, 5])
    np.testing.assert_array_equal(cost["costs"], [157.9, 157.9])


def test_parse_sample_file(sample_costs_file):
    """Test parsing of the sample cost file."""
    parser = CostParser(str(sample_costs_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_costs == 2
    costs = parser.costs
    assert len(costs) == 2

    # Verify all generators have required fields
    for cen_cost in costs:
        assert isinstance(cen_cost["name"], str)
        assert cen_cost["name"] != ""
        assert isinstance(cen_cost["stages"], np.ndarray)
        assert isinstance(cen_cost["costs"], np.ndarray)
        assert len(cen_cost["stages"]) > 0
        assert len(cen_cost["stages"]) == len(cen_cost["costs"])

        # Verify array types and values
        assert cen_cost["stages"].dtype == np.int32
        assert cen_cost["costs"].dtype == np.float64
        assert np.all(cen_cost["stages"] > 0)
        assert np.all(cen_cost["costs"] > 0)

    # Verify first central data
    cen1 = costs[0]
    assert cen1["name"] == "CMPC_PACIFICO_BL3"
    assert len(cen1["stages"]) == 4
    assert cen1["stages"][0] == 4
    assert cen1["costs"][0] == 157.9

    # Verify second central data
    cen2 = costs[1]
    assert cen2["name"] == "ANDINA"
    assert len(cen2["stages"]) == 3
    assert cen2["stages"][0] == 5
    assert cen2["costs"][0] == 67.2


def test_real_file_parsing():
    """Test parsing of the real plpcosce.dat file."""
    real_file = (
        Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpcosce.dat"
    )
    parser = CostParser(str(real_file))
    parser.parse()

    # Verify basic structure
    assert parser.num_costs == 2
    costs = parser.costs
    assert len(costs) == 2

    # Verify first central data
    cen1 = costs[0]
    assert cen1["name"] == "CMPC_PACIFICO_BL3"
    assert len(cen1["stages"]) == 4
    assert cen1["stages"][0] == 4
    assert cen1["costs"][0] == pytest.approx(157.9)

    # Verify second central data
    cen2 = costs[1]
    assert cen2["name"] == "ANDINA"
    assert len(cen2["stages"]) == 3
    assert cen2["stages"][0] == 5
    assert cen2["costs"][0] == pytest.approx(67.2)


def test_get_costs_by_name(sample_costs_file):
    """Test getting costs by central name."""
    parser = CostParser(str(sample_costs_file))
    parser.parse()

    # Test existing central
    costs = parser.costs
    first_cen = costs[0]["name"]
    cen_data = parser.get_cost_by_name(first_cen)
    assert cen_data is not None
    assert cen_data["name"] == first_cen
    assert len(cen_data["stages"]) > 0
    assert len(cen_data["costs"]) > 0

    # Test another existing central if available
    if len(costs) > 1:
        second_cen = costs[1]["name"]
        cen_data = parser.get_cost_by_name(second_cen)
        assert cen_data is not None
        assert cen_data["name"] == second_cen
        assert len(cen_data["stages"]) > 0
        assert len(cen_data["costs"]) > 0

    # Test non-existent central
    missing = parser.get_cost_by_name("NonExistentCen")
    assert missing is None
