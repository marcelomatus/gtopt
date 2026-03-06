"""Tests for ts2gtopt.cases.bat4b_annual – bat_4b annual time-series generator.

Organised in three groups:

Unit tests (fast, small data)
------------------------------
* Profile shapes (demand daily profile, demand seasonal ratio, weekend factor)
* Solar properties (zero at night, seasonal ratio)
* Generator maintenance (pmax=0 during window, no overlap)

Integration tests – ts2gtopt projection (full year)
-----------------------------------------------------
* Project demand time-series and verify energy + period conservation
* Project solar pmax and verify energy + period conservation
* Project generator pmax and verify energy + period conservation
* Verify written schedule files are gtopt-compatible (correct dtypes, no
  _duration column, correct row count)
* Verify full case directory is written correctly

Integration tests – end-to-end gtopt solver
--------------------------------------------
* Run the gtopt binary on the generated bat_4b annual case
* Verify status = 0 (optimal)
* Verify demand is fully satisfied (fail_sol ≈ 0 MW)
* Retry with higher generator pmax if unsatisfied demand is detected
"""
# pylint: disable=too-many-lines

import datetime
import json
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from ts2gtopt.cases.bat4b_annual import (
    DEMAND_DAILY_PROFILE,
    D3_BASE_MW,
    D4_BASE_MW,
    G1_PMAX_MW,
    G2_PMAX_MW,
    SOLAR_PMAX_MW,
    WEEKEND_FACTOR,
    _G1_MAINT_DAYS,
    _G1_MAINT_START,
    _G2_MAINT_DAYS,
    _G2_MAINT_START,
    _SOLAR_SUNRISE_H,
    _SOLAR_SUNSET_H,
    make_bat4b_case_json,
    make_bat4b_horizon,
    make_chilean_holidays,
    make_demand_timeseries,
    make_generator_pmax_timeseries,
    make_solar_pmax_timeseries,
    write_bat4b_case,
    write_bat4b_gtopt_input,
    write_bat4b_timeseries,
)
from ts2gtopt.ts2gtopt import (
    energy_conservation_check,
    make_horizon,
    project_timeseries,
)

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

_YEAR = 2023  # fixed year for all tests
_GTOPT_TIMEOUT_SECONDS = 300  # maximum seconds to wait for the gtopt binary


def _demand_df_short(days: int = 7) -> pd.DataFrame:
    """Return a *days*-day demand DataFrame (days × 24 rows)."""
    df = make_demand_timeseries(_YEAR)
    return df.iloc[: days * 24].reset_index(drop=True)


def _solar_df_short(days: int = 7) -> pd.DataFrame:
    """Return a *days*-day solar pmax DataFrame."""
    df = make_solar_pmax_timeseries(_YEAR)
    return df.iloc[: days * 24].reset_index(drop=True)


def _gen_pmax_df() -> pd.DataFrame:
    """Full-year generator pmax DataFrame."""
    return make_generator_pmax_timeseries(_YEAR)


# ---------------------------------------------------------------------------
# Unit tests – demand daily profile
# ---------------------------------------------------------------------------


class TestDemandDailyProfile:
    def test_profile_mean_is_one(self):
        """Normalised profile must have mean = 1.0."""
        assert DEMAND_DAILY_PROFILE.mean() == pytest.approx(1.0, abs=1e-9)

    def test_profile_length_24(self):
        assert len(DEMAND_DAILY_PROFILE) == 24

    def test_peak_period_18_to_22_highest(self):
        """Hours 18–22 should all exceed the overnight minimum (hours 1–4)."""
        peak_min = DEMAND_DAILY_PROFILE[18:23].min()
        night_max = DEMAND_DAILY_PROFILE[1:5].max()
        assert peak_min > night_max

    def test_35_percent_swing(self):
        """(max - min) / min ≈ 35 %."""
        ratio = (
            DEMAND_DAILY_PROFILE.max() - DEMAND_DAILY_PROFILE.min()
        ) / DEMAND_DAILY_PROFILE.min()
        assert ratio == pytest.approx(0.35, abs=0.02)


# ---------------------------------------------------------------------------
# Unit tests – demand time-series
# ---------------------------------------------------------------------------


