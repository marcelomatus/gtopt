"""Unit tests for CostsParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..costs_parser import CostsParser
from .conftest import get_example_file


@pytest.fixture
def sample_costs_file():
    """Fixture providing path to sample cost file."""
    return get_example_file("plpcosce.dat")


def test_costs_parser_initialization():
    """Test CostsParser initialization."""
    test_path = "test.dat"
    parser = CostsParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.get_costs()
    assert parser.num_generators == 0


def test_get_num_generators():
    """Test get_num_generators returns correct value."""
    parser = CostsParser("test.dat")
    parser.num_generators = 3
    assert parser.get_num_generators() == 3


def test_get_costs():
    """Test get_costs returns properly structured cost data."""
    parser = CostsParser("test.dat")
    # Setup test data
    test_stages = np.array([4, 5], dtype=np.int32)
    test_costs = np.array([157.9, 157.9], dtype=np.float64)

    parser._data = [{"name": "test", "stages": test_stages, "costs": test_costs}]

    costs = parser.get_costs()
    assert len(costs) == 1
    cost = costs[0]

    # Verify structure and types
    assert cost["name"] == "test"
    assert isinstance(cost["stages"], np.ndarray)
    assert isinstance(cost["costs"], np.ndarray)
    assert cost["stages"].dtype == np.int32
    assert cost["costs"].dtype == np.float64

    # Verify array contents
    np.testing.assert_array_equal(cost["stages"], test_stages)
    np.testing.assert_array_equal(cost["costs"], test_costs)


def test_parse_sample_file(sample_costs_file):
    """Test parsing of the sample cost file."""
    parser = CostsParser(str(sample_costs_file))
    parser.parse()

    # Verify basic structure
    assert parser.get_num_generators() == 2
    costs = parser.get_costs()
    assert len(costs) == 2

    # Verify all generators have required fields
    for gen_cost in costs:
        assert isinstance(gen_cost["name"], str)
        assert gen_cost["name"] != ""
        assert isinstance(gen_cost["stages"], np.ndarray)
        assert isinstance(gen_cost["costs"], np.ndarray)
        assert len(gen_cost["stages"]) > 0
        assert len(gen_cost["stages"]) == len(gen_cost["costs"])

        # Verify array types and values
        assert gen_cost["stages"].dtype == np.int32
        assert gen_cost["costs"].dtype == np.float64
        assert np.all(gen_cost["stages"] > 0)
        assert np.all(gen_cost["costs"] > 0)

    # Verify first generator data
    gen1 = costs[0]
    assert gen1["name"] == "CMPC_PACIFICO_BL3"
    assert len(gen1["stages"]) == 4
    assert gen1["stages"][0] == 4
    assert gen1["costs"][0] == 157.9

    # Verify second generator data
    gen2 = costs[1]
    assert gen2["name"] == "ANDINA"
    assert len(gen2["stages"]) == 3
    assert gen2["stages"][0] == 5
    assert gen2["costs"][0] == 67.2


def test_get_costs_by_name(sample_costs_file):
    """Test getting costs by generator name."""
    parser = CostsParser(str(sample_costs_file))
    parser.parse()

    # Test existing generator
    costs = parser.get_costs()
    first_gen = costs[0]["name"]
    gen_data = parser.get_costs_by_name(first_gen)
    assert gen_data is not None
    assert gen_data["name"] == first_gen
    assert len(gen_data["stages"]) > 0
    assert len(gen_data["costs"]) > 0

    # Test another existing generator if available
    if len(costs) > 1:
        second_gen = costs[1]["name"]
        gen_data = parser.get_costs_by_name(second_gen)
        assert gen_data is not None
        assert gen_data["name"] == second_gen
        assert len(gen_data["stages"]) > 0
        assert len(gen_data["costs"]) > 0

    # Test non-existent generator
    missing = parser.get_costs_by_name("NonExistentGen")
    assert missing is None
