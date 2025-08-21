#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for DemandWriter class."""

import json
import tempfile
from pathlib import Path
import pandas as pd
import pytest
from ..demand_writer import DemandWriter
from ..demand_parser import DemandParser
from .conftest import get_example_file


@pytest.fixture
def sample_demand_file():
    """Fixture providing path to sample demand file."""
    return get_example_file("plpdem.dat")


@pytest.fixture
def sample_demand_writer(sample_demand_file, tmp_path):
    """Fixture providing initialized DemandWriter with sample data."""
    parser = DemandParser(sample_demand_file)
    parser.parse()
    options = {"output_dir": tmp_path}
    return DemandWriter(parser, options=options)


def test_demand_writer_initialization(
    sample_demand_file, tmp_path
):  # pylint: disable=redefined-outer-name
    """Test DemandWriter initialization."""
    parser = DemandParser(sample_demand_file)
    parser.parse()
    options = {"output_dir": tmp_path}
    writer = DemandWriter(parser, options=options)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_demands


def test_to_json_array(sample_demand_writer):  # pylint: disable=redefined-outer-name
    """Test conversion of demands to JSON array format."""
    json_demands = sample_demand_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_demands, list)
    assert len(json_demands) > 0


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

        # Additional value checks
        assert demand["uid"] > 0, "UID should be positive integer"
        assert len(demand["name"]) > 0, "Name should not be empty"


def test_write_empty_demands(tmp_path):
    """Test handling of empty demand list."""
    # Create parser with no demands
    parser = DemandParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    options = {"output_dir": tmp_path}
    writer = DemandWriter(parser, options=options)

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


def test_to_dataframe_structure(
    sample_demand_writer,
):  # pylint: disable=redefined-outer-name
    """Verify the structure of the DataFrame created by to_dataframe."""
    df, de = sample_demand_writer.to_dataframe()
    assert isinstance(df, pd.DataFrame)
    assert not df.empty
    assert "block" in df.columns
    assert de.empty

    expected_cols = ["block"]
    for item in sample_demand_writer.items:
        if item.get("bus", -1) == 0:
            continue
        if len(item["blocks"]) == 0 or len(item["values"]) == 0:
            continue
        uid = item.get("bus", item["name"])
        name = f"uid:{uid}" if not isinstance(uid, str) else uid
        expected_cols.append(name)
    assert set(df.columns) == set(expected_cols)


def test_to_parquet_creates_file(
    sample_demand_writer, tmp_path
):  # pylint: disable=redefined-outer-name
    """Test that to_parquet creates a Parquet file."""
    sample_demand_writer.to_parquet()
    parquet_file = tmp_path / "Demand" / "lmax.parquet"
    assert parquet_file.exists()
    assert parquet_file.is_file()
