"""Unit tests for pydem.py DemandParser class."""

import pytest
from pathlib import Path
from plp2gtopt.pydem import DemandParser

@pytest.fixture
def sample_demand_file(tmp_path):
    """Create a temporary demand file for testing."""
    content = """2
    1 100.0 50.0 0.9
    2 200.0 100.0 0.8
    """
    file_path = tmp_path / "test_demand.dat"
    file_path.write_text(content)
    return file_path

def test_get_num_bars(sample_demand_file):
    """Test get_num_bars returns correct number of bars."""
    parser = DemandParser(sample_demand_file)
    assert parser.get_num_bars() == 2

def test_get_demands(sample_demand_file):
    """Test get_demands returns correct demand data."""
    parser = DemandParser(sample_demand_file)
    demands = parser.get_demands()
    
    assert len(demands) == 2
    assert demands[0]["bus"] == 1
    assert demands[0]["demand"] == 100.0
    assert demands[0]["reserve"] == 50.0
    assert demands[0]["probability"] == 0.9
    
    assert demands[1]["bus"] == 2
    assert demands[1]["demand"] == 200.0
    assert demands[1]["reserve"] == 100.0
    assert demands[1]["probability"] == 0.8

def test_empty_file(tmp_path):
    """Test behavior with empty file."""
    empty_file = tmp_path / "empty.dat"
    empty_file.write_text("")
    
    with pytest.raises(ValueError):
        DemandParser(empty_file)

def test_invalid_format(tmp_path):
    """Test behavior with invalid file format."""
    invalid_file = tmp_path / "invalid.dat"
    invalid_file.write_text("not a number\n1 2 3")
    
    with pytest.raises(ValueError):
        DemandParser(invalid_file)

def test_missing_values(tmp_path):
    """Test behavior with missing values in data lines."""
    missing_file = tmp_path / "missing.dat"
    missing_file.write_text("1\n1 100.0")
    
    with pytest.raises(ValueError):
        DemandParser(missing_file)
