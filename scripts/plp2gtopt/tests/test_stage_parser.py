"""Unit tests for stage_parser.py StageParser class."""

import pytest
from pathlib import Path
from ..stage_parser import StageParser


@pytest.fixture
def sample_stage_file():
    """Fixture providing path to sample stage file."""
    test_file = Path(__file__).parent.parent / "test_data" / "plpeta.dat"
    if not test_file.exists():
        test_file = (
            Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpeta.dat"
        )
    return test_file


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
    assert stage1["duracion"] == 3.0  # Actual duration in sample file
    assert "discount_factor" in stage1
    assert isinstance(stage1["discount_factor"], float)

    # Verify last stage data
    stage10 = stages[9]
    # The sample file repeats stage numbers, so we only check duration
    assert stage10["duracion"] == 5.0  # Actual duration in sample file
    assert "discount_factor" in stage10
    assert isinstance(stage10["discount_factor"], float)


def test_discount_factor_calculation():
    """Test discount factor calculation with and without FactTasa."""
    test_file = Path(__file__).parent / "test_data" / "test_stages.dat"
    if not test_file.exists():
        pytest.skip("Test data file not found")

    parser = StageParser(str(test_file))
    parser.parse()
    stages = parser.get_stages()
    
    # First line has FactTasa
    assert stages[0]["discount_factor"] == pytest.approx(1.0/1.05)
    # Second line has no FactTasa
    assert stages[1]["discount_factor"] == 1.0


def test_get_stage_by_number(sample_stage_file):
    """Test getting stage by number."""
    parser = StageParser(str(sample_stage_file))
    parser.parse()

    # Test existing stage
    stage1 = parser.get_stage_by_number(1)
    assert stage1 is not None
    assert stage1["numero"] == 1
    assert stage1["duracion"] == 3.0  # Actual duration in sample file

    # Test non-existent stage
    missing = parser.get_stage_by_number(99)
    assert missing is None
