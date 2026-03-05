"""Tests for ts2gtopt – time-series to gtopt schedule projection tool."""

import json
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pandas as pd
import pytest

from ts2gtopt.main import main as ts2gtopt_main

from ts2gtopt.ts2gtopt import (
    _block_hours,
    _get_blocks,
    _get_stage_blocks,
    _get_stages,
    _infer_interval_hours,
    _stage_months,
    convert_timeseries,
    energy_conservation_check,
    load_horizon,
    load_timeseries,
    make_horizon,
    project_timeseries,
    update_horizon_durations,
    write_schedule,
)


# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------


def _make_hourly_df(year: int = 2023) -> pd.DataFrame:
    """Full-year synthetic hourly DataFrame (8760 rows)."""
    idx = pd.date_range(f"{year}-01-01", periods=8760, freq="h")
    rng = np.random.default_rng(42)
    return pd.DataFrame(
        {
            "datetime": idx,
            "uid:1": rng.uniform(0, 100, 8760),
            "uid:2": rng.uniform(0, 200, 8760),
        }
    )


def _make_simple_df() -> pd.DataFrame:
    """Minimal DataFrame: Jan (2 days worth) and Feb, hours 0 and 1."""
    # Jan hour 0: 10, 20  →  mean=15
    # Jan hour 1: 30      →  mean=30
    # Feb hour 0: 40      →  mean=40
    # Feb hour 1: 50, 60  →  mean=55
    dts = [
        pd.Timestamp("2023-01-01 00:00"),
        pd.Timestamp("2023-01-02 00:00"),
        pd.Timestamp("2023-01-01 01:00"),
        pd.Timestamp("2023-02-01 00:00"),
        pd.Timestamp("2023-02-01 01:00"),
        pd.Timestamp("2023-02-02 01:00"),
    ]
    vals1 = [10.0, 20.0, 30.0, 40.0, 50.0, 60.0]
    vals2 = [v * 10 for v in vals1]
    df = pd.DataFrame({"datetime": dts, "uid:1": vals1, "uid:2": vals2})
    df["_month"] = df["datetime"].dt.month
    df["_hour"] = df["datetime"].dt.hour
    df["_year"] = df["datetime"].dt.year
    return df


def _make_two_stage_horizon() -> dict:
    """Horizon: 2 stages (Jan, Feb) × 2 blocks (hours 0 and 1)."""
    return {
        "scenarios": [{"uid": 1}],
        "stages": [
            {"uid": 1, "month": 1},
            {"uid": 2, "month": 2},
        ],
        "blocks": [
            {"uid": 1, "hour": 0, "duration": 1.0},
            {"uid": 2, "hour": 1, "duration": 1.0},
        ],
    }


def _write_timeseries_csv(path: Path, year: int = 2023, seed: int = 0) -> Path:
    """Write a full-year synthetic hourly CSV and return *path*."""
    path.parent.mkdir(parents=True, exist_ok=True)
    idx = pd.date_range(f"{year}-01-01", periods=8760, freq="h")
    rng = np.random.default_rng(seed)
    df = pd.DataFrame({"datetime": idx, "uid:1": rng.uniform(0, 100, 8760)})
    df.to_csv(path, index=False)
    return path


# ---------------------------------------------------------------------------
# make_horizon
# ---------------------------------------------------------------------------


