"""Unit tests for ManemWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..manem_writer import ManemWriter
from ..manem_parser import ManemParser
from .conftest import get_example_file


@pytest.fixture
def sample_manem_file():
    """Fixture providing path to sample reservoir maintenance file."""
    return get_example_file("plpmanem.dat")


@pytest.fixture
def sample_manem_writer(sample_manem_file):
    """Fixture providing initialized ManemWriter with sample data."""
    parser = ManemParser(sample_manem_file)
    parser.parse()
    return ManemWriter(parser)


def test_manem_writer_initialization(sample_manem_file):
    """Test ManemWriter initialization."""
    parser = ManemParser(sample_manem_file)
    parser.parse()
    writer = ManemWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_manems


def test_to_json_array(sample_manem_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_manems = sample_manem_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_manems, list)
    assert len(json_manems) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "stage": list,
        "vmin": list,
        "vmax": list,
    }

    for manem in json_manems:
        for field, field_type in required_fields.items():
            assert field in manem, f"Missing field: {field}"
            assert isinstance(
                manem[field], field_type
            ), f"Field {field} should be {field_type}, got {type(manem[field])}"


def test_write_to_file(sample_manem_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_manem_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_manem_writer):
    """Verify JSON output matches expected structure."""
    json_manems = sample_manem_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "stage": list,
        "vmin": list,
        "vmax": list,
    }

    for manem in json_manems:
        # Check all required fields exist and have correct types
        assert set(manem.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                manem[field], field_type
            ), f"Field {field} should be {field_type}, got {type(manem[field])}"

        # Additional value checks
        assert len(manem["name"]) > 0, "Name should not be empty"
        assert len(manem["stage"]) > 0, "Should have at least one stage"
        assert len(manem["stage"]) == len(
            manem["vmin"]
        ), "Stages and vol_min should match"
        assert len(manem["stage"]) == len(
            manem["vmax"]
        ), "Stages and vol_max should match"


class MockEmptyManemParser(ManemParser):
    """Mock parser that returns empty data."""

    def __init__(self):
        """Initializes the mock parser with a dummy file path."""
        super().__init__("dummy.dat")

    def get_all(self) -> list:
        """Return an empty list of items."""
        return []


def test_write_empty_manems():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = MockEmptyManemParser()
    writer = ManemWriter(parser)

    # Test empty array conversion
    json_manems = writer.to_json_array()
    assert isinstance(json_manems, list)
    assert len(json_manems) == 0

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


def test_to_dataframe_with_empty_parser():
    """Test DataFrame creation with empty parser."""
    parser = MockEmptyManemParser()
    writer = ManemWriter(parser)

    df_vmin, df_vmax = writer.to_dataframe()

    assert df_vmin.empty
    assert df_vmax.empty
