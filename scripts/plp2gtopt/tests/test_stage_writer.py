#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for StageWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..stage_writer import StageWriter
from ..stage_parser import StageParser
from .conftest import get_example_file


@pytest.fixture
def sample_stage_file():
    """Fixture providing path to sample stage file."""
    return get_example_file("plpeta.dat")


@pytest.fixture
def sample_stage_writer(sample_stage_file):
    """Fixture providing initialized StageWriter with sample data."""
    parser = StageParser(sample_stage_file)
    parser.parse()
    return StageWriter(parser)


def test_stage_writer_initialization(
    sample_stage_file,
):  # pylint: disable=redefined-outer-name
    """Test StageWriter initialization."""
    parser = StageParser(sample_stage_file)
    parser.parse()
    writer = StageWriter(parser)

    assert writer.stage_parser == parser
    assert len(writer.stages) == parser.num_stages


def test_to_json_array(sample_stage_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of stages to JSON array format."""
    json_stages = sample_stage_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_stages, list)
    assert len(json_stages) > 0

    # Verify each stage has required fields
    for stage in json_stages:
        assert "uid" in stage
        assert "first_block" in stage
        assert "count_block" in stage
        assert "active" in stage
        assert isinstance(stage["uid"], int)
        assert isinstance(stage["first_block"], int)
        assert isinstance(stage["count_block"], int)
        assert isinstance(stage["active"], int)


def test_write_to_file(sample_stage_writer):  # pylint: disable=redefined-outer-name
    """Test writing stage data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_stage_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_from_stage_file(sample_stage_file):  # pylint: disable=redefined-outer-name
    """Test creating StageWriter directly from stage file."""
    writer = StageWriter.from_stage_file(sample_stage_file)

    # Verify parser was initialized and parsed
    assert writer.stage_parser.file_path == sample_stage_file
    assert writer.stage_parser.num_stages > 0
    assert len(writer.stages) == writer.stage_parser.num_stages


def test_json_output_structure(
    sample_stage_writer,
):  # pylint: disable=redefined-outer-name
    """Verify JSON output matches expected structure."""
    json_stages = sample_stage_writer.to_json_array()

    # Check against example from system_c0.json
    for stage in json_stages:
        assert set(stage.keys()) == {"uid", "first_block", "count_block", "active"}
        assert isinstance(stage["uid"], int)
        assert isinstance(stage["first_block"], int)
        assert isinstance(stage["count_block"], int)
        assert isinstance(stage["active"], int)


def test_write_empty_stages():
    """Test handling of empty stage list."""
    # Create parser with no stages
    parser = StageParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser.num_stages = 0

    writer = StageWriter(parser)
    json_stages = writer.to_json_array()
    assert not json_stages

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert data == []