class TestMakeHorizon:
    def test_default_structure(self):
        h = make_horizon(2023)
        assert len(_get_stages(h)) == 12
        assert len(_get_blocks(h)) == 12 * 24  # per-stage blocks

    def test_covers_all_months(self):
        h = make_horizon(2023)
        all_months: set[int] = set()
        for st in _get_stages(h):
            all_months.update(st["months"])
        assert all_months == set(range(1, 13))

    def test_covers_all_hours(self):
        h = make_horizon(2023)
        # Check first stage's blocks cover hours 0-23
        stage0_blocks = _get_stage_blocks(_get_stages(h)[0], _get_blocks(h))
        all_hours: set[int] = set()
        for bl in stage0_blocks:
            all_hours.update(bl["hours"])
        assert all_hours == set(range(24))

    def test_period_conservation(self):
        """Sum of all block durations == 8760 h for a full year (hourly)."""
        h = make_horizon(2023, interval_hours=1.0)
        total = sum(bl["duration"] for bl in _get_blocks(h))
        assert total == pytest.approx(8760.0)

    def test_period_conservation_leap_year(self):
        h = make_horizon(2024, interval_hours=1.0)
        total = sum(bl["duration"] for bl in _get_blocks(h))
        assert total == pytest.approx(8784.0)  # 2024 is a leap year

    def test_period_conservation_4_stages(self):
        """4-season horizon also sums to 8760 h."""
        h = make_horizon(2023, n_stages=4, interval_hours=1.0)
        total = sum(bl["duration"] for bl in _get_blocks(h))
        assert total == pytest.approx(8760.0)

    def test_per_stage_blocks(self):
        """Each stage has its own blocks via first_block/count_block."""
        h = make_horizon(2023)
        stages = _get_stages(h)
        all_blocks = _get_blocks(h)
        for st in stages:
            assert "first_block" in st
            assert "count_block" in st
            blks = _get_stage_blocks(st, all_blocks)
            assert len(blks) == 24

    def test_january_block_duration(self):
        """January blocks should have duration = 31 h (31 days × 1 h)."""
        h = make_horizon(2023, interval_hours=1.0)
        jan_stage = _get_stages(h)[0]  # uid=1 → January
        jan_blocks = _get_stage_blocks(jan_stage, _get_blocks(h))
        for bl in jan_blocks:
            assert bl["duration"] == pytest.approx(31.0)

    def test_february_block_duration_non_leap(self):
        """February 2023 (non-leap): blocks should have duration = 28 h."""
        h = make_horizon(2023, interval_hours=1.0)
        feb_stage = _get_stages(h)[1]  # uid=2 → February
        feb_blocks = _get_stage_blocks(feb_stage, _get_blocks(h))
        for bl in feb_blocks:
            assert bl["duration"] == pytest.approx(28.0)

    def test_february_block_duration_leap(self):
        """February 2024 (leap year): blocks should have duration = 29 h."""
        h = make_horizon(2024, interval_hours=1.0)
        feb_stage = _get_stages(h)[1]
        feb_blocks = _get_stage_blocks(feb_stage, _get_blocks(h))
        for bl in feb_blocks:
            assert bl["duration"] == pytest.approx(29.0)

    def test_interval_hours_scales_durations(self):
        """15-min interval: durations should be 0.25× vs hourly."""
        h_h = make_horizon(2023, interval_hours=1.0)
        h_q = make_horizon(2023, interval_hours=0.25)
        total_h = sum(bl["duration"] for bl in _get_blocks(h_h))
        total_q = sum(bl["duration"] for bl in _get_blocks(h_q))
        assert total_q == pytest.approx(total_h * 0.25)

    def test_seasonal_4_stages(self):
        h = make_horizon(2023, n_stages=4, n_blocks=24)
        assert len(_get_stages(h)) == 4
        assert len(_get_blocks(h)) == 4 * 24

    def test_single_block(self):
        h = make_horizon(2023, n_blocks=1)
        blocks = _get_blocks(h)
        # Each stage has 1 block covering hours 0-23
        assert len(blocks[0]["hours"]) == 24

    def test_year_stored(self):
        h = make_horizon(2024)
        assert h["year"] == 2024

    def test_interval_hours_stored(self):
        h = make_horizon(2023, interval_hours=0.5)
        assert h["interval_hours"] == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# load_horizon
# ---------------------------------------------------------------------------


class TestLoadHorizon:
    def test_load_valid_file(self, tmp_path):
        h = make_horizon(2023, n_stages=2, n_blocks=2)
        p = tmp_path / "horizon.json"
        p.write_text(json.dumps(h), encoding="utf-8")
        loaded = load_horizon(p)
        assert len(_get_stages(loaded)) == 2
        assert len(_get_blocks(loaded)) == 2 * 2

    def test_load_stage_array_key(self, tmp_path):
        """load_horizon accepts stage_array / block_array."""
        h = {
            "stage_array": [{"uid": 1, "month": 1}],
            "block_array": [{"uid": 1, "hour": 0}],
        }
        p = tmp_path / "h.json"
        p.write_text(json.dumps(h), encoding="utf-8")
        loaded = load_horizon(p)
        assert len(_get_stages(loaded)) == 1

    def test_missing_stages_raises(self, tmp_path):
        h = {"blocks": [{"uid": 1, "hour": 0}]}
        p = tmp_path / "bad.json"
        p.write_text(json.dumps(h), encoding="utf-8")
        with pytest.raises(ValueError, match="stages"):
            load_horizon(p)

    def test_missing_blocks_raises(self, tmp_path):
        h = {"stages": [{"uid": 1, "month": 1}]}
        p = tmp_path / "bad.json"
        p.write_text(json.dumps(h), encoding="utf-8")
        with pytest.raises(ValueError, match="blocks"):
            load_horizon(p)


