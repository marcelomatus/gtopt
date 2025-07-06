"""Unit tests for pydem.py DemandParser class."""

from pathlib import Path
import pytest
import numpy as np
from ..demand_parser import DemandParser
from .conftest import get_example_file


@pytest.fixture
def sample_demand_file():
    """Fixture providing path to sample demand file."""
    return get_example_file("plpdem.dat")


def test_demand_parser_initialization():
    """Test DemandParser initialization."""
    test_path = "test.dat"
    parser = DemandParser(test_path)
    assert parser.file_path == Path(test_path)  # Compare Path objects
    assert not parser.get_demands()  # Use public method instead of accessing _data
    assert parser.num_demands == 0


def test_get_num_bars():
    """Test get_num_bars returns correct value."""
    parser = DemandParser("test.dat")
    parser.num_demands = 5
    assert parser.get_num_bars() == 5


def test_get_demands():
    """Test get_demands returns properly structured demand data."""
    parser = DemandParser("test.dat")
    # Setup test data
    test_blocks = np.array([1, 2, 3], dtype=np.int32)
    test_values = np.array([1.0, 2.0, 3.0], dtype=np.float64)
    
    parser._data = [{"number": 1, "name": "test"}]  # pylint: disable=protected-access
    parser.demand_blocks = test_blocks
    parser.demand_values = test_values 
    parser.demand_indices = [(0, 3)]

    demands = parser.get_demands()
    assert len(demands) == 1
    demand = demands[0]
    
    # Verify structure and types
    assert demand["number"] == 1
    assert demand["name"] == "test"
    assert isinstance(demand["blocks"], np.ndarray)
    assert isinstance(demand["values"], np.ndarray)
    assert demand["blocks"].dtype == np.int32
    assert demand["values"].dtype == np.float64
    
    # Verify array contents
    np.testing.assert_array_equal(demand["blocks"], test_blocks)
    np.testing.assert_array_equal(demand["values"], test_values)


def test_parse_sample_file(sample_demand_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample demand file."""
    parser = DemandParser(str(sample_demand_file))
    parser.parse()

    # Verify basic structure
    assert parser.get_num_bars() == 2
    demands = parser.get_demands()
    assert len(demands) == 2

    # Verify all bars have required fields
    for demand_bar in demands:
        assert isinstance(demand_bar["name"], str)
        assert demand_bar["name"] != ""
        assert isinstance(demand_bar["blocks"], np.ndarray)
        assert isinstance(demand_bar["values"], np.ndarray)
        assert len(demand_bar["blocks"]) > 0
        assert len(demand_bar["blocks"]) == len(demand_bar["values"])

        # Verify array types and values
        assert demand_bar["blocks"].dtype == np.int32
        assert demand_bar["values"].dtype == np.float64
        assert np.all(demand_bar["blocks"] > 0)
        assert np.all(demand_bar["values"] > 0)

    # Verify first bar data
    bar1 = demands[0]
    assert bar1["name"] == "Coronel066"
    assert len(bar1["blocks"]) == 5
    assert bar1["blocks"][0] == 1  # First demand block is 3 in test data
    assert bar1["values"][0] == 89.05
    assert bar1["blocks"][-1] == 5
    assert bar1["values"][-1] == 82.63

    # Verify second bar data
    bar2 = demands[1]
    assert bar2["name"] == "Condores220"
    assert len(bar2["blocks"]) == 4
    assert bar2["blocks"][0] == 1
    assert bar2["values"][0] == 105.21
    assert bar2["blocks"][-1] == 4
    assert bar2["values"][-1] == 93.05

    # Verify block numbers are sequential per bar
    for demand_bar in demands:
        blocks = demand_bar["blocks"]
        for i, block in enumerate(blocks, 1):
            assert block == i


def test_get_demand_by_name(sample_demand_file):  # pylint: disable=redefined-outer-name
    """Test getting demand by bus name."""
    parser = DemandParser(str(sample_demand_file))
    parser.parse()

    # Test existing bus
    demands = parser.get_demands()
    first_bus = demands[0]["name"]
    bus_data = parser.get_demand_by_name(first_bus)
    assert bus_data is not None
    assert bus_data["name"] == first_bus
    assert len(bus_data["blocks"]) > 0
    assert len(bus_data["values"]) > 0

    # Test another existing bus if available
    if len(demands) > 1:
        second_bus = demands[1]["name"]
        bus_data = parser.get_demand_by_name(second_bus)
        assert bus_data is not None
        assert bus_data["name"] == second_bus
        assert len(bus_data["blocks"]) > 0
        assert len(bus_data["values"]) > 0

    # Test non-existent bus
    missing = parser.get_demand_by_name("NonExistentBus")
    assert missing is None
