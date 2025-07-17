#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for CentralWriter class."""

import typing
import json
import tempfile
from pathlib import Path
import pytest
from ..central_writer import CentralWriter
from ..central_parser import CentralParser
from .conftest import get_example_file


@pytest.fixture
def sample_central_file():
    """Fixture providing path to sample central file."""
    return get_example_file("plpcnfce.dat")


@pytest.fixture
def sample_central_writer(sample_central_file):
    """Fixture providing initialized CentralWriter with sample data."""
    parser = CentralParser(sample_central_file)
    parser.parse()
    return CentralWriter(parser)


def test_central_writer_initialization(
    sample_central_file,
):  # pylint: disable=redefined-outer-name
    """Test CentralWriter initialization."""
    parser = CentralParser(sample_central_file)
    parser.parse()
    writer = CentralWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_centrals


def test_to_json_array(sample_central_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of centrals to JSON array format."""
    json_centrals = sample_central_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_centrals, list)
    assert len(json_centrals) > 0

    # Verify each central has required fields
    for central in json_centrals:
        assert "uid" in central
        assert "name" in central
        assert "bus" in central
        assert "gcost" in central
        assert "capacity" in central
        assert isinstance(central["uid"], int)
        assert isinstance(central["name"], str)
        assert isinstance(central["bus"], int)
        assert isinstance(central["gcost"], float)
        assert isinstance(central["capacity"], float)


def test_write_to_file(sample_central_writer):  # pylint: disable=redefined-outer-name
    """Test writing central data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_central_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_write_empty_centrals():
    """Test handling of empty central list."""

    # Create parser with no centrals
    # Create a mock parser with empty centrals list
    class MockCentralParser:
        def __init__(self):
            self._centrals = []
            self._num_centrals = 0

        @property
        def centrals(self):
            return self._centrals

        @property
        def num_centrals(self):
            return self._num_centrals

        def get_centrals(self):
            return self._centrals

        def get_all(self):
            return self._centrals

    mock_parser: typing.Any = MockCentralParser()

    writer = CentralWriter(mock_parser)
    json_centrals = writer.to_json_array()
    assert not json_centrals

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
