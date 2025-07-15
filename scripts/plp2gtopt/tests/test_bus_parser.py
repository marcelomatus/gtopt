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
    # assert parser.num_buses == 0


def test_parse_sample_file(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample bus file."""
    parser = BusParser(str(sample_bus_file))
    parser.parse()

    # Verify basic structure
    num_buses = parser.num_buses
    assert num_buses == 10
    buses = parser.buses
    assert len(buses) == num_buses

    # Verify all buses have required fields
    for bus in buses:
        assert isinstance(bus["number"], int)
        assert isinstance(bus["name"], str)
        assert isinstance(bus["voltage"], float)
        assert bus["number"] > 0
        assert bus["name"] != ""
        assert bus["voltage"] > 0

    # Verify first bus data
    bus1 = buses[0]
    assert bus1["number"] == 1
    assert bus1["name"] == "AltoNorte110"
    assert bus1["voltage"] == 110.0

    # Verify last bus data
    last_bus = buses[-1]
    assert last_bus["number"] == 10
    assert last_bus["name"] == "Capricornio220"
    assert last_bus["voltage"] == 220.0

    # Verify bus numbers are sequential
    for i, bus in enumerate(buses, 1):
        assert bus["number"] == i

    # Verify voltage extraction from names
    test_cases = [
        ("AltoNorte110", 110.0),
        ("Andes220", 220.0),
        ("Andes345", 345.0),
        ("Arica066", 66.0),
        ("Atacama220_BP1", 220.0),
        ("Capricornio110", 110.0),
    ]
    for name, expected_voltage in test_cases:
        assert any(
            bus["name"] == name and bus["voltage"] == expected_voltage for bus in buses
        )


def test_get_bus_by_name(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test getting bus by name."""
    parser = BusParser(str(sample_bus_file))
    parser.parse()

    # Test existing bus
    buses = parser.buses
    first_bus_name = buses[0]["name"]
    bus = parser.get_bus_by_name(first_bus_name)
    assert bus is not None
    assert bus["name"] == first_bus_name

    # Test non-existent bus
    missing = parser.get_bus_by_name("NonExistentBus")
    assert missing is None
