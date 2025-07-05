#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for BusWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..bus_writer import BusWriter
from ..bus_parser import BusParser
from .conftest import get_example_file


@pytest.fixture
def sample_bus_file():
    """Fixture providing path to sample bus file."""
    return get_example_file("plpbar.dat")


@pytest.fixture
def sample_bus_writer(sample_bus_file):
    """Fixture providing initialized BusWriter with sample data."""
    parser = BusParser(sample_bus_file)
    parser.parse()
    return BusWriter(parser)


def test_bus_writer_initialization(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test BusWriter initialization."""
    parser = BusParser(sample_bus_file)
    parser.parse()
    writer = BusWriter(parser)

    assert writer.parser == parser
    assert len(writer.items) == parser.num_buses


def test_to_json_array(sample_bus_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of buses to JSON array format."""
    json_buses = sample_bus_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_buses, list)
    assert len(json_buses) > 0

    # Verify each bus has required fields
    for bus in json_buses:
        assert "uid" in bus
        assert "name" in bus
        assert "voltage" in bus
        assert isinstance(bus["uid"], int)
        assert isinstance(bus["name"], str)
        assert isinstance(bus["voltage"], float)


def test_write_to_file(sample_bus_writer):  # pylint: disable=redefined-outer-name
    """Test writing bus data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_bus_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_from_bus_file(sample_bus_file):  # pylint: disable=redefined-outer-name
    """Test creating BusWriter directly from bus file."""
    writer = BusWriter.from_file(sample_bus_file, BusParser)

    # Verify parser was initialized and parsed
    assert writer.parser.file_path == sample_bus_file
    assert writer.parser.num_buses > 0
    assert len(writer.items) == writer.parser.num_buses


def test_json_output_structure(
    sample_bus_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_buses = sample_bus_writer.to_json_array()

    # Check against example from system_c0.json
    for bus in json_buses:
        assert set(bus.keys()) == {"uid", "name", "voltage"}
        assert isinstance(bus["uid"], int)
        assert isinstance(bus["name"], str)
        assert isinstance(bus["voltage"], float)


def test_write_empty_buses():
    """Test handling of empty bus list."""
    # Create parser with no buses
    parser = BusParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser.num_buses = 0

    writer = BusWriter(parser)
    json_buses = writer.to_json_array()
    assert not json_buses

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
