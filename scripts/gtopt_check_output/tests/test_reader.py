# SPDX-License-Identifier: BSD-3-Clause
"""Tests for output reader utilities."""

from pathlib import Path

import pandas as pd

from gtopt_check_output._reader import (
    get_block_durations,
    get_demand_info,
    get_generator_info,
    get_generator_profile_info,
    get_line_info,
    read_table,
)


def test_read_table_csv(tmp_path: Path):
    """Reads a CSV file."""
    df = pd.DataFrame({"a": [1, 2], "b": [3, 4]})
    df.to_csv(tmp_path / "data.csv", index=False)
    result = read_table(tmp_path, "data")
    assert result is not None
    assert len(result) == 2


def test_read_table_parquet(tmp_path: Path):
    """Reads a Parquet file."""
    df = pd.DataFrame({"a": [1, 2], "b": [3, 4]})
    df.to_parquet(tmp_path / "data.parquet")
    result = read_table(tmp_path, "data")
    assert result is not None
    assert len(result) == 2


def test_read_table_missing(tmp_path: Path):
    """Returns None for missing files."""
    assert read_table(tmp_path, "nonexistent") is None


def test_get_block_durations():
    """Extracts block UID → duration mapping."""
    planning = {
        "simulation": {
            "block_array": [
                {"uid": 1, "duration": 8.0},
                {"uid": 2, "duration": 16.0},
            ]
        }
    }
    durations = get_block_durations(planning)
    assert durations[1] == 8.0
    assert durations[2] == 16.0


def test_get_block_durations_empty():
    """Empty block array returns empty dict."""
    assert not get_block_durations({"simulation": {}})


def test_get_generator_info():
    """Extracts generator info from planning dict."""
    planning = {
        "system": {
            "generator_array": [
                {"uid": 1, "name": "g1", "type": "solar", "bus": "b1", "pmax": 100.0},
                {
                    "uid": 2,
                    "name": "g2",
                    "type": "thermal",
                    "bus": "b2",
                    "pmax": "pmax",
                },
            ]
        }
    }
    df = get_generator_info(planning)
    assert len(df) == 2
    assert df.iloc[0]["type"] == "solar"
    assert df.iloc[0]["pmax"] == 100.0
    assert df.iloc[1]["pmax"] == 0.0  # file-referenced → 0.0


def test_get_line_info():
    """Extracts line info with tmax fallback."""
    planning = {
        "system": {
            "line_array": [
                {"uid": 1, "name": "L1", "bus_a": "a", "bus_b": "b", "tmax_ab": 200.0},
                {"uid": 2, "name": "L2", "bus_a": "c", "bus_b": "d", "tmax": 150.0},
            ]
        }
    }
    df = get_line_info(planning)
    assert len(df) == 2
    assert df.iloc[0]["tmax"] == 200.0
    assert df.iloc[1]["tmax"] == 150.0


def test_get_demand_info():
    """Extracts demand info."""
    planning = {
        "system": {
            "demand_array": [
                {"uid": 1, "name": "d1", "bus": "b1"},
                {"uid": 2, "name": "d2", "bus": "b2"},
            ]
        }
    }
    df = get_demand_info(planning)
    assert len(df) == 2
    assert df.iloc[0]["name"] == "d1"


def test_get_generator_info_empty():
    """Empty generator array returns empty DataFrame."""
    df = get_generator_info({"system": {}})
    assert len(df) == 0


def test_get_generator_profile_info():
    """Extracts generator profile info with generator name resolution."""
    planning = {
        "system": {
            "generator_array": [
                {"uid": 1, "name": "g1", "type": "solar", "bus": "b1", "pmax": 100.0},
                {"uid": 2, "name": "g2", "type": "wind", "bus": "b2", "pmax": 50.0},
            ],
            "generator_profile_array": [
                {"uid": 1, "name": "gp_solar", "generator": "g1"},
                {"uid": 2, "name": "gp_wind", "generator": 2},
            ],
        }
    }
    df = get_generator_profile_info(planning)
    assert len(df) == 2
    assert df.iloc[0]["name"] == "gp_solar"
    assert df.iloc[0]["generator_uid"] == 1
    assert df.iloc[1]["generator_uid"] == 2


def test_get_generator_profile_info_empty():
    """Empty profile array returns empty DataFrame."""
    df = get_generator_profile_info({"system": {}})
    assert len(df) == 0
