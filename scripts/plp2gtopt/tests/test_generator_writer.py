#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for GeneratorWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..generator_writer import CentralWriter
from ..generator_parser import CentralParser
from .conftest import get_example_file


@pytest.fixture
def sample_generator_file():
    """Fixture providing path to sample generator file."""
    return get_example_file("plpcnfce.dat")


@pytest.fixture
def sample_generator_writer(sample_generator_file):
    """Fixture providing initialized GeneratorWriter with sample data."""
    parser = CentralParser(sample_generator_file)
    parser.parse()
    return CentralWriter(parser)


def test_generator_writer_initialization(
    sample_generator_file,
):  # pylint: disable=redefined-outer-name
    """Test GeneratorWriter initialization."""
    parser = CentralParser(sample_generator_file)
    parser.parse()
    writer = CentralWriter(parser)

    assert writer.parser == parser
    assert len(writer.items) == parser.num_generators


def test_to_json_array(sample_generator_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of generators to JSON array format."""
    json_generators = sample_generator_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_generators, list)
    assert len(json_generators) > 0

    # Verify each generator has required fields
    for generator in json_generators:
        assert "uid" in generator
        assert "name" in generator
        assert "bus" in generator
        assert "gcost" in generator
        assert "capacity" in generator
        assert isinstance(generator["uid"], int)
        assert isinstance(generator["name"], str)
        assert isinstance(generator["bus"], int)
        assert isinstance(generator["gcost"], float)
        assert isinstance(generator["capacity"], float)


def test_write_to_file(sample_generator_writer):  # pylint: disable=redefined-outer-name
    """Test writing generator data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_generator_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_from_generator_file(
    sample_generator_file,
):  # pylint: disable=redefined-outer-name
    """Test creating GeneratorWriter directly from generator file."""
    writer = CentralWriter.from_file(sample_generator_file, CentralParser)

    # Verify parser was initialized and parsed
    assert writer.parser.file_path == sample_generator_file
    assert writer.parser.num_generators > 0
    assert len(writer.items) == writer.parser.num_generators


def test_write_empty_generators():
    """Test handling of empty generator list."""
    # Create parser with no generators
    parser = CentralParser("dummy.dat")

    # Create a mock parser with empty generators list
    class MockGeneratorParser:
        def __init__(self):
            self._generators = []
            self._num_generators = 0

        @property
        def generators(self):
            return self._generators

        @property
        def num_generators(self):
            return self._num_generators

        def get_generators(self):
            return self._generators

    parser = MockGeneratorParser()

    writer = CentralWriter(parser)
    json_generators = writer.to_json_array()
    assert not json_generators

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
