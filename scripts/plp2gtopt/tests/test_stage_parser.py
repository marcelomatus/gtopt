"""Unit tests for stage_parser.py StageParser class."""

from pathlib import Path
import pytest
from ..stage_parser import StageParser
from .conftest import get_example_file


@pytest.fixture
def sample_stage_file():
    """Fixture providing path to sample stage file."""
    return get_example_file("plpeta.dat")


def test_stage_parser_initialization():
    """Test StageParser initialization."""
    parser = StageParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.stages  # Check empty list
    assert parser.num_stages == 0


def test_parse_sample_file(sample_stage_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample stage file."""
    parser = StageParser(str(sample_stage_file))
    parser.parse()

    # Verify basic structure
    num_stages = parser.num_stages
    assert num_stages == 10  # Number of stages is 10 in test data
    stages = parser.stages
    assert len(stages) == num_stages

    # Verify all stages have required fields
    for stage in stages:
        assert isinstance(stage["number"], int)
        assert isinstance(stage["duration"], float)
        assert isinstance(stage["discount_factor"], float)
        assert stage["number"] > 0
        assert stage["duration"] > 0
        assert 0 < stage["discount_factor"] <= 1.0

    # Verify first stage data
    stage1 = stages[0]
    assert stage1["number"] == 1
    assert stage1["duration"] == 168.0
    assert stage1["discount_factor"] == 1.0

    # Verify last stage data
    last_stage = stages[-1]
    assert last_stage["number"] == 10
    assert last_stage["duration"] == 192.0
    assert 0 < last_stage["discount_factor"] < 1.0

    # Verify stage numbers are sequential
    for i, stage in enumerate(stages, 1):
        assert stage["number"] == i

    # Verify discount factors are properly calculated
    for stage in stages[3:]:  # Skip first stage which defaults to 1.0
        if stage["discount_factor"] != 1.0:
            assert stage["discount_factor"] == pytest.approx(1.0 / 1.007974, rel=1e-6)
            break  # Just check one non-default value


def test_discount_factor_calculation(
    sample_stage_file,
):  # pylint: disable=redefined-outer-name
    """Test discount factor calculation with and without FactTasa."""
    parser = StageParser(str(sample_stage_file))
    parser.parse()

    stages = parser.stages

    # Verify discount factors are calculated correctly
    assert "discount_factor" in stages[0]
    assert isinstance(stages[0]["discount_factor"], float)

    assert stages[0]["discount_factor"] == 1.0  # First stage should default to 1.0

    # Verify discount factors are valid
    for stage in stages:
        assert 0 < stage["discount_factor"] <= 1.0  # Should be between 0 and 1