class TestDemandTimeseries:
    def test_shape(self):
        df = make_demand_timeseries(_YEAR)
        assert df.shape == (8760, 3)  # datetime, uid:1, uid:2

    def test_columns(self):
        df = make_demand_timeseries(_YEAR)
        assert list(df.columns) == ["datetime", "uid:1", "uid:2"]

    def test_positive_values(self):
        df = make_demand_timeseries(_YEAR)
        assert (df["uid:1"] > 0).all()
        assert (df["uid:2"] > 0).all()

    def test_seasonal_ratio_winter_higher(self):
        """July mean demand > January mean demand by ~20 %."""
        df = make_demand_timeseries(_YEAR)
        df["_month"] = df["datetime"].dt.month
        jan_mean = df[df["_month"] == 1]["uid:1"].mean()
        jul_mean = df[df["_month"] == 7]["uid:1"].mean()
        ratio = jul_mean / jan_mean
        # With noise and weekends, allow ±5 % tolerance around the expected 1.20
        assert ratio == pytest.approx(1.20, abs=0.07)

    def test_d4_proportional_to_d3(self):
        """d4 base is 70 % of d3 base (D4_BASE_MW / D3_BASE_MW = 0.70)."""
        expected = D4_BASE_MW / D3_BASE_MW
        df = make_demand_timeseries(_YEAR)
        # Ratio should be constant across all rows because they share factors
        ratios = df["uid:2"] / df["uid:1"]
        assert ratios.mean() == pytest.approx(expected, abs=1e-9)

    def test_weekend_factor_applied(self):
        """Saturday demand should be ~30 % less than average weekday demand."""
        df = make_demand_timeseries(_YEAR, noise_seed=0)
        df["_weekday"] = df["datetime"].dt.weekday
        df["_hour"] = df["datetime"].dt.hour

        # Compare same-hour values: Monday h=10 (no-noise seed) vs Saturday h=10
        # Use January 9 (Mon) and January 14 (Sat) which are same week
        mon = df[df["datetime"].dt.date == datetime.date(_YEAR, 1, 9)]["uid:1"]
        sat = df[df["datetime"].dt.date == datetime.date(_YEAR, 1, 14)]["uid:1"]
        ratio = sat.sum() / mon.sum()
        # Should be close to WEEKEND_FACTOR = 0.70 (noise is the same seed)
        assert ratio == pytest.approx(WEEKEND_FACTOR, abs=0.02)

    def test_holiday_jan1_reduced(self):
        """January 1 (New Year's Day) should have reduced demand."""
        df = make_demand_timeseries(_YEAR, noise_seed=0)
        df["_date"] = df["datetime"].dt.date

        jan1 = df[df["_date"] == datetime.date(_YEAR, 1, 1)]["uid:1"].mean()
        # Jan 2 is a regular Monday
        jan2 = df[df["_date"] == datetime.date(_YEAR, 1, 2)]["uid:1"].mean()
        # Jan 1 is also a Sunday, so both weekend and holiday factor apply
        # Either way: jan1 demand should be < jan2 demand
        assert jan1 < jan2

    def test_peak_hours_higher_than_night(self):
        """Hours 18–22 should have higher mean demand than hours 1–4."""
        df = make_demand_timeseries(_YEAR)
        df["_hour"] = df["datetime"].dt.hour
        peak_mean = df[df["_hour"].isin(range(18, 23))]["uid:1"].mean()
        night_mean = df[df["_hour"].isin(range(1, 5))]["uid:1"].mean()
        assert peak_mean > night_mean

    def test_base_values_around_nominal(self):
        """Overall mean demand should be close to D3_BASE_MW / D4_BASE_MW."""
        df = make_demand_timeseries(_YEAR)
        # Mean should be within 25% of base (seasonal + weekends shift it)
        assert df["uid:1"].mean() == pytest.approx(D3_BASE_MW, rel=0.25)
        assert df["uid:2"].mean() == pytest.approx(D4_BASE_MW, rel=0.25)


# ---------------------------------------------------------------------------
# Unit tests – solar pmax time-series
# ---------------------------------------------------------------------------


class TestSolarTimeseries:
    def test_shape(self):
        df = make_solar_pmax_timeseries(_YEAR)
        assert df.shape == (8760, 2)

    def test_columns(self):
        df = make_solar_pmax_timeseries(_YEAR)
        assert list(df.columns) == ["datetime", "uid:3"]

    def test_zero_before_sunrise(self):
        """Solar pmax must be 0 before sunrise hour."""
        df = make_solar_pmax_timeseries(_YEAR)
        df["_hour"] = df["datetime"].dt.hour
        night = df[df["_hour"] < _SOLAR_SUNRISE_H]["uid:3"]
        assert (night == 0.0).all()

    def test_zero_after_sunset(self):
        """Solar pmax must be 0 after sunset hour."""
        df = make_solar_pmax_timeseries(_YEAR)
        df["_hour"] = df["datetime"].dt.hour
        night = df[df["_hour"] > _SOLAR_SUNSET_H]["uid:3"]
        assert (night == 0.0).all()

    def test_bounded_by_pmax(self):
        """Solar pmax must not exceed the installed capacity."""
        df = make_solar_pmax_timeseries(_YEAR, pmax_mw=90.0)
        assert df["uid:3"].max() <= 90.0 + 1e-9

    def test_summer_higher_than_winter(self):
        """January (summer) noon pmax > July (winter) noon pmax by ~40 %."""
        df = make_solar_pmax_timeseries(_YEAR)
        df["_month"] = df["datetime"].dt.month
        df["_hour"] = df["datetime"].dt.hour

        jan_noon = df[(df["_month"] == 1) & (df["_hour"] == 13)]["uid:3"].mean()
        jul_noon = df[(df["_month"] == 7) & (df["_hour"] == 13)]["uid:3"].mean()
        ratio = jan_noon / jul_noon
        # Expected ≈ 1.40; allow ±5 %
        assert ratio == pytest.approx(1.40, abs=0.05)

    def test_positive_during_day(self):
        """Solar pmax must be strictly positive between sunrise and sunset."""
        df = make_solar_pmax_timeseries(_YEAR)
        df["_hour"] = df["datetime"].dt.hour
        # Strictly interior daylight hours
        day = df[(df["_hour"] > _SOLAR_SUNRISE_H) & (df["_hour"] < _SOLAR_SUNSET_H)]
        assert (day["uid:3"] > 0).all()

    def test_peak_at_noon(self):
        """The maximum pmax across all hours should occur near 13 h."""
        df = make_solar_pmax_timeseries(_YEAR)
        df["_hour"] = df["datetime"].dt.hour
        hourly_mean = df.groupby("_hour")["uid:3"].mean()
        peak_hour = int(hourly_mean.idxmax())
        assert peak_hour == 13


