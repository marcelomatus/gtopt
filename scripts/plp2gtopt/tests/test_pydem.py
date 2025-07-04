"""Unit tests for pydem.py DemandParser class."""

import pytest
from pathlib import Path
from plp2gtopt.pydem import DemandParser

def test_demand_parser_initialization():
    """Test DemandParser initialization."""
    parser = DemandParser("test.dat")
    assert parser.file_path == "test.dat"
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
