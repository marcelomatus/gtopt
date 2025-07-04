"""Unit tests for bus_parser.py BusParser class."""

from pathlib import Path
import pytest
from ..bus_parser import BusParser
from .conftest import get_example_file


@pytest.fixture
def sample_bus_file():
    """Fixture providing path to sample bus file."""
    return get_example_file("plpbar.dat")


def test_bus_parser_initialization():
    """Test BusParser initialization."""
    test_path = "test.dat"
    parser = BusParser(test_path)
    assert parser.file_path == Path(test_path)
    assert not parser.buses
    assert parser.num_buses == 0


def test_get_num_buses():
    """Test get_num_buses returns correct value."""
    parser = BusParser("test.dat")
    parser.num_buses = 5
    assert parser.get_num_buses() == 5


def test_get_buses():
    """Test get_buses returns buses list."""
    parser = BusParser("test.dat")
    test_buses = [{"test": "data"}]
    parser.buses = test_buses
    assert parser.get_buses() == test_buses


def test_parse_sample_file(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample bus file."""
    parser = BusParser(str(sample_bus_file))
    parser.parse()

    # Verify basic structure
    num_buses = parser.get_num_buses()
    assert num_buses > 0
    buses = parser.get_buses()
    assert len(buses) == num_buses

    # Verify first bus data
    bus1 = buses[0]
    assert isinstance(bus1["number"], int)
    assert isinstance(bus1["name"], str)
    assert isinstance(bus1["voltage"], float)


def test_get_bus_by_name(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test getting bus by name."""
    parser = BusParser(str(sample_bus_file))
    parser.parse()

    # Test existing bus
    buses = parser.get_buses()
    first_bus_name = buses[0]["name"]
    bus = parser.get_bus_by_name(first_bus_name)
    assert bus is not None
    assert bus["name"] == first_bus_name

    # Test non-existent bus
    missing = parser.get_bus_by_name("NonExistentBus")
    assert missing is None