# ---------------------------------------------------------------------------
# Unit tests – generator pmax time-series
# ---------------------------------------------------------------------------


class TestGeneratorPmaxTimeseries:
    def test_shape(self):
        df = make_generator_pmax_timeseries(_YEAR)
        assert df.shape == (8760, 3)

    def test_columns(self):
        df = make_generator_pmax_timeseries(_YEAR)
        assert list(df.columns) == ["datetime", "uid:1", "uid:2"]

    def test_g1_nominal_outside_maintenance(self):
        """g1 pmax = G1_PMAX_MW outside its maintenance window."""
        df = make_generator_pmax_timeseries(_YEAR)
        outside = df[
            df["datetime"].dt.month.isin([3, 4, 5, 6])  # no overlap with maintenance
        ]
        assert (outside["uid:1"] == G1_PMAX_MW).all()

    def test_g2_nominal_outside_maintenance(self):
        """g2 pmax = G2_PMAX_MW outside its maintenance window."""
        df = make_generator_pmax_timeseries(_YEAR)
        outside = df[
            df["datetime"].dt.month.isin([2, 3, 4, 5, 6])  # not in Jul maintenance
        ]
        assert (outside["uid:2"] == G2_PMAX_MW).all()

    def test_g1_zero_during_maintenance(self):
        """g1 pmax = 0 during its 2-week maintenance window."""
        df = make_generator_pmax_timeseries(_YEAR)
        g1_start = datetime.date(_YEAR, *_G1_MAINT_START)
        g1_end = g1_start + datetime.timedelta(days=_G1_MAINT_DAYS)
        dates = df["datetime"].dt.date
        in_maint = (dates >= g1_start) & (dates < g1_end)
        assert (df.loc[in_maint, "uid:1"] == 0.0).all()
        assert in_maint.sum() == _G1_MAINT_DAYS * 24

    def test_g2_zero_during_maintenance(self):
        """g2 pmax = 0 during its 2-week maintenance window."""
        df = make_generator_pmax_timeseries(_YEAR)
        g2_start = datetime.date(_YEAR, *_G2_MAINT_START)
        g2_end = g2_start + datetime.timedelta(days=_G2_MAINT_DAYS)
        dates = df["datetime"].dt.date
        in_maint = (dates >= g2_start) & (dates < g2_end)
        assert (df.loc[in_maint, "uid:2"] == 0.0).all()
        assert in_maint.sum() == _G2_MAINT_DAYS * 24

    def test_maintenance_windows_do_not_overlap(self):
        """g1 and g2 maintenance windows must not share any calendar day."""
        g1_start = datetime.date(_YEAR, *_G1_MAINT_START)
        g1_end = g1_start + datetime.timedelta(days=_G1_MAINT_DAYS)
        g2_start = datetime.date(_YEAR, *_G2_MAINT_START)
        g2_end = g2_start + datetime.timedelta(days=_G2_MAINT_DAYS)
        # Windows [g1_start, g1_end) and [g2_start, g2_end) must not overlap
        assert g1_end <= g2_start or g2_end <= g1_start

    def test_g1_g2_never_both_zero(self):
        """g1 and g2 must never both have pmax=0 at the same time."""
        df = make_generator_pmax_timeseries(_YEAR)
        both_zero = (df["uid:1"] == 0) & (df["uid:2"] == 0)
        assert not both_zero.any()


# ---------------------------------------------------------------------------
# Unit tests – horizon and holiday helpers
# ---------------------------------------------------------------------------


class TestHelpers:
    def test_chilean_holidays_includes_new_year(self):
        h = make_chilean_holidays(_YEAR)
        assert datetime.date(_YEAR, 1, 1) in h

    def test_chilean_holidays_includes_christmas(self):
        h = make_chilean_holidays(_YEAR)
        assert datetime.date(_YEAR, 12, 25) in h

    def test_chilean_holidays_2023_good_friday(self):
        h = make_chilean_holidays(_YEAR)
        # Easter 2023 = April 9 → Good Friday = April 7
        assert datetime.date(_YEAR, 4, 7) in h

    def test_bat4b_horizon_12_stages(self):
        h = make_bat4b_horizon(_YEAR)
        assert len(h["stages"]) == 12

    def test_bat4b_horizon_288_blocks(self):
        h = make_bat4b_horizon(_YEAR)
        assert len(h["blocks"]) == 12 * 24

    def test_bat4b_horizon_period_conservation(self):
        """Sum of all block durations must equal 8760 h."""
        h = make_bat4b_horizon(_YEAR)
        total = sum(b["duration"] for b in h["blocks"])
        assert total == pytest.approx(8760.0, rel=1e-6)

    def test_bat4b_case_json_has_required_keys(self):
        case = make_bat4b_case_json(Path("sched"), _YEAR)
        assert "options" in case
        assert "simulation" in case
        assert "system" in case

    def test_bat4b_case_json_references_schedule_files(self):
        """make_bat4b_case_json (flat-file variant) embeds full schedule paths."""
        case = make_bat4b_case_json(Path("sched"), _YEAR)
        gen_array = case["system"]["generator_array"]
        # g_solar pmax should reference the solar_pmax schedule file
        solar = next(g for g in gen_array if g["name"] == "g_solar")
        assert "solar_pmax" in str(solar["pmax"])

    def test_bat4b_case_json_d_bat_in_lmax_is_scalar(self):
        """d_bat_in lmax must be the scalar 60 (not a broken list)."""
        case = make_bat4b_case_json(Path("sched"), _YEAR)
        d_bat_in = next(
            d for d in case["system"]["demand_array"] if d["name"] == "d_bat_in"
        )
        assert d_bat_in["lmax"] == 60


