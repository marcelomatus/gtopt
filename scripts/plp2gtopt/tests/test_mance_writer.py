"""Unit tests for ManceWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..mance_writer import ManceWriter
from ..mance_parser import ManceParser
from .conftest import get_example_file


@pytest.fixture
def sample_mance_file():
    """Fixture providing path to sample maintenance file."""
    return get_example_file("plpmance.dat")


@pytest.fixture
def sample_mance_writer(sample_mance_file):
    """Fixture providing initialized ManceWriter with sample data."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    return ManceWriter(parser)


def test_mance_writer_initialization(sample_mance_file):
    """Test ManceWriter initialization."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    writer = ManceWriter(parser)

    assert writer.parser == parser
    assert len(writer.items) == parser.num_mances


def test_to_json_array(sample_mance_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_mances = sample_mance_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_mances, list)
    assert len(json_mances) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "blocks": list,
        "p_min": list,
        "p_max": list,
    }

    for mance in json_mances:
        for field, field_type in required_fields.items():
            assert field in mance, f"Missing field: {field}"
            assert isinstance(
                mance[field], field_type
            ), f"Field {field} should be {field_type}, got {type(mance[field])}"


def test_write_to_file(sample_mance_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_mance_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_mance_writer):
    """Verify JSON output matches expected structure."""
    json_mances = sample_mance_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "blocks": list,
        "p_min": list,
        "p_max": list,
    }

    for mance in json_mances:
        # Check all required fields exist and have correct types
        assert set(mance.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                mance[field], field_type
            ), f"Field {field} should be {field_type}, got {type(mance[field])}"

        # Additional value checks
        assert len(mance["name"]) > 0, "Name should not be empty"
        assert len(mance["blocks"]) > 0, "Should have at least one block"
        assert len(mance["blocks"]) == len(
            mance["p_min"]
        ), "Blocks and p_min should match"
        assert len(mance["blocks"]) == len(
            mance["p_max"]
        ), "Blocks and p_max should match"


def test_write_empty_mances():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = ManceParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser.num_centrals = 0

    writer = ManceWriter(parser)

    # Test empty array conversion
    json_mances = writer.to_json_array()
    assert isinstance(json_mances, list)
    assert len(json_mances) == 0

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        # Verify file exists and is valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) == 0
