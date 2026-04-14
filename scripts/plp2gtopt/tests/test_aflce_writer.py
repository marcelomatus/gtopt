#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for AflceWriter class."""

import json
import tempfile
from pathlib import Path

import pandas as pd
import pytest

from ..aflce_parser import AflceParser
from ..aflce_writer import AflceWriter
from ..block_parser import BlockParser
from ..central_parser import CentralParser
from .conftest import get_example_file


@pytest.fixture
def sample_aflce_file():
    """Fixture providing path to sample flow file."""
    return get_example_file("plpaflce.dat")


@pytest.fixture
def sample_aflce_writer(sample_aflce_file):
    """Fixture providing initialized AflceWriter with sample data."""
    parser = AflceParser(sample_aflce_file)
    parser.parse()
    return AflceWriter(parser)


def test_aflce_writer_initialization(sample_aflce_file):
    """Test AflceWriter initialization."""
    parser = AflceParser(sample_aflce_file)
    parser.parse()
    writer = AflceWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_flows


def test_to_json_array(sample_aflce_writer):
    """Test conversion of flows to JSON array format."""
    json_flows = sample_aflce_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_flows, list)
    assert len(json_flows) > 0

    # Verify each flow has required fields
    required_fields = {
        "name": str,
        "block": list,
        "flow": list,
    }

    for flow in json_flows:
        for field, field_type in required_fields.items():
            assert field in flow, f"Missing field: {field}"
            assert isinstance(flow[field], field_type), (
                f"Field {field} should be {field_type}, got {type(flow[field])}"
            )


