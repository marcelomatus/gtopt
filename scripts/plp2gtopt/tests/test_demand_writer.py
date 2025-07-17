#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for DemandWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..demand_writer import DemandWriter
from ..demand_parser import DemandParser
from .conftest import get_example_file


@pytest.fixture
def sample_demand_file():
    """Fixture providing path to sample demand file."""
    return get_example_file("plpdem.dat")


@pytest.fixture
def sample_demand_writer(sample_demand_file):
    """Fixture providing initialized DemandWriter with sample data."""
    parser = DemandParser(sample_demand_file)
    parser.parse()
    return DemandWriter(parser)


def test_demand_writer_initialization(
    sample_demand_file,
):  # pylint: disable=redefined-outer-name
    """Test DemandWriter initialization."""
    parser = DemandParser(sample_demand_file)
    parser.parse()
    writer = DemandWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_demands


def test_to_json_array(sample_demand_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of demands to JSON array format."""
    json_demands = sample_demand_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_demands, list)
    assert len(json_demands) > 0

    # Verify each demand has required fields
    required_fields = [
        ("uid", int),
        ("name", str),
        ("bus", str),
        ("lmax", (str, float)),  # Tuple of allowed types
    ]

    for demand in json_demands:
        for field, field_type in required_fields:
            assert field in demand, f"Missing field: {field}"
            if isinstance(field_type, tuple):
                valid = any(isinstance(demand[field], t) for t in field_type)
                assert valid, (
                    f"Field {field} should be one of {field_type}, got {type(demand[field])}"
                )
            else:
                assert isinstance(demand[field], field_type), (
                    f"Field {field} should be {field_type}, got {type(demand[field])}"
                )


def test_write_to_file(sample_demand_writer):  # pylint: disable=redefined-outer-name
    """Test writing demand data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_demand_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(
    sample_demand_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_demands = sample_demand_writer.to_json_array()

    # Expected structure from system_c0.json
    REQUIRED_FIELDS = [
        ("uid", int),
        ("name", str),
        ("bus", str),
        ("lmax", (str, float)),
    ]

    for demand in json_demands:
        # Check all required fields exist and have correct types
        assert set(demand.keys()) == {field for field, _ in REQUIRED_FIELDS}
        for field, field_type in REQUIRED_FIELDS:
            if isinstance(field_type, tuple):
                valid = any(isinstance(demand[field], t) for t in field_type)
                assert valid, (
                    f"Field {field} should be one of {field_type}, got {type(demand[field])}"
                )
            else:
                assert isinstance(demand[field], field_type), (
                    f"Field {field} should be {field_type}, got {type(demand[field])}"
                )

        # Additional value checks
        assert demand["uid"] > 0, "UID should be positive integer"
        assert len(demand["name"]) > 0, "Name should not be empty"


def test_write_empty_demands():
    """Test handling of empty demand list."""
    # Create parser with no demands
    parser = DemandParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    writer = DemandWriter(parser)

    # Test empty array conversion
    json_demands = writer.to_json_array()
    assert isinstance(json_demands, list)
    assert len(json_demands) == 0

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

        # Verify file can be read again
        with open(output_path, "r", encoding="utf-8") as f:
            data2 = json.load(f)
            assert data == data2