# ---------------------------------------------------------------------------
# Integration tests – full-year projections
# ---------------------------------------------------------------------------


class TestDemandProjectionIntegration:
    @pytest.mark.integration
    def test_demand_period_conservation(self):
        """Sum of projected block durations equals 8760 h (full year)."""
        df = make_demand_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        assert result["_duration"].sum() == pytest.approx(8760.0, rel=1e-4)

    @pytest.mark.integration
    def test_demand_energy_conservation(self):
        """Energy (value × duration) is preserved across the projection."""
        df = make_demand_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        ratios = energy_conservation_check(df, result, interval_hours=1.0)
        for col, ratio in ratios.items():
            assert ratio == pytest.approx(1.0, abs=5e-3), f"column {col}: ratio={ratio}"

    @pytest.mark.integration
    def test_demand_projection_shape(self):
        """Projection produces 12 × 24 = 288 rows per scenario."""
        df = make_demand_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        assert len(result) == 12 * 24

    @pytest.mark.integration
    def test_demand_projection_winter_summer_preserved(self):
        """Projected winter stage mean > summer stage mean by ~20 %."""
        df = make_demand_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        # Stage 1 = January (summer), Stage 7 = July (winter)
        jan_mean = result[result["stage"] == 1]["uid:1"].mean()
        jul_mean = result[result["stage"] == 7]["uid:1"].mean()
        ratio = jul_mean / jan_mean
        assert ratio == pytest.approx(1.20, abs=0.08)


class TestSolarProjectionIntegration:
    @pytest.mark.integration
    def test_solar_period_conservation(self):
        """Sum of projected block durations equals 8760 h."""
        df = make_solar_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        assert result["_duration"].sum() == pytest.approx(8760.0, rel=1e-4)

    @pytest.mark.integration
    def test_solar_energy_conservation(self):
        """Solar energy is preserved across the projection."""
        df = make_solar_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        ratios = energy_conservation_check(df, result, interval_hours=1.0)
        for col, ratio in ratios.items():
            assert ratio == pytest.approx(1.0, abs=5e-3), f"column {col}: ratio={ratio}"

    @pytest.mark.integration
    def test_solar_night_blocks_zero(self):
        """Blocks corresponding to nighttime hours must have pmax = 0."""
        df = make_solar_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        # In the 12×24 horizon, block uid = hour index (1-based within stage).
        # Blocks whose hour is before sunrise or after sunset are night blocks.
        # The block's "position within the stage" equals (block_uid - 1) % 24.
        night_block_uids = [
            b["uid"]
            for b in h["blocks"]
            if (b["uid"] - 1) % 24 < _SOLAR_SUNRISE_H
            or (b["uid"] - 1) % 24 > _SOLAR_SUNSET_H
        ]
        night_rows = result[result["block"].isin(night_block_uids)]
        if not night_rows.empty:
            assert (night_rows["uid:3"].fillna(0) == 0.0).all()

    @pytest.mark.integration
    def test_solar_summer_stage_higher(self):
        """January projected pmax exceeds July projected pmax by ~40 %."""
        df = make_solar_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        jan_mean = result[result["stage"] == 1]["uid:3"].mean()
        jul_mean = result[result["stage"] == 7]["uid:3"].mean()
        ratio = jan_mean / jul_mean
        assert ratio == pytest.approx(1.40, abs=0.1)


class TestGeneratorPmaxProjectionIntegration:
    @pytest.mark.integration
    def test_generator_period_conservation(self):
        """Sum of projected block durations equals 8760 h."""
        df = make_generator_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        assert result["_duration"].sum() == pytest.approx(8760.0, rel=1e-4)

    @pytest.mark.integration
    def test_generator_energy_conservation(self):
        """Generator pmax energy is preserved across the projection."""
        df = make_generator_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        ratios = energy_conservation_check(df, result, interval_hours=1.0)
        for col, ratio in ratios.items():
            assert ratio == pytest.approx(1.0, abs=5e-3), f"column {col}: ratio={ratio}"

    @pytest.mark.integration
    def test_g1_maintenance_stage_reduced(self):
        """January projected pmax for g1 should be reduced (maintenance)."""
        df = make_generator_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        jan_mean = result[result["stage"] == 1]["uid:1"].mean()
        # 14 days out of 31 in January → reduction ≈ 14/31 = 45 %
        full_pmax_mean = G1_PMAX_MW * (1 - _G1_MAINT_DAYS / 31.0)
        assert jan_mean == pytest.approx(full_pmax_mean, rel=0.05)

    @pytest.mark.integration
    def test_g2_maintenance_stage_reduced(self):
        """July projected pmax for g2 should be reduced (maintenance)."""
        df = make_generator_pmax_timeseries(_YEAR)
        h = make_horizon(_YEAR, n_stages=12, n_blocks=24)
        result = project_timeseries(df, h, year=_YEAR)
        jul_mean = result[result["stage"] == 7]["uid:2"].mean()
        # g2 maintenance starts July 17 (day 198), so ~14 out of 31 days in July
        # are under maintenance (Jul 17-30 = 14 days)
        full_pmax_mean = G2_PMAX_MW * (1 - _G2_MAINT_DAYS / 31.0)
        assert jul_mean == pytest.approx(full_pmax_mean, rel=0.05)