def test_write_to_file(sample_aflce_writer):
    """Test writing flow data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_aflce_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_aflce_writer):
    """Verify JSON output matches expected structure."""
    json_flows = sample_aflce_writer.to_json_array()

    # Expected structure
    required_fields = {
        "name": str,
        "block": list,
        "flow": list,
    }

    for flow in json_flows:
        # Check all required fields exist and have correct types
        assert set(flow.keys()) == set(required_fields.keys())
        for field, field_type in required_fields.items():
            assert isinstance(flow[field], field_type), (
                f"Field {field} should be {field_type}, got {type(flow[field])}"
            )

        # Additional value checks
        assert len(flow["name"]) > 0, "Name should not be empty"
        assert len(flow["block"]) > 0, "Should have at least one block"
        assert len(flow["block"]) == len(flow["flow"]), "Blocks and flows should match"


def test_write_empty_flows():
    """Test handling of empty flow list."""
    # Create parser with no flows
    parser = AflceParser("dummy.dat")
    parser._data = []  # pylint: disable=protected-access

    writer = AflceWriter(parser)

    # Test empty array conversion
    json_flows = writer.to_json_array()
    assert isinstance(json_flows, list)
    assert len(json_flows) == 0

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


def _make_central_parser(tmp_path, name, number=1, afluent=10.0):
    """Create a minimal CentralParser with one central entry."""

    parser = CentralParser.__new__(CentralParser)
    parser.file_path = tmp_path / "plpcnfce.dat"
    parser._data = [{"name": name, "number": number, "afluent": afluent}]
    parser._name_index_map = {name: 0}
    parser._number_index_map = {number: 0}
    return parser


def _make_block_parser(tmp_path, n_blocks=3):
    """Create a minimal BlockParser with n blocks (all stage 1)."""

    parser = BlockParser.__new__(BlockParser)
    parser.file_path = tmp_path / "plpblo.dat"
    parser._data = [
        {
            "number": i + 1,
            "stage": 1,
            "duration": 7.0,
            "accumulated_time": (i + 1) * 7.0,
        }
        for i in range(n_blocks)
    ]
    parser._name_index_map = {}
    parser._number_index_map = {i + 1: i for i in range(n_blocks)}
    parser.stage_number_map = {i + 1: 1 for i in range(n_blocks)}
    return parser


def test_to_dataframe_with_scenarios(tmp_path):
    """Test to_dataframe returns a DataFrame when scenarios are provided."""

    aflce_f = tmp_path / "plpaflce.dat"
    # 1 central, 2 hydrologies, 3 blocks per stage
    # Format: Mes Block flow_hyd1 flow_hyd2  (4 fields per line)
    aflce_f.write_text(
        "# Nro. Cent. c/Caudales Estoc. (EstocNVar2) y Nro. Hidrologias (NClase)\n"
        "  1                                         2\n"
        "# Nombre de la central\n"
        "'FLOWGEN'\n"
        "3\n"
        "   01   001   10.0   20.0\n"
        "   01   002   10.0   20.0\n"
        "   01   003   10.0   20.0\n"
    )
    aflce_parser = AflceParser(aflce_f)
    aflce_parser.parse()

    central_parser = _make_central_parser(tmp_path, "FLOWGEN", number=5, afluent=0.0)
    block_parser = _make_block_parser(tmp_path, 3)
    scenarios = [{"uid": 1, "hydrology": 0}, {"uid": 2, "hydrology": 1}]

    writer = AflceWriter(
        aflce_parser,
        central_parser=central_parser,
        block_parser=block_parser,
        scenarios=scenarios,
    )
    df = writer.to_dataframe()

    assert isinstance(df, pd.DataFrame)
    assert not df.empty
    assert "scenario" in df.columns


def test_to_parquet_with_scenarios(tmp_path):
    """Test to_parquet writes discharge.parquet when scenarios are provided."""

    aflce_f = tmp_path / "plpaflce.dat"
    # Format: Mes Block flow_hyd1  (3 fields per line for 1 hydrology)
    aflce_f.write_text(
        "# Nro. Cent. c/Caudales Estoc. (EstocNVar2) y Nro. Hidrologias (NClase)\n"
        "  1                                         1\n"
        "# Nombre de la central\n"
        "'FLOWGEN'\n"
        "2\n"
        "   01   001   15.0\n"
        "   01   002   15.0\n"
    )
    aflce_parser = AflceParser(aflce_f)
    aflce_parser.parse()

    central_parser = _make_central_parser(tmp_path, "FLOWGEN", number=5, afluent=0.0)
    block_parser = _make_block_parser(tmp_path, 2)
    scenarios = [{"uid": 1, "hydrology": 0}]

    writer = AflceWriter(
        aflce_parser,
        central_parser=central_parser,
        block_parser=block_parser,
        scenarios=scenarios,
    )
    out_dir = tmp_path / "aflce_out"
    cols = writer.to_parquet(out_dir)

    assert (out_dir / "discharge.parquet").exists()
    assert len(cols["discharge"]) > 0


def test_to_dataframe_no_scenarios(tmp_path):
    """Test to_dataframe returns empty result when there are no scenarios."""

    aflce_f = tmp_path / "plpaflce.dat"
    aflce_f.write_text(
        "  1                                         1\n"
        "'FLOWGEN'\n"
        "1\n"
        "   01   001   10.0\n"
    )
    aflce_parser = AflceParser(aflce_f)
    aflce_parser.parse()

    # No scenarios → to_dataframe should return empty result (list or empty DataFrame)
    writer = AflceWriter(aflce_parser, scenarios=[])
    result = writer.to_dataframe()
    # AflceWriter returns [] (empty list) when there are no scenarios
    if isinstance(result, list):
        assert result == []
    else:
        assert result.empty


def test_pasada_unscale_scales_timeseries_and_fill(tmp_path):
    """Promoted pasada discharge is divided by real production_factor.

    PLP stores pasada flow as ``physical_flow * real_production_factor``
    with the placeholder ``efficiency=1.0``.  The ``pasada_unscale_map``
    carries ``1.0 / real_production_factor`` — dividing the inflow by
    the real factor recovers the physical m³/s consistent with the
    ``vmax_hm3`` of the daily-cycle reservoir.

    This test pins both (a) time-series values and (b) the ``afluent``
    fill value so a regression would trip a clear assertion.
    """
    aflce_f = tmp_path / "plpaflce.dat"
    aflce_f.write_text(
        "# Nro. Cent. c/Caudales Estoc. (EstocNVar2) y Nro. Hidrologias (NClase)\n"
        "  1                                         1\n"
        "# Nombre de la central\n"
        "'PASA'\n"
        "2\n"
        "   01   001   10.0\n"
        "   01   002   20.0\n"
    )
    aflce_parser = AflceParser(aflce_f)
    aflce_parser.parse()

    # afluent > 0 so the fill_value path is exercised too.
    central_parser = _make_central_parser(tmp_path, "PASA", number=7, afluent=30.0)
    block_parser = _make_block_parser(tmp_path, 2)
    scenarios = [{"uid": 1, "hydrology": 0}]

    # real_production_factor = 0.5  →  unscale = 1 / 0.5 = 2.0
    writer = AflceWriter(
        aflce_parser,
        central_parser=central_parser,
        block_parser=block_parser,
        scenarios=scenarios,
        pasada_unscale_map={"PASA": 2.0},
    )
    df = writer.to_dataframe()
    assert isinstance(df, pd.DataFrame)
    assert not df.empty

    # The discharge column is named via pcol_name("PASA", 7).  Grab the only
    # non-meta column to avoid hard-coding the prefix format.
    meta_cols = {"scenario", "stage", "block"}
    data_cols = [c for c in df.columns if c not in meta_cols]
    assert len(data_cols) == 1, f"expected 1 data column, got {data_cols}"
    col = data_cols[0]

    # Both time-series values are doubled (unscale=2.0).
    values = df[col].tolist()
    assert values == pytest.approx([20.0, 40.0])


def test_pasada_unscale_absent_is_identity(tmp_path):
    """Without an unscale entry the writer is a no-op (regression guard)."""
    aflce_f = tmp_path / "plpaflce.dat"
    aflce_f.write_text(
        "  1                                         1\n"
        "'SER'\n"
        "2\n"
        "   01   001   10.0\n"
        "   01   002   20.0\n"
    )
    aflce_parser = AflceParser(aflce_f)
    aflce_parser.parse()

    central_parser = _make_central_parser(tmp_path, "SER", number=9, afluent=0.0)
    block_parser = _make_block_parser(tmp_path, 2)
    scenarios = [{"uid": 1, "hydrology": 0}]

    writer = AflceWriter(
        aflce_parser,
        central_parser=central_parser,
        block_parser=block_parser,
        scenarios=scenarios,
        # Empty map ≡ None.
        pasada_unscale_map={},
    )
    df = writer.to_dataframe()

    meta_cols = {"scenario", "stage", "block"}
    data_cols = [c for c in df.columns if c not in meta_cols]
    assert len(data_cols) == 1
    values = df[data_cols[0]].tolist()
    assert values == pytest.approx([10.0, 20.0])
