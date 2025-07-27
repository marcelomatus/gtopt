"""Unit tests for ManceWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
import pandas as pd
import numpy as np
from ..mance_writer import ManceWriter
from ..mance_parser import ManceParser
from ..central_parser import CentralParser
from ..block_parser import BlockParser
from .conftest import get_example_file


@pytest.fixture
def sample_mance_file():
    """Fixture providing path to sample maintenance file."""
    return get_example_file("plpmance.dat")


@pytest.fixture
def sample_mance_writer(sample_mance_file):
    """Fixture providing initialized ManceWriter with sample data."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    return ManceWriter(parser)


def test_mance_writer_initialization(sample_mance_file):
    """Test ManceWriter initialization."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    writer = ManceWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_mances


def test_to_json_array(sample_mance_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_mances = sample_mance_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_mances, list)
    assert len(json_mances) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "block": list,
        "pmin": list,
        "pmax": list,
    }

    for mance in json_mances:
        for field, field_type in required_fields.items():
            assert field in mance, f"Missing field: {field}"
            assert isinstance(
                mance[field], field_type
            ), f"Field {field} should be {field_type}, got {type(mance[field])}"


def test_write_to_file(sample_mance_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_mance_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_mance_writer):
    """Verify JSON output matches expected structure."""
    json_mances = sample_mance_writer.to_json_array()

    # Expected structure
    REQUIRED_FIELDS = {
        "name": str,
        "block": list,
        "pmin": list,
        "pmax": list,
    }

    for mance in json_mances:
        # Check all required fields exist and have correct types
        assert set(mance.keys()) == set(REQUIRED_FIELDS.keys())
        for field, field_type in REQUIRED_FIELDS.items():
            assert isinstance(
                mance[field], field_type
            ), f"Field {field} should be {field_type}, got {type(mance[field])}"

        # Additional value checks
        assert len(mance["name"]) > 0, "Name should not be empty"
        assert len(mance["block"]) > 0, "Should have at least one block"
        assert len(mance["block"]) == len(
            mance["pmin"]
        ), "Blocks and p_min should match"
        assert len(mance["block"]) == len(
            mance["pmax"]
        ), "Blocks and p_max should match"


def test_write_empty_mances():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = ManceParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access
    parser._num_centrals = 0  # type: ignore[attr-defined]

    writer = ManceWriter(parser)

    # Test empty array conversion
    json_mances = writer.to_json_array()
    assert isinstance(json_mances, list)
    assert len(json_mances) == 0

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

def test_to_dataframe_with_valid_data(sample_mance_writer):
    """Test DataFrame creation with valid maintenance data."""
    df_pmin, df_pmax = sample_mance_writer.to_dataframe()
    
    assert isinstance(df_pmin, pd.DataFrame)
    assert isinstance(df_pmax, pd.DataFrame)
    assert not df_pmin.empty
    assert not df_pmax.empty
    
    # Check DataFrame structure
    assert "block" in df_pmin.columns
    assert "pmin" in df_pmin.columns
    assert "block" in df_pmax.columns 
    assert "pmax" in df_pmax.columns
    
    # Verify data types
    assert df_pmin["block"].dtype == np.int16
    assert df_pmin["pmin"].dtype == np.float64
    assert df_pmax["block"].dtype == np.int16
    assert df_pmax["pmax"].dtype == np.float64

def test_to_dataframe_with_empty_parser():
    """Test DataFrame creation with empty parser."""
    parser = ManceParser("dummy.dat")
    parser._data = []
    writer = ManceWriter(parser)
    
    df_pmin, df_pmax = writer.to_dataframe()
    
    assert df_pmin.empty
    assert df_pmax.empty

def test_to_dataframe_with_missing_blocks(sample_mance_writer):
    """Test DataFrame creation when some blocks are missing."""
    # Remove some block data from the first maintenance entry
    sample_mance_writer.items[0]["block"] = np.array([], dtype=np.int16)
    sample_mance_writer.items[0]["pmin"] = np.array([], dtype=np.float64)
    sample_mance_writer.items[0]["pmax"] = np.array([], dtype=np.float64)
    
    df_pmin, df_pmax = sample_mance_writer.to_dataframe()
    
    # Should still create DataFrames but with fewer rows
    assert not df_pmin.empty
    assert not df_pmax.empty
    assert len(df_pmin) < len(sample_mance_writer.items[1]["block"])
    assert len(df_pmax) < len(sample_mance_writer.items[1]["block"])

def test_to_parquet(tmp_path, sample_mance_writer):
    """Test writing maintenance data to Parquet files."""
    output_dir = tmp_path / "output"
    sample_mance_writer.to_parquet(output_dir)
    
    # Verify files were created
    assert (output_dir / "pmin.parquet").exists()
    assert (output_dir / "pmax.parquet").exists()
    
    # Verify files can be read back
    df_pmin = pd.read_parquet(output_dir / "pmin.parquet")
    df_pmax = pd.read_parquet(output_dir / "pmax.parquet")
    
    assert not df_pmin.empty
    assert not df_pmax.empty

def test_to_dataframe_with_block_parser(sample_mance_file):
    """Test DataFrame creation with block parser."""
    mance_parser = ManceParser(sample_mance_file)
    mance_parser.parse()
    
    # Create mock block parser
    block_parser = BlockParser("dummy.dat")
    block_parser._data = [{"number": 1, "stage": 1, "duration": 1.0}]
    
    writer = ManceWriter(mance_parser, block_parser=block_parser)
    df_pmin, df_pmax = writer.to_dataframe()
    
    assert "stage" in df_pmin.columns
    assert "stage" in df_pmax.columns
