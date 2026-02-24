#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for LineWriter class."""

import json
import tempfile
import typing
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
def sample_line_writer(sample_line_file, tmp_path):
    """Fixture providing initialized LineWriter with sample data."""
    parser = LineParser(sample_line_file)  # Using fixture directly
    parser.parse()
    options = {"output_dir": tmp_path}
    return LineWriter(parser, options=options)


def test_line_writer_initialization(sample_line_file, tmp_path):  # pylint: disable=redefined-outer-name
    """Test LineWriter initialization."""
    parser = LineParser(sample_line_file)
    parser.parse()
    options = {"output_dir": tmp_path}
    writer = LineWriter(parser, options=options)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_lines


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
        "bus_a": int,
        "bus_b": int,
        "resistance": float,
        "reactance": float,
        "tmax_ab": float,
        "tmax_ba": float,
        "voltage": float,
        "active": int,
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


def test_json_output_structure(
    sample_line_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_lines = sample_line_writer.to_json_array()

    # Expected structure from system_c0.json
    required_fields = {
        "uid": int,
        "name": str,
        "bus_a": int,
        "bus_b": int,
        "resistance": float,
        "reactance": float,
        "tmax_ab": float,
        "tmax_ba": float,
        "voltage": float,
        "active": int,
    }

    for line in json_lines:
        # Check all required fields exist and have correct types
        assert set(line.keys()) == set(required_fields.keys())
        for field, field_type in required_fields.items():
            assert isinstance(line[field], field_type), (
                f"Field {field} should be {field_type}, got {type(line[field])}"
            )

        # Additional value checks
        assert line["resistance"] >= 0, "Resistance should be non-negative"
        assert line["reactance"] >= 0, "Reactance should be non-negative"
        assert line["tmax_ab"] >= 0, "Flow limit AB should be non-negative"
        assert line["tmax_ba"] >= 0, "Flow limit BA should be non-negative"


def test_write_empty_lines(tmp_path):
    """Test handling of empty line list."""

    # Create parser with no lines
    # Create mock parser with empty data
    class MockLineParser:
        def __init__(self):
            self._data = []
            self.num_lines = 0

        def get_lines(self):
            return self._data

        def get_all(self):
            return self._data

    mock_parser: typing.Any = MockLineParser()

    options = {"output_dir": tmp_path}
    writer = LineWriter(mock_parser, options=options)
    json_lines = writer.to_json_array()
    assert not json_lines

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