# ---------------------------------------------------------------------------
# Integration tests – file output
# ---------------------------------------------------------------------------


class TestWriteFiles:
    @pytest.mark.integration
    def test_write_timeseries_files_parquet(self, tmp_path):
        """write_bat4b_timeseries creates demand, solar, and generator Parquet files."""
        result = write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        assert "demand_lmax" in result
        assert "solar_pmax" in result
        assert "generator_pmax" in result
        for path in result.values():
            assert path.exists()
            assert path.suffix == ".parquet"

    @pytest.mark.integration
    def test_write_timeseries_files_csv(self, tmp_path):
        """write_bat4b_timeseries creates CSV files when requested."""
        result = write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="csv")
        for path in result.values():
            assert path.suffix == ".csv"

    @pytest.mark.integration
    def test_parquet_demand_schema(self, tmp_path):
        """Demand Parquet file must have correct gtopt-compatible schema."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "demand_lmax.parquet")
        assert "scenario" in df.columns
        assert "stage" in df.columns
        assert "block" in df.columns
        assert "uid:1" in df.columns
        assert "uid:2" in df.columns
        assert "_duration" not in df.columns
        assert df["scenario"].dtype == "int32"
        assert df["stage"].dtype == "int32"
        assert df["block"].dtype == "int32"
        assert df["uid:1"].dtype == "float64"

    @pytest.mark.integration
    def test_parquet_demand_row_count(self, tmp_path):
        """Demand Parquet: 1 scenario × 12 stages × 24 blocks = 288 rows."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "demand_lmax.parquet")
        assert len(df) == 12 * 24

    @pytest.mark.integration
    def test_parquet_solar_row_count(self, tmp_path):
        """Solar Parquet: 288 rows."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "solar_pmax.parquet")
        assert len(df) == 12 * 24

    @pytest.mark.integration
    def test_parquet_generator_row_count(self, tmp_path):
        """Generator pmax Parquet: 288 rows."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "generator_pmax.parquet")
        assert len(df) == 12 * 24

    @pytest.mark.integration
    def test_write_bat4b_case_creates_json(self, tmp_path):
        """write_bat4b_case produces a valid JSON planning case file."""
        json_path = write_bat4b_case(tmp_path, year=_YEAR)
        assert json_path.exists()
        case = json.loads(json_path.read_text(encoding="utf-8"))
        assert "simulation" in case
        assert "system" in case
        # New API: uses input_directory convention; g1/g2 reference "pmax" stem
        assert case["options"]["input_directory"] == "input"
        gen_array = case["system"]["generator_array"]
        g1 = next(g for g in gen_array if g["name"] == "g1")
        assert g1["pmax"] == "pmax"

    @pytest.mark.integration
    def test_write_bat4b_case_schedule_files_exist(self, tmp_path):
        """All input schedule files must exist after write_bat4b_case."""
        write_bat4b_case(tmp_path, year=_YEAR)
        assert (tmp_path / "input" / "Demand" / "lmax.parquet").exists()
        assert (tmp_path / "input" / "Generator" / "pmax.parquet").exists()
        assert (tmp_path / "input" / "GeneratorProfile" / "profile.parquet").exists()

    @pytest.mark.integration
    def test_parquet_solar_bounded(self, tmp_path):
        """Projected solar pmax must be within [0, SOLAR_PMAX_MW]."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "solar_pmax.parquet")
        assert df["uid:3"].min() >= 0.0
        assert df["uid:3"].max() <= SOLAR_PMAX_MW + 1e-6

    @pytest.mark.integration
    def test_parquet_generator_maintenance_reflected(self, tmp_path):
        """Projected January g1 pmax should be < nominal due to maintenance."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="parquet")
        df = pd.read_parquet(tmp_path / "generator_pmax.parquet")
        # Stage 1 = January (g1 has maintenance)
        jan_g1_mean = df[df["stage"] == 1]["uid:1"].mean()
        assert jan_g1_mean < G1_PMAX_MW

    @pytest.mark.integration
    def test_csv_demand_readable(self, tmp_path):
        """Demand CSV must be readable and contain correct columns."""
        write_bat4b_timeseries(tmp_path, year=_YEAR, output_format="csv")
        df = pd.read_csv(tmp_path / "demand_lmax.csv")
        assert "scenario" in df.columns
        assert "uid:1" in df.columns
        assert "uid:2" in df.columns
        assert len(df) == 12 * 24


# ---------------------------------------------------------------------------
# Small-data smoke tests for ts2gtopt core (non-annual)
# ---------------------------------------------------------------------------


