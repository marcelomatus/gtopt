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
    assert len(writer.items) == parser.num_bars


def test_to_json_array(sample_demand_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of demands to JSON array format."""
    json_demands = sample_demand_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_demands, list)
    assert len(json_demands) > 0

    # Verify each demand has required fields
    required_fields = {
        "uid": int,
        "name": str,
        "bus": str,
        "lmax": str,
        "capacity": float,
        "expcap": type(None),
        "expmod": type(None),
        "annual_capcost": type(None),
    }

    for demand in json_demands:
        for field, field_type in required_fields.items():
            assert field in demand
            assert isinstance(demand[field], field_type)


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


def test_from_demand_file(sample_demand_file):  # pylint: disable=redefined-outer-name
    """Test creating DemandWriter directly from demand file."""
    writer = DemandWriter.from_file(sample_demand_file, DemandParser)

    # Verify parser was initialized and parsed
    assert writer.parser.file_path == sample_demand_file
    assert writer.parser.num_bars > 0
    assert len(writer.items) == writer.parser.num_bars


def test_json_output_structure(
    sample_demand_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_demands = sample_demand_writer.to_json_array()

    # Check against example from system_c0.json
    expected_fields = {
        "uid",
        "name",
        "bus",
        "lmax",
        "capacity",
        "expcap",
        "expmod",
        "annual_capcost",
    }
    for demand in json_demands:
        assert set(demand.keys()) == expected_fields
        assert isinstance(demand["uid"], int)
        assert isinstance(demand["name"], str)
        assert isinstance(demand["bus"], str)
        assert isinstance(demand["lmax"], str)
        assert isinstance(demand["capacity"], float)
        assert demand["expcap"] is None
        assert demand["expmod"] is None
        assert demand["annual_capcost"] is None


def test_write_empty_demands():
    """Test handling of empty demand list."""
    # Create parser with no demands
    parser = DemandParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser.num_bars = 0

    writer = DemandWriter(parser)
    json_demands = writer.to_json_array()
    assert not json_demands

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
