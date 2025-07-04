"""Unit tests for pydem.py DemandParser class."""

import pytest
from pathlib import Path
from ..demand_parser import DemandParser


@pytest.fixture
def sample_demand_file():
    """Fixture providing path to sample demand file."""
    test_file = Path(__file__).parent.parent / "test_data" / "plpdem.dat"
    if not test_file.exists():
        test_file = (
            Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpdem.dat"
        )
    return test_file


def test_demand_parser_initialization():
    """Test DemandParser initialization."""
    test_path = "test.dat"
    parser = DemandParser(test_path)
    assert parser.file_path == Path(test_path)  # Compare Path objects
    assert parser.demands == []
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


def test_parse_sample_file(sample_demand_file):
    """Test parsing of the sample demand file."""
    parser = DemandParser(str(sample_demand_file))
    parser.parse()

    # Verify basic structure
    assert parser.get_num_bars() == 2
    demands = parser.get_demands()
    assert len(demands) == 2

    # Verify first bar data
    bar1 = demands[0]
    assert bar1["name"] == "Coronel066"
    assert len(bar1["demands"]) == 5
    assert bar1["demands"][0] == {"mes": 3, "block": 1, "demand": 89.05}
    assert bar1["demands"][4] == {"mes": 3, "block": 5, "demand": 82.63}

    # Verify second bar data
    bar2 = demands[1]
    assert bar2["name"] == "Condores220"
    assert len(bar2["demands"]) == 4
    assert bar2["demands"][0] == {"mes": 3, "block": 1, "demand": 105.21}
    assert bar2["demands"][3] == {"mes": 3, "block": 4, "demand": 93.05}


def test_get_demand_by_name(sample_demand_file):
    """Test getting demand by bus name."""
    parser = DemandParser(str(sample_demand_file))
    parser.parse()

    # Test existing bus
    coronel = parser.get_demand_by_name("Coronel066")
    assert coronel is not None
    assert coronel["name"] == "Coronel066"
    assert len(coronel["demands"]) == 5

    # Test existing bus
    condores = parser.get_demand_by_name("Condores220")
    assert condores is not None
    assert condores["name"] == "Condores220"
    assert len(condores["demands"]) == 4

    # Test non-existent bus
    missing = parser.get_demand_by_name("NonExistentBus")
    assert missing is None