# ---------------------------------------------------------------------------
# Mapping helpers
# ---------------------------------------------------------------------------


class TestStageMappingHelpers:
    def test_single_month(self):
        assert _stage_months({"month": 3}, 0, 12) == [3]

    def test_months_list(self):
        assert _stage_months({"months": [1, 2, 3]}, 0, 12) == [1, 2, 3]

    def test_date_range(self):
        assert _stage_months(
            {"start_date": "2023-03-01", "end_date": "2023-05-31"}, 0, 12
        ) == [3, 4, 5]

    def test_date_range_single_month(self):
        assert _stage_months(
            {"start_date": "2023-06-01", "end_date": "2023-06-30"}, 0, 12
        ) == [6]

    def test_fallback_12_stages(self):
        assert _stage_months({}, 0, 12) == [1]
        assert _stage_months({}, 11, 12) == [12]

    def test_fallback_4_stages(self):
        assert _stage_months({}, 0, 4) == [1, 2, 3]
        assert _stage_months({}, 1, 4) == [4, 5, 6]
        assert _stage_months({}, 2, 4) == [7, 8, 9]
        assert _stage_months({}, 3, 4) == [10, 11, 12]


class TestBlockMappingHelpers:
    def test_single_hour(self):
        assert _block_hours({"hour": 5}, 0, 24) == [5]

    def test_hours_list(self):
        assert _block_hours({"hours": [0, 1, 2]}, 0, 24) == [0, 1, 2]

    def test_hour_range(self):
        assert _block_hours({"start_hour": 6, "end_hour": 8}, 0, 24) == [6, 7, 8]

    def test_fallback_24_blocks(self):
        assert _block_hours({}, 0, 24) == [0]
        assert _block_hours({}, 23, 24) == [23]

    def test_fallback_4_blocks(self):
        assert _block_hours({}, 0, 4) == [0, 1, 2, 3, 4, 5]
        assert _block_hours({}, 3, 4) == [18, 19, 20, 21, 22, 23]

    def test_hour_modulo(self):
        assert _block_hours({"hour": 25}, 0, 24) == [1]


class TestGetStageBlocks:
    def test_shared_blocks(self):
        """Stages without first_block get all blocks."""
        stage = {"uid": 1, "month": 1}
        blocks = [{"uid": i + 1} for i in range(24)]
        result = _get_stage_blocks(stage, blocks)
        assert len(result) == 24

    def test_per_stage_slice(self):
        """Stages with first_block/count_block get their slice."""
        stage = {"uid": 2, "first_block": 24, "count_block": 24}
        blocks = [{"uid": i + 1} for i in range(48)]
        result = _get_stage_blocks(stage, blocks)
        assert len(result) == 24
        assert result[0]["uid"] == 25


# ---------------------------------------------------------------------------
# _infer_interval_hours
# ---------------------------------------------------------------------------


class TestInferIntervalHours:
    def test_hourly(self):
        df = pd.DataFrame({"dt": pd.date_range("2023-01-01", periods=5, freq="h")})
        assert _infer_interval_hours(df, "dt") == pytest.approx(1.0)

    def test_15min(self):
        df = pd.DataFrame({"dt": pd.date_range("2023-01-01", periods=5, freq="15min")})
        assert _infer_interval_hours(df, "dt") == pytest.approx(0.25)

    def test_daily(self):
        df = pd.DataFrame({"dt": pd.date_range("2023-01-01", periods=5, freq="D")})
        assert _infer_interval_hours(df, "dt") == pytest.approx(24.0)

    def test_missing_column(self):
        df = pd.DataFrame({"val": [1, 2, 3]})
        assert _infer_interval_hours(df, "dt") == pytest.approx(1.0)

    def test_single_row(self):
        df = pd.DataFrame({"dt": [pd.Timestamp("2023-01-01")]})
        assert _infer_interval_hours(df, "dt") == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# project_timeseries
