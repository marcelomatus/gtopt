#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for AflceWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..aflce_writer import AflceWriter
from ..aflce_parser import AflceParser
from .conftest import get_example_file


@pytest.fixture
def sample_aflce_file():
    """Fixture providing path to sample flow file."""
    return get_example_file("plpaflce.dat")


@pytest.fixture
def sample_aflce_writer(sample_aflce_file):
    """Fixture providing initialized AflceWriter with sample data."""
    parser = AflceParser(sample_aflce_file)
    parser.parse()
    return AflceWriter(parser)


def test_aflce_writer_initialization(sample_aflce_file):
    """Test AflceWriter initialization."""
    parser = AflceParser(sample_aflce_file)
    parser.parse()
    writer = AflceWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_flows


def test_to_json_array(sample_aflce_writer):
    """Test conversion of flows to JSON array format."""
    json_flows = sample_aflce_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_flows, list)
    assert len(json_flows) > 0

    # Verify each flow has required fields
    required_fields = {
        "name": str,
        "blocks": list,
        "flows": list,
    }

    for flow in json_flows:
        for field, field_type in required_fields.items():
            assert field in flow, f"Missing field: {field}"
            assert isinstance(
                flow[field], field_type
            ), f"Field {field} should be {field_type}, got {type(flow[field])}"


def test_write_to_file(sample_aflce_writer):
    """Test writing flow data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_aflce_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_aflce_writer):
    """Verify JSON output matches expected structure."""
    json_flows = sample_aflce_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "blocks": list,
        "flows": list,
    }

    for flow in json_flows:
        # Check all required fields exist and have correct types
        assert set(flow.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                flow[field], field_type
            ), f"Field {field} should be {field_type}, got {type(flow[field])}"

        # Additional value checks
        assert len(flow["name"]) > 0, "Name should not be empty"
        assert len(flow["blocks"]) > 0, "Should have at least one block"
        assert len(flow["blocks"]) == len(
            flow["flows"]
        ), "Blocks and flows should match"


def test_write_empty_flows():
    """Test handling of empty flow list."""
    # Create parser with no flows
    parser = AflceParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    writer = AflceWriter(parser)

    # Test empty array conversion
    json_flows = writer.to_json_array()
    assert isinstance(json_flows, list)
    assert len(json_flows) == 0

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
