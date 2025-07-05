#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for LineWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..line_writer import LineWriter
from ..line_parser import LineParser
from .conftest import get_example_file


@pytest.fixture
def sample_line_file():
    """Fixture providing path to sample line file."""
    return get_example_file("plpcnfli.dat")


@pytest.fixture
def sample_line_writer(sample_line_file):
    """Fixture providing initialized LineWriter with sample data."""
    parser = LineParser(sample_line_file)  # Using fixture directly
    parser.parse()
    return LineWriter(parser)


def test_line_writer_initialization(
    sample_line_file,
):  # pylint: disable=redefined-outer-name
    """Test LineWriter initialization."""
    parser = LineParser(sample_line_file)
    parser.parse()
    writer = LineWriter(parser)

    assert writer.parser == parser
    assert len(writer.items) == parser.num_lines


def test_to_json_array(sample_line_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of lines to JSON array format."""
    json_lines = sample_line_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_lines, list)
    assert len(json_lines) > 0

    # Verify each line has required fields
    required_fields = {
        "uid": int,
        "name": str,
        "bus_a": str,
        "bus_b": str,
        "r": float,
        "x": float,
        "f_max_ab": float,
        "f_max_ba": float,
        "voltage": float,
        "has_losses": bool,
        "is_operational": bool,
    }

    for line in json_lines:
        for field, field_type in required_fields.items():
            assert field in line
            assert isinstance(line[field], field_type)


def test_write_to_file(sample_line_writer):  # pylint: disable=redefined-outer-name
    """Test writing line data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_line_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_from_line_file(sample_line_file):  # pylint: disable=redefined-outer-name
    """Test creating LineWriter directly from line file."""
    writer = LineWriter.from_file(
        sample_line_file, LineParser
    )  # Using fixture directly

    # Verify parser was initialized and parsed
    assert writer.parser.file_path == sample_line_file
    assert writer.parser.num_lines > 0
    assert len(writer.items) == writer.parser.num_lines


def test_json_output_structure(
    sample_line_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_lines = sample_line_writer.to_json_array()

    # Check against example from system_c0.json
    required_fields = {
        "uid",
        "name",
        "bus_a",
        "bus_b",
        "r",
        "x",
        "f_max_ab",
        "f_max_ba",
        "voltage",
        "has_losses",
        "is_operational",
    }

    for line in json_lines:
        assert set(line.keys()) == required_fields
        assert isinstance(line["uid"], int)
        assert isinstance(line["name"], str)
        assert isinstance(line["bus_a"], str)
        assert isinstance(line["bus_b"], str)
        assert isinstance(line["r"], float)
        assert isinstance(line["x"], float)
        assert isinstance(line["f_max_ab"], float)
        assert isinstance(line["f_max_ba"], float)
        assert isinstance(line["voltage"], float)
        assert isinstance(line["has_losses"], bool)
        assert isinstance(line["is_operational"], bool)


def test_write_empty_lines():
    """Test handling of empty line list."""
    # Create parser with no lines
    parser = LineParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser.num_lines = 0

    writer = LineWriter(parser)
    json_lines = writer.to_json_array()
    assert not json_lines

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
