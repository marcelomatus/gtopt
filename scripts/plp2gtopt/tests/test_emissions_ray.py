# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plp2gtopt._emissions_ray — synthetic emissions ray
construction for the --only-emissions LP horizon (#520).
"""

from __future__ import annotations

import csv
import math
from pathlib import Path

import pytest

from plp2gtopt._emissions_ray import (
    build_emissions_ray,
    npv_factor,
    stamp_boundary_cuts_file_ref,
    write_emissions_ray_csv,
)


# ---------------------------------------------------------------------------
# npv_factor
# ---------------------------------------------------------------------------


class TestNpvFactor:
    def test_perpetuity_at_5pct(self) -> None:
        assert npv_factor(0.05) == pytest.approx(1.0 / 0.05)
        assert npv_factor(0.05) == pytest.approx(20.0)

    def test_perpetuity_at_10pct(self) -> None:
        assert npv_factor(0.10) == pytest.approx(10.0)

    def test_finite_annuity_at_5pct_20yrs(self) -> None:
        """20-year annuity at 5 % ≈ 12.46 yr."""
        v = npv_factor(0.05, horizon_years=20)
        expected = (1 - math.pow(1.05, -20)) / 0.05
        assert v == pytest.approx(expected, rel=1e-6)
        assert v == pytest.approx(12.4622, rel=1e-3)

    def test_zero_discount_finite_horizon_returns_horizon(self) -> None:
        """Zero discount + finite horizon → undiscounted sum = N years."""
        assert npv_factor(0.0, horizon_years=10) == 10.0

    def test_zero_discount_perpetuity_caps_at_one(self) -> None:
        """Zero discount + infinite horizon → can't diverge; defaults
        to single-period (1.0) per the documented contract."""
        assert npv_factor(0.0) == 1.0


# ---------------------------------------------------------------------------
# build_emissions_ray — pure-function semantics
# ---------------------------------------------------------------------------


class TestBuildEmissionsRay:
    def test_l_maule_slope_at_5pct_perpetuity(self) -> None:
        """L_Maule (EPF=9.34) at 5 % perpetuity:
        slope = 9.34 × 0.5 × 0.95 × 8760 × 20
              ≈ 776,635 tCO2eq per cumec"""
        rhs, slopes = build_emissions_ray({"L_Maule": 9.34})
        assert rhs == 0.0
        expected = 9.34 * 0.5 * 0.95 * 8760 * 20.0
        assert slopes["L_Maule"] == pytest.approx(expected, rel=1e-4)
        assert slopes["L_Maule"] == pytest.approx(777_259, rel=1e-3)

    def test_zero_epf_omitted_from_slopes(self) -> None:
        """Reservoirs with EPF=0 (terminal hydros) drop out."""
        _rhs, slopes = build_emissions_ray(
            {
                "L_Maule": 9.34,
                "TERMINAL_HYDRO": 0.0,
            }
        )
        assert "L_Maule" in slopes
        assert "TERMINAL_HYDRO" not in slopes

    def test_custom_gas_em_scales_linearly(self) -> None:
        """Doubling gas_em doubles every slope."""
        _, base = build_emissions_ray({"X": 5.0})
        _, high = build_emissions_ray({"X": 5.0}, gas_em=1.0)  # 2× default
        assert high["X"] == pytest.approx(2.0 * base["X"], rel=1e-9)

    def test_custom_discount_changes_slopes(self) -> None:
        """Higher discount rate → smaller slope (less future value)."""
        _, low_r = build_emissions_ray({"X": 5.0}, discount_rate=0.05)
        _, high_r = build_emissions_ray({"X": 5.0}, discount_rate=0.10)
        assert high_r["X"] < low_r["X"]
        # 10 % perpetuity = 1/0.10 = 10 yr (half of 20 yr at 5 %)
        assert high_r["X"] == pytest.approx(low_r["X"] / 2.0, rel=1e-4)

    def test_finite_horizon_smaller_than_perpetuity(self) -> None:
        """20-yr annuity < perpetuity at the same rate."""
        _, perp = build_emissions_ray({"X": 5.0}, discount_rate=0.05)
        _, ann = build_emissions_ray({"X": 5.0}, discount_rate=0.05, horizon_years=20)
        assert ann["X"] < perp["X"]

    def test_empty_input_produces_empty_slopes(self) -> None:
        rhs, slopes = build_emissions_ray({})
        assert rhs == 0.0
        assert not slopes


# ---------------------------------------------------------------------------
# write_emissions_ray_csv — I/O semantics
# ---------------------------------------------------------------------------


class TestWriteEmissionsRayCsv:
    def test_csv_format_matches_boundary_cuts_schema(self, tmp_path: Path) -> None:
        """One header row + one data row, schema
        ``iteration,scene,rhs,Reservoir1,...``."""
        p = tmp_path / "boundary_cuts.csv"
        write_emissions_ray_csv(
            p,
            {"L_Maule": 9.34, "COLBUN": 1.42},
            reservoir_order=["L_Maule", "COLBUN"],
        )
        with open(p, encoding="utf-8") as f:
            rows = list(csv.reader(f))
        assert len(rows) == 2
        assert rows[0] == ["iteration", "scene", "rhs", "L_Maule", "COLBUN"]
        # Arrow CSV writes shortest round-trip floats, so 0.0 -> "0".
        # Compare on numeric equivalence, not textual form.
        assert [float(x) for x in rows[1][:3]] == [1.0, 0.0, 0.0]
        # Slopes are non-zero floats
        assert float(rows[1][3]) > 0
        assert float(rows[1][4]) > 0
        # L_Maule has 6.6× the EPF of COLBUN → 6.6× the slope
        ratio = float(rows[1][3]) / float(rows[1][4])
        assert ratio == pytest.approx(9.34 / 1.42, rel=1e-5)

    def test_csv_zero_for_missing_reservoirs(self, tmp_path: Path) -> None:
        """Reservoirs in reservoir_order but not in EPF dict → slope = 0."""
        p = tmp_path / "boundary_cuts.csv"
        write_emissions_ray_csv(
            p,
            {"L_Maule": 9.34},  # only one EPF
            reservoir_order=["L_Maule", "TERMINAL"],
        )
        with open(p, encoding="utf-8") as f:
            rows = list(csv.reader(f))
        assert float(rows[1][3]) > 0
        assert float(rows[1][4]) == 0.0

    def test_csv_sorted_default_order(self, tmp_path: Path) -> None:
        """No reservoir_order → sorted alphabetically."""
        p = tmp_path / "boundary_cuts.csv"
        write_emissions_ray_csv(p, {"Z": 1.0, "A": 2.0, "M": 1.5})
        with open(p, encoding="utf-8") as f:
            rows = list(csv.reader(f))
        assert rows[0][3:] == ["A", "M", "Z"]

    def test_csv_creates_parent_directory(self, tmp_path: Path) -> None:
        """Writing into a non-existent subdirectory creates it."""
        p = tmp_path / "deep" / "nested" / "boundary_cuts.csv"
        assert not p.parent.exists()
        write_emissions_ray_csv(p, {"X": 5.0})
        assert p.exists()


# ---------------------------------------------------------------------------
# stamp_boundary_cuts_file_ref
# ---------------------------------------------------------------------------


class TestStampBoundaryCutsFileRef:
    def test_stamps_when_missing(self) -> None:
        pl: dict = {"options": {}}
        stamp_boundary_cuts_file_ref(pl)
        assert pl["options"]["boundary_cuts_file"] == "boundary_cuts.csv"

    def test_preserves_user_value(self) -> None:
        """User-set boundary_cuts_file path wins (idempotent)."""
        pl = {"options": {"boundary_cuts_file": "custom_cuts.csv"}}
        stamp_boundary_cuts_file_ref(pl)
        assert pl["options"]["boundary_cuts_file"] == "custom_cuts.csv"

    def test_creates_options_dict_if_missing(self) -> None:
        pl: dict = {}
        stamp_boundary_cuts_file_ref(pl)
        assert pl["options"]["boundary_cuts_file"] == "boundary_cuts.csv"

    def test_custom_relative_path(self) -> None:
        pl: dict = {}
        stamp_boundary_cuts_file_ref(pl, "emissions/cuts.csv")
        assert pl["options"]["boundary_cuts_file"] == "emissions/cuts.csv"
