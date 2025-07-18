"""Unit tests for ManliWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..manli_writer import ManliWriter
from ..manli_parser import ManliParser
from .conftest import get_example_file


@pytest.fixture
def sample_manli_file():
    """Fixture providing path to sample line maintenance file."""
    return get_example_file("plpmanli.dat")


@pytest.fixture
def sample_manli_writer(sample_manli_file):
    """Fixture providing initialized ManliWriter with sample data."""
    parser = ManliParser(sample_manli_file)
    parser.parse()
    return ManliWriter(parser)


def test_manli_writer_initialization(sample_manli_file):
    """Test ManliWriter initialization."""
    parser = ManliParser(sample_manli_file)
    parser.parse()
    writer = ManliWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_manlis


def test_to_json_array(sample_manli_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_manlis = sample_manli_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_manlis, list)
    assert len(json_manlis) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "block": list,
        "p_max_ab": list,
        "p_max_ba": list,
        "operational": list,
    }

    for manli in json_manlis:
        for field, field_type in required_fields.items():
            assert field in manli, f"Missing field: {field}"
            assert isinstance(
                manli[field], field_type
            ), f"Field {field} should be {field_type}, got {type(manli[field])}"


def test_write_to_file(sample_manli_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_manli_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_manli_writer):
    """Verify JSON output matches expected structure."""
    json_manlis = sample_manli_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "block": list,
        "p_max_ab": list,
        "p_max_ba": list,
        "operational": list,
    }

    for manli in json_manlis:
        # Check all required fields exist and have correct types
        assert set(manli.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                manli[field], field_type
            ), f"Field {field} should be {field_type}, got {type(manli[field])}"

        # Additional value checks
        assert len(manli["name"]) > 0, "Name should not be empty"
        assert len(manli["block"]) > 0, "Should have at least one block"
        assert len(manli["block"]) == len(
            manli["p_max_ab"]
        ), "Blocks and p_max_ab should match"
        assert len(manli["block"]) == len(
            manli["p_max_ba"]
        ), "Blocks and p_max_ba should match"
        assert len(manli["block"]) == len(
            manli["operational"]
        ), "Blocks and operational should match"


def test_write_empty_manlis():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = ManliParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    writer = ManliWriter(parser)

    # Test empty array conversion
    json_manlis = writer.to_json_array()
    assert isinstance(json_manlis, list)
    assert len(json_manlis) == 0

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