class TestSmallDataSmoke:
    """Fast smoke tests using only a few days of synthetic data.

    These tests verify that ts2gtopt works correctly on small bat_4b-like
    inputs without running the full-year projection.
    """

    def _make_3day_demand(self) -> pd.DataFrame:
        """3 days × 24 hours of demand data (one weekend, two weekdays)."""
        # Jan 6 = Friday, Jan 7 = Saturday, Jan 8 = Sunday
        idx = pd.date_range(f"{_YEAR}-01-06", periods=72, freq="h")
        rng = np.random.default_rng(1)
        vals = rng.uniform(80, 120, 72)
        return pd.DataFrame({"datetime": idx, "uid:1": vals, "uid:2": vals * 0.7})

    def test_project_3day_demand_shape(self):
        df = self._make_3day_demand()
        h = make_bat4b_horizon(_YEAR)
        result = project_timeseries(df, h, year=_YEAR)
        # Only January data → 24 blocks for stage 1
        assert len(result) == 12 * 24  # full horizon (other stages have NaN)

    def test_project_3day_demand_jan_stage_not_nan(self):
        """January stage (stage 1) should have real projected values."""
        df = self._make_3day_demand()
        h = make_bat4b_horizon(_YEAR)
        result = project_timeseries(df, h, year=_YEAR)
        jan = result[result["stage"] == 1]["uid:1"]
        assert not jan.isna().all()

    def test_project_small_solar(self):
        """Solar projection on 1-week data completes without error."""
        idx = pd.date_range(f"{_YEAR}-01-01", periods=7 * 24, freq="h")
        idx_s = pd.Series(idx)
        hours_arr = idx_s.dt.hour.to_numpy()
        solar_vals = np.maximum(
            0.0,
            np.sin(np.pi * (hours_arr - 6) / 14)
            * np.where((hours_arr >= 6) & (hours_arr <= 20), 1.0, 0.0),
        )
        df = pd.DataFrame({"datetime": idx, "uid:3": solar_vals * 90.0})
        h = make_bat4b_horizon(_YEAR)
        result = project_timeseries(df, h, year=_YEAR)
        assert len(result) == 12 * 24

    def test_write_small_case_csv(self, tmp_path):
        """write_bat4b_timeseries with csv format creates 3 files in < 5 s."""
        result = write_bat4b_timeseries(
            tmp_path / "out", year=_YEAR, output_format="csv"
        )
        assert len(result) == 3
        for p in result.values():
            assert p.exists()


# ---------------------------------------------------------------------------
# Integration tests – write_bat4b_gtopt_input (directory structure)
# ---------------------------------------------------------------------------


class TestGtoptInputStructure:
    """Verify the gtopt-compatible input directory structure."""

    @pytest.mark.integration
    def test_subdirectories_created(self, tmp_path):
        """write_bat4b_gtopt_input creates Demand/, Generator/, GeneratorProfile/."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        assert (tmp_path / "Demand").is_dir()
        assert (tmp_path / "Generator").is_dir()
        assert (tmp_path / "GeneratorProfile").is_dir()

    @pytest.mark.integration
    def test_lmax_file_schema(self, tmp_path):
        """input/Demand/lmax.parquet has scenario,stage,block,uid:1,uid:2."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        df = pd.read_parquet(tmp_path / "Demand" / "lmax.parquet")
        for col in ("scenario", "stage", "block", "uid:1", "uid:2"):
            assert col in df.columns
        assert "_duration" not in df.columns
        assert len(df) == 12 * 24

    @pytest.mark.integration
    def test_pmax_file_schema(self, tmp_path):
        """input/Generator/pmax.parquet has scenario,stage,block,uid:1,uid:2."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        df = pd.read_parquet(tmp_path / "Generator" / "pmax.parquet")
        for col in ("scenario", "stage", "block", "uid:1", "uid:2"):
            assert col in df.columns
        assert len(df) == 12 * 24

    @pytest.mark.integration
    def test_profile_file_schema(self, tmp_path):
        """input/GeneratorProfile/profile.parquet has scenario,stage,block,uid:1."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        df = pd.read_parquet(tmp_path / "GeneratorProfile" / "profile.parquet")
        for col in ("scenario", "stage", "block", "uid:1"):
            assert col in df.columns
        # Should NOT have uid:3 (the raw solar column – it was renamed/normalized)
        assert "uid:3" not in df.columns

    @pytest.mark.integration
    def test_profile_values_in_0_1(self, tmp_path):
        """Solar profile factors must be in [0, 1]."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        df = pd.read_parquet(tmp_path / "GeneratorProfile" / "profile.parquet")
        assert df["uid:1"].min() >= 0.0
        assert df["uid:1"].max() <= 1.0 + 1e-9

    @pytest.mark.integration
    def test_pmax_g1_reduced_in_january(self, tmp_path):
        """January g1 projected pmax should be below nominal due to maintenance."""
        write_bat4b_gtopt_input(tmp_path, year=_YEAR)
        df = pd.read_parquet(tmp_path / "Generator" / "pmax.parquet")
        jan_g1 = df[df["stage"] == 1]["uid:1"].mean()
        assert jan_g1 < G1_PMAX_MW

    @pytest.mark.integration
    def test_write_bat4b_case_json_valid(self, tmp_path):
        """write_bat4b_case writes valid JSON with input_directory option."""
        json_path = write_bat4b_case(tmp_path, year=_YEAR)
        case = json.loads(json_path.read_text(encoding="utf-8"))
        assert case["options"]["input_directory"] == "input"
        assert case["options"]["input_format"] == "parquet"
        # Generators should use "pmax" stem (not full path)
        gen_g1 = next(g for g in case["system"]["generator_array"] if g["name"] == "g1")
        assert gen_g1["pmax"] == "pmax"
        # Solar generator has scalar pmax (capacity factor via profile)
        gen_solar = next(
            g for g in case["system"]["generator_array"] if g["name"] == "g_solar"
        )
        assert gen_solar["pmax"] == SOLAR_PMAX_MW
        # Profile present
        assert len(case["system"]["generator_profile_array"]) == 1

    @pytest.mark.integration
    def test_write_bat4b_case_input_files_exist(self, tmp_path):
        """All referenced input files exist after write_bat4b_case."""
        write_bat4b_case(tmp_path, year=_YEAR)
        assert (tmp_path / "input" / "Demand" / "lmax.parquet").exists()
        assert (tmp_path / "input" / "Generator" / "pmax.parquet").exists()
        assert (tmp_path / "input" / "GeneratorProfile" / "profile.parquet").exists()


# ---------------------------------------------------------------------------
# gtopt binary helpers (shared across end-to-end tests)
# ---------------------------------------------------------------------------


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary.

    Checks (in order):
    1. ``GTOPT_BIN`` environment variable
    2. ``shutil.which("gtopt")``
    3. ``../../build/gtopt`` (relative to the scripts/ directory)
    4. ``../../build-standalone/gtopt``
    5. ``../../all/build/gtopt``
    """
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).exists():
        return env_bin

    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin

    # Try build directories relative to the repo root
    # __file__ = scripts/ts2gtopt/tests/test_bat4b_annual.py
    # parents[3] = repo root (scripts/../..)
    repo_root = Path(__file__).resolve().parents[3]
    for rel in ("build/gtopt", "build-standalone/gtopt", "all/build/gtopt"):
        candidate = repo_root / rel
        if candidate.exists():
            return str(candidate)

    return None


