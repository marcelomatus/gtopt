"""Unit tests for ExtracWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..extrac_writer import ExtracWriter
from ..extrac_parser import ExtracParser
from .conftest import get_example_file


@pytest.fixture
def sample_extrac_file():
    """Fixture providing path to sample extraction file."""
    return get_example_file("plpextrac.dat")


@pytest.fixture
def sample_extrac_writer(sample_extrac_file):
    """Fixture providing initialized ExtracWriter with sample data."""
    parser = ExtracParser(sample_extrac_file)
    parser.parse()
    return ExtracWriter(parser)


def test_extrac_writer_initialization(sample_extrac_file):
    """Test ExtracWriter initialization."""
    parser = ExtracParser(sample_extrac_file)
    parser.parse()
    writer = ExtracWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_extracs


def test_to_json_array(sample_extrac_writer):
    """Test conversion of extraction data to JSON array format."""
    json_extracs = sample_extrac_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_extracs, list)
    assert len(json_extracs) == 3

    # Verify each extraction has required fields
    required_fields = {
        "name": str,
        "max_extrac": float,
        "downstream": str,
    }

    for extrac in json_extracs:
        for field, field_type in required_fields.items():
            assert field in extrac, f"Missing field: {field}"
            assert isinstance(extrac[field], field_type), (
                f"Field {field} should be {field_type}, got {type(extrac[field])}"
            )


def test_write_to_file(sample_extrac_writer):
    """Test writing extraction data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_extrac_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) == 3


def test_json_output_structure(sample_extrac_writer):
    """Verify JSON output matches expected structure."""
    json_extracs = sample_extrac_writer.to_json_array()

    # Expected structure
    required_fields = {
        "name": str,
        "max_extrac": float,
        "downstream": str,
    }

    for extrac in json_extracs:
        # Check all required fields exist and have correct types
        assert set(extrac.keys()) == set(required_fields.keys())
        for field, field_type in required_fields.items():
            assert isinstance(extrac[field], field_type), (
                f"Field {field} should be {field_type}, got {type(extrac[field])}"
            )

        # Additional value checks
        assert len(extrac["name"]) > 0, "Name should not be empty"
        assert extrac["max_extrac"] >= 0, "Max extraction should be non-negative"
        assert len(extrac["downstream"]) > 0, "Downstream should not be empty"


class MockEmptyExtracParser(ExtracParser):
    """Mock parser that returns empty data."""

    def __init__(self):
        """Initializes the mock parser with a dummy file path."""
        super().__init__("dummy.dat")

    def get_all(self) -> list:
        """Return an empty list of items."""
        return []


def test_write_empty_extracs():
    """Test handling of empty extraction list."""
    # Create parser with no extraction data
    parser = MockEmptyExtracParser()
    writer = ExtracWriter(parser)

    # Test empty array conversion
    json_extracs = writer.to_json_array()
    assert isinstance(json_extracs, list)
    assert len(json_extracs) == 0

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
