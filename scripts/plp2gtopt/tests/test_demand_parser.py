"""Unit tests for pydem.py DemandParser class."""

from pathlib import Path
import pytest
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
    assert not parser.demands
    assert parser.num_bars == 0


def test_get_num_bars():
    """Test get_num_bars returns correct value."""
    parser = DemandParser("test.dat")
    parser.num_bars = 5
    assert parser.get_num_bars() == 5


def test_get_demands():
    """Test get_demands returns demands list."""
    parser = DemandParser("test.dat")
    test_demands = [{"test": "data"}]
    parser.demands = test_demands
    assert parser.get_demands() == test_demands


def test_parse_sample_file(sample_demand_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample demand file."""
    parser = DemandParser(str(sample_demand_file))
    parser.parse()

    # Verify basic structure
    assert parser.get_num_bars() == 2
    demands = parser.get_demands()
    assert len(demands) == 2

    # Verify first bar data
    bar1 = demands[0]
    assert isinstance(bar1["name"], str)
    assert len(bar1["demands"]) > 0  # At least one demand entry
    # Verify first demand entry
    assert "block" in bar1["demands"][0]
    assert "demand" in bar1["demands"][0]
    assert isinstance(bar1["demands"][0]["block"], int)
    assert isinstance(bar1["demands"][0]["demand"], float)

    # Verify last demand entry
    assert "block" in bar1["demands"][-1]
    assert "demand" in bar1["demands"][-1]
    assert isinstance(bar1["demands"][-1]["block"], int)
    assert isinstance(bar1["demands"][-1]["demand"], float)

    # Verify second bar data
    bar2 = demands[1]
    assert bar2["name"] == "Condores220"
    assert len(bar2["demands"]) > 0
    assert "block" in bar2["demands"][0]
    assert "demand" in bar2["demands"][0]


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
    assert len(bus_data["demands"]) > 0

    # Test another existing bus if available
    if len(demands) > 1:
        second_bus = demands[1]["name"]
        bus_data = parser.get_demand_by_name(second_bus)
        assert bus_data is not None
        assert bus_data["name"] == second_bus
        assert len(bus_data["demands"]) > 0

    # Test non-existent bus
    missing = parser.get_demand_by_name("NonExistentBus")
    assert missing is None