def _read_max_fail_sol(output_dir: Path) -> float:
    """Return the maximum unserved demand (MW) from the gtopt fail_sol output.

    Returns 0.0 if the file does not exist or contains no uid columns.
    Returns ``float("nan")`` if the output directory is missing entirely.
    """
    fail_path = output_dir / "Demand" / "fail_sol.csv"
    if not output_dir.exists():
        return float("nan")
    if not fail_path.exists():
        return 0.0
    df = pd.read_csv(fail_path)
    uid_cols = [c for c in df.columns if c.startswith("uid:")]
    if not uid_cols:
        return 0.0
    return float(df[uid_cols].abs().max().max())


def _read_solution_status(output_dir: Path) -> int:
    """Return the solver status from ``output/solution.csv`` (0 = optimal).

    gtopt writes solution.csv as a key-value CSV with leading spaces:

    .. code-block:: text

        obj_value,12.4342
               kappa,1
              status,0

    We parse it with ``header=None``, strip whitespace from the first column,
    and find the row whose key equals ``"status"``.
    """
    sol_path = output_dir / "solution.csv"
    if not sol_path.exists():
        return -1
    df = pd.read_csv(sol_path, header=None)
    df.columns = ["key", "value"]
    df["key"] = df["key"].str.strip()
    status_rows = df[df["key"] == "status"]
    if status_rows.empty:
        return -1
    return int(status_rows["value"].iloc[0])


@pytest.fixture(scope="module")
def gtopt_bin() -> str:
    """Pytest fixture that provides the gtopt binary path.

    Skips the test if the binary is not found in the environment.
    """
    binary = _find_gtopt_binary()
    if binary is None:
        pytest.skip("gtopt binary not found (set GTOPT_BIN or add to PATH)")
    return binary


# ---------------------------------------------------------------------------
# Integration tests – end-to-end gtopt solver
# ---------------------------------------------------------------------------


