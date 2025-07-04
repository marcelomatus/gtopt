"""Unit tests for stage_parser.py StageParser class."""

import pytest
from pathlib import Path
from scripts.plp2gtopt import StageParser


@pytest.fixture
def sample_stage_file():
    """Fixture providing path to sample stage file."""
    return Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpeta.dat"


def test_stage_parser_initialization():
    """Test StageParser initialization."""
    parser = StageParser("test.dat")
    assert parser.file_path == "test.dat"
    assert not parser.stages  # Check empty list
    assert parser.num_stages == 0


def test_get_num_stages():
    """Test get_num_stages returns correct value."""
    parser = StageParser("test.dat")
    parser.num_stages = 3
    assert parser.get_num_stages() == 3


def test_get_stages():
    """Test get_stages returns stages list."""
    parser = StageParser("test.dat")
    test_stages = [{"test": "data"}]
    parser.stages = test_stages
    assert parser.get_stages() == test_stages


def test_parse_sample_file(sample_stage_file):
    """Test parsing of the sample stage file."""
    parser = StageParser(str(sample_stage_file))
    parser.parse()

    # Verify basic structure
    assert parser.get_num_stages() == 10
    stages = parser.get_stages()
    assert len(stages) == 10

    # Verify first stage data
    stage1 = stages[0]
    assert stage1["numero"] == 1
    assert stage1["duracion"] == 1.0

    # Verify last stage data
    stage10 = stages[9]
    assert stage10["numero"] == 10
    assert stage10["duracion"] == 1.0


def test_get_stage_by_number(sample_stage_file):
    """Test getting stage by number."""
    parser = StageParser(str(sample_stage_file))
    parser.parse()

    # Test existing stage
    stage5 = parser.get_stage_by_number(5)
    assert stage5 is not None
    assert stage5["numero"] == 5
    assert stage5["duracion"] == 1.0

    # Test non-existent stage
    missing = parser.get_stage_by_number(99)
    assert missing is None
