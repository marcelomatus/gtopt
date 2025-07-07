"""Unit tests for CostsWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..costs_writer import CostWriter
from ..costs_parser import CostParser
from .conftest import get_example_file


@pytest.fixture
def sample_costs_file():
    """Fixture providing path to sample cost file."""
    return get_example_file("plpcosce.dat")


@pytest.fixture
def sample_costs_writer(sample_costs_file):
    """Fixture providing initialized CostsWriter with sample data."""
    parser = CostParser(sample_costs_file)
    parser.parse()
    return CostWriter(parser)


def test_costs_writer_initialization(sample_costs_file):
    """Test CostsWriter initialization."""
    parser = CostParser(sample_costs_file)
    parser.parse()
    writer = CostWriter(parser)

    assert writer.parser == parser
    assert len(writer.items) == parser.num_generators


def test_to_json_array(sample_costs_writer):
    """Test conversion of costs to JSON array format."""
    json_costs = sample_costs_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_costs, list)
    assert len(json_costs) > 0

    # Verify each cost has required fields
    required_fields = {
        "name": str,
        "stages": list,
        "costs": list,
    }

    for cost in json_costs:
        for field, field_type in required_fields.items():
            assert field in cost, f"Missing field: {field}"
            assert isinstance(
                cost[field], field_type
            ), f"Field {field} should be {field_type}, got {type(cost[field])}"


def test_write_to_file(sample_costs_writer):
    """Test writing cost data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_costs_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_from_costs_file(sample_costs_file):
    """Test creating CostsWriter directly from cost file."""
    writer = CostWriter.from_file(sample_costs_file, CostParser)

    # Verify parser was initialized and parsed
    assert writer.parser.file_path == sample_costs_file
    assert writer.parser.num_generators > 0
    assert len(writer.items) == writer.parser.num_generators


def test_json_output_structure(sample_costs_writer):
    """Verify JSON output matches expected structure."""
    json_costs = sample_costs_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "stages": list,
        "costs": list,
    }

    for cost in json_costs:
        # Check all required fields exist and have correct types
        assert set(cost.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                cost[field], field_type
            ), f"Field {field} should be {field_type}, got {type(cost[field])}"

        # Additional value checks
        assert len(cost["name"]) > 0, "Name should not be empty"
        assert len(cost["stages"]) > 0, "Should have at least one stage"
        assert len(cost["stages"]) == len(
            cost["costs"]
        ), "Stages and costs should match"


def test_write_empty_costs():
    """Test handling of empty cost list."""
    # Create parser with no costs
    parser = CostParser("dummy.dat")
    parser._data = []
    parser.num_generators = 0

    writer = CostWriter(parser)

    # Test empty array conversion
    json_costs = writer.to_json_array()
    assert isinstance(json_costs, list)
    assert len(json_costs) == 0

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