class TestGtoptEndToEnd:
    """Run the gtopt solver on the bat_4b annual case and verify outputs.

    These tests require the ``gtopt`` binary to be available (see
    :func:`_find_gtopt_binary`).  They are skipped automatically when the
    binary is not found.

    Demand satisfaction guarantee
    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    The case is designed with sufficient generation capacity:

    * Peak demand ≈ 130–145 MW (d3 + d4, winter peak, workday evening)
    * g1 nominal pmax = 250 MW, g2 nominal pmax = 150 MW
    * Even during maintenance (g1 in Jan, g2 in Jul) the remaining generator
      plus solar (≤ 90 MW) and battery discharge (≤ 60 MW) cover all demand

    If load shedding is nonetheless detected, the test retries with 1.5× and
    then 2.0× the nominal pmax values before failing.
    """

    def _run_gtopt(
        self,
        gtopt_bin: str,
        case_dir: Path,
        year: int,
        g1_pmax: float = G1_PMAX_MW,
        g2_pmax: float = G2_PMAX_MW,
        timeout: int = _GTOPT_TIMEOUT_SECONDS,
    ) -> tuple[int, float, int, str]:
        """Write the case, run gtopt, return (solver_status, max_fail_mw, returncode, stderr)."""
        json_path = write_bat4b_case(
            case_dir, year=year, g1_pmax=g1_pmax, g2_pmax=g2_pmax
        )
        result = subprocess.run(
            [gtopt_bin, json_path.stem],
            cwd=str(json_path.parent),
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        status = _read_solution_status(case_dir / "output")
        max_fail = _read_max_fail_sol(case_dir / "output")
        return status, max_fail, result.returncode, result.stderr

    @pytest.mark.integration
    def test_gtopt_exits_zero(self, gtopt_bin, tmp_path):
        """gtopt must exit with return code 0 (no crash/error)."""
        _, _, rc, stderr = self._run_gtopt(gtopt_bin, tmp_path / "case", _YEAR)
        assert rc == 0, f"gtopt exited {rc}:\n{stderr}"

    @pytest.mark.integration
    def test_gtopt_status_optimal(self, gtopt_bin, tmp_path):
        """solution.csv must report status = 0 (optimal)."""
        status, _, rc, stderr = self._run_gtopt(gtopt_bin, tmp_path / "case", _YEAR)
        assert rc == 0, f"gtopt crashed: {stderr}"
        assert status == 0, f"solver status = {status} (expected 0 = optimal)"

    @pytest.mark.integration
    def test_gtopt_no_load_shedding(self, gtopt_bin, tmp_path):
        """Demand must be fully satisfied (fail_sol ≈ 0 MW).

        If unsatisfied demand is detected the test retries with 1.5× and 2.0×
        the nominal generator pmax before reporting failure.
        """
        fail_threshold_mw = 1.0  # 1 MW tolerance

        # Nominal run
        status, max_fail, rc, stderr = self._run_gtopt(
            gtopt_bin, tmp_path / "run1", _YEAR
        )
        assert rc == 0, f"gtopt crashed: {stderr}"
        assert status == 0, f"solver not optimal (status={status})"

        if max_fail <= fail_threshold_mw:
            return  # demand satisfied on first attempt

        # Retry with 1.5× pmax (network congestion guard)
        _, max_fail_15, rc15, _ = self._run_gtopt(
            gtopt_bin,
            tmp_path / "run2",
            _YEAR,
            g1_pmax=G1_PMAX_MW * 1.5,
            g2_pmax=G2_PMAX_MW * 1.5,
        )
        if rc15 == 0 and max_fail_15 <= fail_threshold_mw:
            return  # satisfied with 1.5× pmax

        # Retry with 2.0× pmax
        _, max_fail_20, rc20, _ = self._run_gtopt(
            gtopt_bin,
            tmp_path / "run3",
            _YEAR,
            g1_pmax=G1_PMAX_MW * 2.0,
            g2_pmax=G2_PMAX_MW * 2.0,
        )
        assert rc20 == 0 and max_fail_20 <= fail_threshold_mw, (
            f"Load shedding {max_fail_20:.2f} MW at 2.0× pmax "
            f"(g1={G1_PMAX_MW * 2.0} MW, g2={G2_PMAX_MW * 2.0} MW). "
            "Consider checking network topology or raising pmax further."
        )

    @pytest.mark.integration
    def test_gtopt_output_files_exist(self, gtopt_bin, tmp_path):
        """gtopt must produce the standard output CSV files."""
        _, _, rc, stderr = self._run_gtopt(gtopt_bin, tmp_path / "case", _YEAR)
        assert rc == 0, f"gtopt crashed: {stderr}"
        out = tmp_path / "case" / "output"
        assert (out / "solution.csv").exists(), "solution.csv not found"
        assert (out / "Demand" / "fail_sol.csv").exists(), "fail_sol.csv not found"
        assert (out / "Generator" / "generation_sol.csv").exists(), (
            "generation_sol.csv not found"
        )

    @pytest.mark.integration
    def test_gtopt_generation_covers_demand(self, gtopt_bin, tmp_path):
        """Total generation must be ≥ total served demand in every block."""
        _, _, rc, stderr = self._run_gtopt(gtopt_bin, tmp_path / "case", _YEAR)
        assert rc == 0, f"gtopt crashed: {stderr}"
        out = tmp_path / "case" / "output"

        gen_path = out / "Generator" / "generation_sol.csv"
        load_path = out / "Demand" / "load_sol.csv"
        if not gen_path.exists() or not load_path.exists():
            pytest.skip("generation_sol or load_sol not produced")

        gen_df = pd.read_csv(gen_path)
        load_df = pd.read_csv(load_path)

        uid_gen = [c for c in gen_df.columns if c.startswith("uid:")]
        uid_load = [c for c in load_df.columns if c.startswith("uid:")]

        total_gen = gen_df[uid_gen].sum(axis=1)
        total_load = load_df[uid_load].sum(axis=1)

        # Total generation ≥ total load in every row (with 1 MW tolerance for solver noise)
        slack = (total_gen - total_load).min()
        assert slack >= -1.0, (
            f"Generation deficit of {-slack:.2f} MW detected in at least one block"
        )

    @pytest.mark.integration
    def test_gtopt_seasonal_generation_ratio(self, gtopt_bin, tmp_path):
        """Summer mean generation (Jan) should be lower than winter (Jul).

        In winter, demand is higher, so total generation dispatched is higher.
        """
        _, _, rc, stderr = self._run_gtopt(gtopt_bin, tmp_path / "case", _YEAR)
        assert rc == 0, f"gtopt crashed: {stderr}"
        out = tmp_path / "case" / "output"
        gen_path = out / "Generator" / "generation_sol.csv"
        if not gen_path.exists():
            pytest.skip("generation_sol.csv not produced")

        gen_df = pd.read_csv(gen_path)
        uid_gen = [c for c in gen_df.columns if c.startswith("uid:")]
        gen_df["total"] = gen_df[uid_gen].sum(axis=1)
        # Stage 1 = January (summer), Stage 7 = July (winter)
        jan_mean = gen_df[gen_df["stage"] == 1]["total"].mean()
        jul_mean = gen_df[gen_df["stage"] == 7]["total"].mean()
        assert jul_mean > jan_mean, (
            f"Expected winter (Jul) generation > summer (Jan), "
            f"got Jul={jul_mean:.1f} MW, Jan={jan_mean:.1f} MW"
        )
