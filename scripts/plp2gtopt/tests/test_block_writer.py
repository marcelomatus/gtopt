#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for BlockWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..block_writer import BlockWriter
from ..block_parser import BlockParser
from .conftest import get_example_file


@pytest.fixture
def sample_block_file():
    """Fixture providing path to sample block file."""
    return get_example_file("plpblo.dat")


@pytest.fixture
def sample_block_writer(sample_block_file):
    """Fixture providing initialized BlockWriter with sample data."""
    parser = BlockParser(sample_block_file)
    parser.parse()
    return BlockWriter(parser)


def test_block_writer_initialization(
    sample_block_file,
):  # pylint: disable=redefined-outer-name
    """Test BlockWriter initialization."""
    parser = BlockParser(sample_block_file)
    parser.parse()
    writer = BlockWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_blocks


def test_to_json_array(sample_block_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of blocks to JSON array format."""
    json_blocks = sample_block_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_blocks, list)
    assert len(json_blocks) > 0

    # Verify each block has required fields
    for block in json_blocks:
        assert "uid" in block
        assert "duration" in block
        assert isinstance(block["uid"], int)
        assert isinstance(block["duration"], (int, float))


def test_write_to_file(sample_block_writer):  # pylint: disable=redefined-outer-name
    """Test writing block data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_block_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(
    sample_block_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_blocks = sample_block_writer.to_json_array()

    # Check against example from system_c0.json
    for block in json_blocks:
        assert set(block.keys()) == {"uid", "duration", "stage"}
        assert isinstance(block["uid"], int)
        assert isinstance(block["duration"], (int, float))


def test_write_empty_blocks():
    """Test handling of empty block list."""
    # Create parser with no blocks
    parser = BlockParser("dummy.dat")

    writer = BlockWriter(parser)
    json_blocks = writer.to_json_array()
    assert not json_blocks

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