# ---------------------------------------------------------------------------


class TestProjectTimeseries:
    def test_basic_shape(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        # 1 scenario × 2 stages × 2 blocks = 4 rows
        assert len(result) == 4

    def test_column_order(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        assert list(result.columns) == [
            "scenario",
            "stage",
            "block",
            "_duration",
            "uid:1",
            "uid:2",
        ]

    def test_mean_values(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        # Jan hour 0: mean of [10, 20] = 15
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["uid:1"] == pytest.approx(15.0)
        assert row["uid:2"] == pytest.approx(150.0)
        # Jan hour 1: single value 30
        row = result[(result["stage"] == 1) & (result["block"] == 2)].iloc[0]
        assert row["uid:1"] == pytest.approx(30.0)
        # Feb hour 1: mean of [50, 60] = 55
        row = result[(result["stage"] == 2) & (result["block"] == 2)].iloc[0]
        assert row["uid:1"] == pytest.approx(55.0)

    def test_duration_column_present(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        assert "_duration" in result.columns

    def test_duration_equals_n_occ_times_interval(self):
        """_duration = n_occurrences × interval_hours."""
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, interval_hours=1.0)
        # Jan hour 0: 2 occurrences × 1h = 2h
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["_duration"] == pytest.approx(2.0)
        # Jan hour 1: 1 occurrence × 1h = 1h
        row = result[(result["stage"] == 1) & (result["block"] == 2)].iloc[0]
        assert row["_duration"] == pytest.approx(1.0)
        # Feb hour 1: 2 occurrences × 1h = 2h
        row = result[(result["stage"] == 2) & (result["block"] == 2)].iloc[0]
        assert row["_duration"] == pytest.approx(2.0)

    def test_period_conservation_full_year(self):
        """Sum of _duration == 8760 h for a full year of hourly data."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result = project_timeseries(df, h, year=2023, interval_hours=1.0)
        total = result["_duration"].sum()
        assert total == pytest.approx(8760.0, rel=1e-6)

    def test_energy_conservation_full_year(self):
        """Σ(value × _duration) == Σ(value × 1h) for a full year."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result = project_timeseries(df, h, year=2023, interval_hours=1.0)

        # Original energy (all element values × 1h per row)
        orig = df[["uid:1", "uid:2"]].sum()

        # Projected energy
        proj1 = (result["uid:1"] * result["_duration"]).sum()
        proj2 = (result["uid:2"] * result["_duration"]).sum()

        assert proj1 == pytest.approx(float(orig["uid:1"]), rel=1e-6)
        assert proj2 == pytest.approx(float(orig["uid:2"]), rel=1e-6)

    def test_index_column_types(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        assert result["scenario"].dtype == "int32"
        assert result["stage"].dtype == "int32"
        assert result["block"].dtype == "int32"

    def test_element_column_types(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        assert result["uid:1"].dtype == "float64"
        assert result["_duration"].dtype == "float64"

    def test_agg_median(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, agg_func="median")
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["uid:1"] == pytest.approx(15.0)

    def test_agg_min(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, agg_func="min")
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["uid:1"] == pytest.approx(10.0)

    def test_agg_max(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, agg_func="max")
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["uid:1"] == pytest.approx(20.0)

    def test_agg_sum(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, agg_func="sum")
        row = result[(result["stage"] == 1) & (result["block"] == 1)].iloc[0]
        assert row["uid:1"] == pytest.approx(30.0)  # 10 + 20

    def test_invalid_agg_raises(self):
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        with pytest.raises(ValueError, match="agg_func"):
            project_timeseries(df, h, agg_func="variance")

    def test_year_filter(self):
        df_multi = pd.concat(
            [_make_hourly_df(2022), _make_hourly_df(2023)], ignore_index=True
        )
        h = make_horizon(2023)
        result = project_timeseries(df_multi, h, year=2023)
        # Only 2023 data → same row count as single-year
        assert len(result) == 12 * 24

    def test_year_filter_wrong_year_raises(self):
        df = _make_hourly_df(2023)
        df["_year"] = 2023
        h = make_horizon(2024)
        with pytest.raises(ValueError, match="year 2024"):
            project_timeseries(df, h, year=2024)

    def test_no_element_columns_raises(self):
        df = pd.DataFrame(
            {
                "datetime": pd.date_range("2023-01-01", periods=3, freq="h"),
                "_month": [1, 1, 1],
                "_hour": [0, 1, 2],
                "_year": [2023, 2023, 2023],
            }
        )
        h = _make_two_stage_horizon()
        with pytest.raises(ValueError, match="No element columns"):
            project_timeseries(df, h)

    def test_multi_scenario(self):
        df = _make_simple_df()
        h = dict(_make_two_stage_horizon())
        h["scenarios"] = [{"uid": 1}, {"uid": 2}]
        result = project_timeseries(df, h)
        assert len(result) == 2 * 2 * 2  # 2 scen × 2 stages × 2 blocks
        assert set(result["scenario"]) == {1, 2}


class TestProjectTimeseriesConservation:
    """Conservation-focused tests for project_timeseries."""

    def test_per_stage_blocks_used(self):
        """project_timeseries uses per-stage blocks from make_horizon."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023)
        result = project_timeseries(df, h, year=2023)
        # 12 stages × 24 blocks (per-stage) = 288 rows
        assert len(result) == 12 * 24

    def test_per_stage_block_uids_are_unique(self):
        """All block UIDs in the 12×24 layout should be distinct per stage."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023)
        result = project_timeseries(df, h, year=2023)
        # Within each stage, block UIDs should increase
        for _, grp in result.groupby("stage"):
            uids = grp["block"].tolist()
            assert uids == sorted(uids)
            assert len(set(uids)) == 24  # 24 unique UIDs per stage

    def test_interval_hours_from_horizon(self):
        """interval_hours is read from horizon when not given explicitly."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result = project_timeseries(df, h, year=2023)
        total = result["_duration"].sum()
        assert total == pytest.approx(8760.0, rel=1e-6)

    def test_interval_hours_explicit_overrides(self):
        """Explicit interval_hours overrides the horizon value."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result_1h = project_timeseries(df, h, year=2023, interval_hours=1.0)
        result_half = project_timeseries(df, h, year=2023, interval_hours=0.5)
        assert result_1h["_duration"].sum() == pytest.approx(
            result_half["_duration"].sum() * 2, rel=1e-6
        )

    def test_period_conservation_full_year_here(self):
        """Duplicate of period conservation test – kept in conservation class."""
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result = project_timeseries(df, h, year=2023, interval_hours=1.0)
        assert result["_duration"].sum() == pytest.approx(8760.0, rel=1e-6)


# ---------------------------------------------------------------------------
# energy_conservation_check
# ---------------------------------------------------------------------------


class TestEnergyConservationCheck:
    def test_mean_gives_ratio_1(self):
        df = _make_hourly_df(2023)
        h = make_horizon(2023, interval_hours=1.0)
        result = project_timeseries(df, h, year=2023, interval_hours=1.0)
        ratios = energy_conservation_check(df, result, interval_hours=1.0)
        for col, ratio in ratios.items():
            assert ratio == pytest.approx(1.0, rel=1e-6), f"{col}: ratio={ratio}"

    def test_missing_duration_raises(self):
        df = _make_hourly_df(2023)
        h = make_horizon(2023)
        sched = project_timeseries(df, h, year=2023)
        sched_no_dur = sched.drop(columns=["_duration"])
        with pytest.raises(ValueError, match="_duration"):
            energy_conservation_check(df, sched_no_dur)

    def test_simple_case(self):
        """Simple: 2 values 10, 20 → mean=15 × 2 occurrences = 30 = sum."""
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h, interval_hours=1.0)
        ratios = energy_conservation_check(df, result, interval_hours=1.0)
        for col, ratio in ratios.items():
            assert ratio == pytest.approx(1.0, rel=1e-6), f"{col}: ratio={ratio}"


# ---------------------------------------------------------------------------
# update_horizon_durations
# ---------------------------------------------------------------------------


class TestUpdateHorizonDurations:
    def test_updates_block_durations(self):
        """update_horizon_durations works correctly for per-stage blocks."""
        # Explicit per-stage horizon: stage 1 → blocks 1,2 (Jan); stage 2 → blocks 3,4 (Feb)
        h = {
            "scenarios": [{"uid": 1}],
            "stages": [
                {"uid": 1, "month": 1, "first_block": 0, "count_block": 2},
                {"uid": 2, "month": 2, "first_block": 2, "count_block": 2},
            ],
            "blocks": [
                {"uid": 1, "hour": 0, "duration": 99.0},  # Jan, hour 0 – placeholder
                {"uid": 2, "hour": 1, "duration": 99.0},  # Jan, hour 1 – placeholder
                {"uid": 3, "hour": 0, "duration": 99.0},  # Feb, hour 0 – placeholder
                {"uid": 4, "hour": 1, "duration": 99.0},  # Feb, hour 1 – placeholder
            ],
        }
        df = _make_simple_df()
        result = project_timeseries(df, h, interval_hours=1.0)
        updated = update_horizon_durations(h, result)
        blocks = {b["uid"]: b for b in _get_blocks(updated)}
        # Jan hour 0: 2 occurrences × 1h = 2.0
        assert blocks[1]["duration"] == pytest.approx(2.0)
        # Jan hour 1: 1 occurrence × 1h = 1.0
        assert blocks[2]["duration"] == pytest.approx(1.0)
        # Feb hour 0: 1 occurrence × 1h = 1.0
        assert blocks[3]["duration"] == pytest.approx(1.0)
        # Feb hour 1: 2 occurrences × 1h = 2.0
        assert blocks[4]["duration"] == pytest.approx(2.0)

    def test_does_not_mutate_original(self):
        h = _make_two_stage_horizon()
        df = _make_simple_df()
        result = project_timeseries(df, h)
        original_dur = _get_blocks(h)[0]["duration"]
        update_horizon_durations(h, result)
        assert _get_blocks(h)[0]["duration"] == original_dur

    def test_no_duration_column_returns_unchanged(self):
        h = _make_two_stage_horizon()
        df = _make_simple_df()
        sched = project_timeseries(df, h).drop(columns=["_duration"])
        updated = update_horizon_durations(h, sched)
        assert _get_blocks(updated)[0]["duration"] == _get_blocks(h)[0]["duration"]


# ---------------------------------------------------------------------------
# write_schedule
# ---------------------------------------------------------------------------


class TestWriteSchedule:
    def _sample_df(self) -> pd.DataFrame:
        return pd.DataFrame(
            {
                "scenario": pd.array([1, 1], dtype="int32"),
                "stage": pd.array([1, 2], dtype="int32"),
                "block": pd.array([1, 1], dtype="int32"),
                "_duration": [31.0, 28.0],
                "uid:1": [10.0, 20.0],
            }
        )

    def test_write_parquet(self, tmp_path):
        df = self._sample_df()
        out = tmp_path / "lmax.parquet"
        write_schedule(df, out, output_format="parquet")
        assert out.exists()
        check_df = pd.read_parquet(out)
        # _duration should be dropped
        assert "_duration" not in check_df.columns
        assert "uid:1" in check_df.columns
        assert len(check_df) == 2

    def test_write_csv(self, tmp_path):
        df = self._sample_df()
        out = tmp_path / "lmax.csv"
        write_schedule(df, out, output_format="csv")
        assert out.exists()
        loaded = pd.read_csv(out)
        assert "_duration" not in loaded.columns
        assert len(loaded) == 2

    def test_creates_parent_dir(self, tmp_path):
        df = self._sample_df()
        out = tmp_path / "Demand" / "lmax.parquet"
        write_schedule(df, out)
        assert out.exists()

    def test_invalid_format_raises(self, tmp_path):
        df = self._sample_df()
        with pytest.raises(ValueError, match="output_format"):
            write_schedule(df, tmp_path / "f.x", output_format="xlsx")

    def test_parquet_roundtrip_dtypes(self, tmp_path):
        df = self._sample_df()
        out = tmp_path / "sched.parquet"
        write_schedule(df, out)
        loaded = pd.read_parquet(out)
        assert loaded["scenario"].dtype == "int32"
        assert loaded["uid:1"].dtype == "float64"

    def test_duration_not_in_schedule_file(self, tmp_path):
        """_duration is planning metadata – never written to schedule files."""
        df = _make_simple_df()
        h = _make_two_stage_horizon()
        result = project_timeseries(df, h)
        out = tmp_path / "sched.parquet"
        write_schedule(result, out)
        loaded = pd.read_parquet(out)
        assert "_duration" not in loaded.columns
        assert "uid:1" in loaded.columns


# ---------------------------------------------------------------------------
# load_timeseries
# ---------------------------------------------------------------------------


class TestLoadTimeseries:
    def test_csv_load(self, tmp_path):
        idx = pd.date_range("2023-01-01", periods=48, freq="h")
        df = pd.DataFrame({"datetime": idx, "val": range(48)})
        csv_path = tmp_path / "ts.csv"
        df.to_csv(csv_path, index=False)
        loaded, col = load_timeseries(csv_path)
        assert col == "datetime"
        assert "_month" in loaded.columns
        assert "_hour" in loaded.columns

    def test_parquet_load(self, tmp_path):
        idx = pd.date_range("2023-03-01", periods=24, freq="h")
        df = pd.DataFrame({"timestamp": idx, "val": range(24)})
        p = tmp_path / "ts.parquet"
        df.to_parquet(p, index=False)
        loaded, col = load_timeseries(p, time_column="timestamp")
        assert "_hour" in loaded.columns
        assert col == "timestamp"

    def test_alias_detection(self, tmp_path):
        idx = pd.date_range("2023-06-01", periods=12, freq="h")
        df = pd.DataFrame({"timestamp": idx, "val": range(12)})
        p = tmp_path / "ts.csv"
        df.to_csv(p, index=False)
        _, col = load_timeseries(p, time_column="datetime")
        assert col == "timestamp"

    def test_missing_time_column_raises(self, tmp_path):
        df = pd.DataFrame({"val": [1, 2, 3]})
        p = tmp_path / "bad.csv"
        df.to_csv(p, index=False)
        with pytest.raises(ValueError, match="not found"):
            load_timeseries(p, time_column="datetime")

    def test_helper_columns_correct(self, tmp_path):
        idx = pd.date_range("2023-07-15 06:00", periods=1, freq="h")
        df = pd.DataFrame({"datetime": idx, "val": [42.0]})
        p = tmp_path / "ts.csv"
        df.to_csv(p, index=False)
        loaded, _ = load_timeseries(p)
        assert loaded["_month"].iloc[0] == 7
        assert loaded["_hour"].iloc[0] == 6
        assert loaded["_year"].iloc[0] == 2023


# ---------------------------------------------------------------------------
# convert_timeseries (high-level)
# ---------------------------------------------------------------------------


class TestConvertTimeseries:
    def _write_ts_csv(self, path: Path, year: int = 2023) -> Path:
        return _write_timeseries_csv(path, year=year, seed=0)

    def test_output_parquet_created(self, tmp_path):
        ts = self._write_ts_csv(tmp_path / "input" / "demand.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts], out_dir, h, year=2023)
        assert "demand" in results
        assert results["demand"].suffix == ".parquet"
        assert results["demand"].exists()

    def test_output_csv(self, tmp_path):
        ts = self._write_ts_csv(tmp_path / "input" / "pmax.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts], out_dir, h, year=2023, output_format="csv")
        assert results["pmax"].suffix == ".csv"

    def test_output_rows_288(self, tmp_path):
        ts = self._write_ts_csv(tmp_path / "input" / "ts.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts], out_dir, h, year=2023)
        df = pd.read_parquet(results["ts"])
        assert len(df) == 12 * 24  # 288

    def test_gtopt_compatible_types(self, tmp_path):
        ts = self._write_ts_csv(tmp_path / "input" / "ts.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts], out_dir, h, year=2023)
        df = pd.read_parquet(results["ts"])
        assert df["scenario"].dtype == "int32"
        assert df["stage"].dtype == "int32"
        assert df["block"].dtype == "int32"
        assert df["uid:1"].dtype == "float64"

    def test_duration_not_in_output_file(self, tmp_path):
        """write_schedule must strip _duration from the Parquet file."""
        ts = self._write_ts_csv(tmp_path / "input" / "ts.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts], out_dir, h, year=2023)
        df = pd.read_parquet(results["ts"])
        assert "_duration" not in df.columns

    def test_output_horizon_written(self, tmp_path):
        ts = self._write_ts_csv(tmp_path / "input" / "ts.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        horizon_out = tmp_path / "horizon_updated.json"
        convert_timeseries([ts], out_dir, h, year=2023, output_horizon_path=horizon_out)
        assert horizon_out.exists()
        loaded = json.loads(horizon_out.read_text())
        assert "stages" in loaded or "stage_array" in loaded

    def test_multiple_inputs(self, tmp_path):
        ts1 = self._write_ts_csv(tmp_path / "input" / "demand.csv")
        ts2 = self._write_ts_csv(tmp_path / "input" / "pmax.csv")
        h = make_horizon(2023)
        out_dir = tmp_path / "output"
        results = convert_timeseries([ts1, ts2], out_dir, h, year=2023)
        assert set(results.keys()) == {"demand", "pmax"}


# ---------------------------------------------------------------------------
# CLI integration
# ---------------------------------------------------------------------------


class TestMainCLI:
    def _write_ts(self, path: Path, year: int = 2023) -> Path:
        return _write_timeseries_csv(path, year=year, seed=7)

    def _run_cli(self, argv):
        with patch("sys.argv", argv):
            try:
                ts2gtopt_main()
            except SystemExit as exc:
                return exc.code
        return 0

    def test_auto_horizon(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        code = self._run_cli(["ts2gtopt", str(ts), "-y", "2023", "-o", str(out)])
        assert code == 0
        result = out / "ts.parquet"
        assert result.exists()
        df = pd.read_parquet(result)
        assert len(df) == 12 * 24
        assert "_duration" not in df.columns

    def test_seasonal_stages(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        code = self._run_cli(
            ["ts2gtopt", str(ts), "-y", "2023", "--stages", "4", "-o", str(out)]
        )
        assert code == 0
        df = pd.read_parquet(out / "ts.parquet")
        assert len(df) == 4 * 24

    def test_csv_output(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        code = self._run_cli(
            ["ts2gtopt", str(ts), "-y", "2023", "-o", str(out), "-f", "csv"]
        )
        assert code == 0
        assert (out / "ts.csv").exists()

    def test_output_horizon(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        h_out = tmp_path / "h.json"
        code = self._run_cli(
            [
                "ts2gtopt",
                str(ts),
                "-y",
                "2023",
                "-o",
                str(out),
                "--output-horizon",
                str(h_out),
            ]
        )
        assert code == 0
        assert h_out.exists()

    def test_planning_file(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        case = {
            "simulation": {
                "stage_array": [
                    {"uid": i + 1, "first_block": i * 24, "count_block": 24}
                    for i in range(12)
                ],
                "block_array": [{"uid": j + 1, "duration": 1.0} for j in range(24)],
                "scenario_array": [{"uid": 1, "probability_factor": 1.0}],
            }
        }
        case_path = tmp_path / "case.json"
        case_path.write_text(json.dumps(case), encoding="utf-8")
        code = self._run_cli(
            ["ts2gtopt", str(ts), "-P", str(case_path), "-y", "2023", "-o", str(out)]
        )
        assert code == 0
        assert (out / "ts.parquet").exists()

    def test_horizon_file(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        h = make_horizon(2023, n_stages=4, n_blocks=6)
        h_path = tmp_path / "h.json"
        h_path.write_text(json.dumps(h), encoding="utf-8")
        code = self._run_cli(
            ["ts2gtopt", str(ts), "-H", str(h_path), "-y", "2023", "-o", str(out)]
        )
        assert code == 0
        df = pd.read_parquet(out / "ts.parquet")
        assert len(df) == 4 * 6

    def test_missing_year_error(self, tmp_path):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        code = self._run_cli(["ts2gtopt", str(ts), "-o", str(out)])
        assert code == 2

    def test_verify_flag(self, tmp_path, capsys):
        ts = self._write_ts(tmp_path / "in" / "ts.csv")
        out = tmp_path / "out"
        code = self._run_cli(
            ["ts2gtopt", str(ts), "-y", "2023", "-o", str(out), "--verify"]
        )
        assert code == 0
        captured = capsys.readouterr()
        assert "conservation" in captured.out.lower() or "ratio" in captured.out.lower()
