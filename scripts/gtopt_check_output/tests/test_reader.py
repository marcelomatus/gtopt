# SPDX-License-Identifier: BSD-3-Clause
"""Tests for output reader utilities."""

from pathlib import Path

import pandas as pd

from gtopt_check_output._reader import (
    dataset_layout,
    get_block_durations,
    get_demand_info,
    get_generator_info,
    get_generator_profile_info,
    get_line_info,
    open_dataset,
    read_table,
    streaming_uid_abs_max,
    streaming_uid_stats,
    streaming_uid_sum,
    streaming_uid_sum_per_col,
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


def _write_hive_partition(
    stem_dir: Path, scene: int, phase: int, df: pd.DataFrame
) -> None:
    part = stem_dir / f"scene={scene}" / f"phase={phase}"
    part.mkdir(parents=True)
    df.to_parquet(part / "part.parquet")


def test_read_table_parquet_hive_dataset(tmp_path: Path):
    """Reads a hive-partitioned parquet directory as one frame."""
    stem_dir = tmp_path / "Generator" / "generation_sol.parquet"
    _write_hive_partition(stem_dir, 0, 0, pd.DataFrame({"uid:1": [1.0]}))
    _write_hive_partition(stem_dir, 0, 1, pd.DataFrame({"uid:1": [2.0]}))
    _write_hive_partition(stem_dir, 1, 0, pd.DataFrame({"uid:1": [3.0]}))

    result = read_table(tmp_path, "Generator/generation_sol")
    assert result is not None
    assert len(result) == 3
    assert sorted(result["uid:1"].tolist()) == [1.0, 2.0, 3.0]
    # Partition columns are surfaced automatically.
    assert {"scene", "phase"}.issubset(result.columns)


def test_read_table_csv_shards(tmp_path: Path):
    """Concatenates per-(scene, phase) CSV shards in sorted order."""
    subdir = tmp_path / "Flow"
    subdir.mkdir()
    pd.DataFrame({"uid:1": [10.0]}).to_csv(subdir / "flow_sol_s0_p0.csv", index=False)
    pd.DataFrame({"uid:1": [20.0]}).to_csv(subdir / "flow_sol_s1_p0.csv", index=False)
    result = read_table(tmp_path, "Flow/flow_sol")
    assert result is not None
    assert len(result) == 2


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


# ──────────────────────────────────────────────────────────────────────────
# Layout sniffing + layout-aware streaming aggregators (added 2026-05-19
# alongside `output_layout = long` default flip on the C++ side).
#
# These tests pin the contract that downstream readers can transparently
# consume *either* layout — the sniff branches on schema, never on
# filename or directory layout, so a single parquet writer change cannot
# silently break downstream tooling.
# ──────────────────────────────────────────────────────────────────────────


def _write_wide_dataset(stem_dir: Path) -> None:
    """Emit a tiny `output_layout = wide` dataset: one `uid:N` column per
    element, dense rows over (scenario, stage, block) tuples."""
    df = pd.DataFrame(
        {
            "scenario": [0, 0, 0, 0],
            "stage": [1, 1, 1, 1],
            "block": [1, 2, 3, 4],
            "uid:1": [10.0, 20.0, 30.0, 40.0],
            "uid:2": [5.0, 0.0, 15.0, 0.0],  # uid:2 has two zero entries
        }
    )
    _write_hive_partition(stem_dir, 0, 0, df)


def _write_long_dataset(stem_dir: Path) -> None:
    """Emit a tiny `output_layout = long` dataset equivalent to the wide
    one above, *with the exact-zero rows dropped* (that's the contract
    of the long writer)."""
    df = pd.DataFrame(
        {
            "scenario": [0, 0, 0, 0, 0, 0],
            "stage": [1, 1, 1, 1, 1, 1],
            "block": [1, 2, 3, 4, 1, 3],
            "uid": [1, 1, 1, 1, 2, 2],
            "value": [10.0, 20.0, 30.0, 40.0, 5.0, 15.0],
        }
    )
    _write_hive_partition(stem_dir, 0, 0, df)


def test_dataset_layout_wide(tmp_path: Path):
    """A schema with `uid:N` columns is detected as wide."""
    stem_dir = tmp_path / "Generator" / "generation_sol.parquet"
    _write_wide_dataset(stem_dir)
    ds = open_dataset(tmp_path, "Generator/generation_sol")
    assert ds is not None
    assert dataset_layout(ds) == "wide"


def test_dataset_layout_long(tmp_path: Path):
    """A schema with bare `uid` + `value` columns is detected as long."""
    stem_dir = tmp_path / "Generator" / "generation_sol.parquet"
    _write_long_dataset(stem_dir)
    ds = open_dataset(tmp_path, "Generator/generation_sol")
    assert ds is not None
    assert dataset_layout(ds) == "long"


def test_streaming_uid_sum_layouts_agree(tmp_path: Path):
    """`streaming_uid_sum` returns the same total for wide and long
    representations of the same dispatch (zeros are sum-invariant)."""
    # 10+20+30+40 + 5+0+15+0 = 120.
    expected = 120.0

    wide_dir = tmp_path / "wide"
    wide_dir.mkdir()
    _write_wide_dataset(wide_dir / "Generator" / "generation_sol.parquet")
    ds_wide = open_dataset(wide_dir, "Generator/generation_sol")
    assert ds_wide is not None
    assert streaming_uid_sum(ds_wide) == expected

    long_dir = tmp_path / "long"
    long_dir.mkdir()
    _write_long_dataset(long_dir / "Generator" / "generation_sol.parquet")
    ds_long = open_dataset(long_dir, "Generator/generation_sol")
    assert ds_long is not None
    assert streaming_uid_sum(ds_long) == expected


def test_streaming_uid_sum_per_col_layouts_agree(tmp_path: Path):
    """`streaming_uid_sum_per_col` returns the same per-uid breakdown
    for wide and long representations."""
    expected = {1: 100.0, 2: 20.0}  # 10+20+30+40, 5+0+15+0

    wide_dir = tmp_path / "wide"
    wide_dir.mkdir()
    _write_wide_dataset(wide_dir / "Generator" / "generation_sol.parquet")
    ds_wide = open_dataset(wide_dir, "Generator/generation_sol")
    assert ds_wide is not None
    assert streaming_uid_sum_per_col(ds_wide) == expected

    long_dir = tmp_path / "long"
    long_dir.mkdir()
    _write_long_dataset(long_dir / "Generator" / "generation_sol.parquet")
    ds_long = open_dataset(long_dir, "Generator/generation_sol")
    assert ds_long is not None
    assert streaming_uid_sum_per_col(ds_long) == expected


def test_streaming_uid_abs_max_layouts_agree(tmp_path: Path):
    """`streaming_uid_abs_max` returns the same max-abs value for both
    layouts."""
    wide_dir = tmp_path / "wide"
    wide_dir.mkdir()
    _write_wide_dataset(wide_dir / "Generator" / "generation_sol.parquet")
    ds_wide = open_dataset(wide_dir, "Generator/generation_sol")
    assert ds_wide is not None
    assert streaming_uid_abs_max(ds_wide) == 40.0

    long_dir = tmp_path / "long"
    long_dir.mkdir()
    _write_long_dataset(long_dir / "Generator" / "generation_sol.parquet")
    ds_long = open_dataset(long_dir, "Generator/generation_sol")
    assert ds_long is not None
    assert streaming_uid_abs_max(ds_long) == 40.0


def test_streaming_uid_stats_long_count_excludes_zeros(tmp_path: Path):
    """For the long form, `count` reflects only non-zero rows (zeros are
    dropped at write time).  For the wide form, `count` includes the
    zero rows because they're stored explicitly.  Both forms produce the
    same `sum`."""
    wide_dir = tmp_path / "wide"
    wide_dir.mkdir()
    _write_wide_dataset(wide_dir / "Generator" / "generation_sol.parquet")
    ds_wide = open_dataset(wide_dir, "Generator/generation_sol")
    assert ds_wide is not None
    stats_wide = streaming_uid_stats(ds_wide)
    assert stats_wide["sum"] == 120.0
    assert stats_wide["count"] == 8  # 4 rows × 2 uid columns

    long_dir = tmp_path / "long"
    long_dir.mkdir()
    _write_long_dataset(long_dir / "Generator" / "generation_sol.parquet")
    ds_long = open_dataset(long_dir, "Generator/generation_sol")
    assert ds_long is not None
    stats_long = streaming_uid_stats(ds_long)
    assert stats_long["sum"] == 120.0
    assert stats_long["count"] == 6  # 8 wide cells − 2 dropped zeros
