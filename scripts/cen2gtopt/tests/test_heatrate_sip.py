# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``cen2gtopt._heatrate_sip`` — SIP curva_consumo parser/fitter."""

from __future__ import annotations

import math

import pandas as pd
import pytest

from cen2gtopt._heatrate_sip import (
    HeatRatePoint,
    build_heatrate_fits,
    build_heatrate_table,
    fit_linear_fuel,
    parse_curva_consumo,
)


# -----------------------------------------------------------------------------
# parse_curva_consumo
# -----------------------------------------------------------------------------


class TestParseCurvaConsumo:
    """Inline numeric heat-rate parser."""

    def test_tarapaca_three_point_curve(self) -> None:
        text = "9583,8 Kj/kWh a 150MW; 9631,53 Kj/kWh a 112,5MW; 9776,25 Kj/kWh a 90MW"
        pts = parse_curva_consumo(text)
        assert len(pts) == 3
        # sorted by power_mw ascending
        assert [p.power_mw for p in pts] == [90.0, 112.5, 150.0]
        assert pts[0].hr_kj_per_kwh == pytest.approx(9776.25)
        assert pts[2].hr_kj_per_kwh == pytest.approx(9583.8)

    def test_fuel_gj_per_h_property(self) -> None:
        pt = HeatRatePoint(power_mw=150.0, hr_kj_per_kwh=9583.8)
        # 150 MW * 9583.8 kJ/kWh = 1,437,570 kJ/h = 1437.57 GJ/h
        assert pt.fuel_gj_per_h == pytest.approx(1437.57)

    def test_kcal_unit_conversion(self) -> None:
        # 2300 kcal/kWh ≈ 9623 kJ/kWh
        text = "2300 kcal/kWh a 100MW"
        pts = parse_curva_consumo(text)
        assert len(pts) == 1
        assert pts[0].hr_kj_per_kwh == pytest.approx(2300 * 4.184)

    def test_at_separator_variants(self) -> None:
        for sep in ("a", "@", "at", "A"):
            pts = parse_curva_consumo(f"9000 kJ/kWh {sep} 50MW")
            assert len(pts) == 1, f"separator {sep!r} failed"
            assert pts[0].power_mw == pytest.approx(50.0)

    def test_dedup_same_power(self) -> None:
        # Same P=100 listed twice — second occurrence dropped
        text = "9000 kJ/kWh a 100MW; 9100 kJ/kWh a 100MW"
        pts = parse_curva_consumo(text)
        assert len(pts) == 1
        assert pts[0].hr_kj_per_kwh == pytest.approx(9000.0)

    @pytest.mark.parametrize(
        "placeholder",
        [
            "",
            "N/A",
            "No Aplica",
            "NO APLICA",
            "N/D",
            "Pendiente hasta PES",
            "DE00577-23",
            "DE 02355-21",
            "Anexo (DE00041-19.pdf)",
            "Informe-Consumo-Especifico-Central-Taltal.pdf",
            "Santa Lidia - Curva CEN",
        ],
    )
    def test_placeholder_returns_empty(self, placeholder: str) -> None:
        assert not parse_curva_consumo(placeholder)

    def test_none_input(self) -> None:
        assert not parse_curva_consumo(None)

    def test_zero_power_skipped(self) -> None:
        # Pathological row: 0 MW must not appear in output (can't divide later)
        assert not parse_curva_consumo("9000 kJ/kWh a 0MW")

    def test_partial_match_extracts_only_complete_pairs(self) -> None:
        # Surrounding free text shouldn't break the parse
        text = "Operating curve: 9583,8 Kj/kWh a 150MW (cf. anexo DE05551-22)"
        pts = parse_curva_consumo(text)
        assert len(pts) == 1
        assert pts[0].power_mw == 150.0


# -----------------------------------------------------------------------------
# fit_linear_fuel
# -----------------------------------------------------------------------------


class TestFitLinearFuel:
    """Linear ``fuel(P) = a + b·P`` least-squares fit."""

    def test_tarapaca_three_point_fit(self) -> None:
        text = "9583,8 Kj/kWh a 150MW; 9631,53 Kj/kWh a 112,5MW; 9776,25 Kj/kWh a 90MW"
        pts = parse_curva_consumo(text)
        fit = fit_linear_fuel(pts)
        assert fit is not None
        # Reference values computed from the 3-point Tarapaca curve.
        # OLS fit: fuel(P) ≈ 39.7 + 9.31·P  GJ/h, R² > 0.99
        assert fit.a_gj_per_h == pytest.approx(39.7, abs=1.0)
        assert fit.b_gj_per_mwh == pytest.approx(9.31, abs=0.05)
        assert fit.rsq > 0.99
        assert fit.n_points == 3

    def test_fit_predicts_input_back(self) -> None:
        text = "9583,8 Kj/kWh a 150MW; 9776,25 Kj/kWh a 90MW"
        pts = parse_curva_consumo(text)
        fit = fit_linear_fuel(pts)
        assert fit is not None
        # 2-point fit must be exact
        assert fit.hr_kj_per_kwh(150.0) == pytest.approx(9583.8, rel=1e-6)
        assert fit.hr_kj_per_kwh(90.0) == pytest.approx(9776.25, rel=1e-6)
        assert fit.rsq == pytest.approx(1.0)

    def test_fit_extrapolates_curvature(self) -> None:
        text = "9583,8 Kj/kWh a 150MW; 9631,53 Kj/kWh a 112,5MW; 9776,25 Kj/kWh a 90MW"
        fit = fit_linear_fuel(parse_curva_consumo(text))
        assert fit is not None
        hr_full = fit.hr_kj_per_kwh(150.0)
        hr_pmin = fit.hr_kj_per_kwh(90.0)
        # Heat rate must rise as power drops (no-load cost gets shared
        # over fewer MWh).
        assert hr_pmin > hr_full
        # The Tarapaca data shows ~+2 % from 150 MW to 90 MW — sanity.
        assert 1.005 < hr_pmin / hr_full < 1.05

    def test_invalid_power_raises(self) -> None:
        text = "9583,8 Kj/kWh a 150MW; 9776,25 Kj/kWh a 90MW"
        fit = fit_linear_fuel(parse_curva_consumo(text))
        assert fit is not None
        with pytest.raises(ValueError, match="must be positive"):
            fit.hr_kj_per_kwh(0.0)
        with pytest.raises(ValueError, match="must be positive"):
            fit.hr_kj_per_kwh(-50.0)

    def test_single_point_returns_none(self) -> None:
        pts = parse_curva_consumo("9583,8 Kj/kWh a 150MW")
        assert fit_linear_fuel(pts) is None

    def test_empty_returns_none(self) -> None:
        assert fit_linear_fuel([]) is None

    def test_degenerate_collinear_p_returns_none(self) -> None:
        # Two points at the same MW (shouldn't pass the parser but
        # bypass via direct construction)
        pts = [
            HeatRatePoint(power_mw=100.0, hr_kj_per_kwh=9000.0),
            HeatRatePoint(power_mw=100.0, hr_kj_per_kwh=9100.0),
        ]
        assert fit_linear_fuel(pts) is None

    def test_negative_intercept_rejected(self) -> None:
        # HR rising with P (non-physical) gives a negative intercept —
        # the fit is rejected to keep callers safe from bad SIP rows.
        pts = [
            HeatRatePoint(power_mw=50.0, hr_kj_per_kwh=8000.0),
            HeatRatePoint(power_mw=150.0, hr_kj_per_kwh=10000.0),
        ]
        # fuel(50) = 400 GJ/h, fuel(150) = 1500 GJ/h
        # slope b = (1500-400)/(150-50) = 11, intercept a = 400 - 11*50 = -150
        assert fit_linear_fuel(pts) is None


# -----------------------------------------------------------------------------
# build_heatrate_table / build_heatrate_fits
# -----------------------------------------------------------------------------


@pytest.fixture(name="units_df")
def fixture_units_df() -> pd.DataFrame:
    """Three-row toy units catalogue mimicking the SIP parquet schema."""
    return pd.DataFrame(
        [
            {
                "id_unidad": 100,
                "unidad_nombre": "TER FAKE TARAPACA",
                "curva_consumo": (
                    "9583,8 Kj/kWh a 150MW; 9631,53 Kj/kWh a 112,5MW; "
                    "9776,25 Kj/kWh a 90MW"
                ),
            },
            {
                "id_unidad": 101,
                "unidad_nombre": "HE FAKE HYDRO",
                "curva_consumo": "No Aplica",
            },
            {
                "id_unidad": 102,
                "unidad_nombre": "TER FAKE DOC_REF",
                "curva_consumo": "DE00577-23",
            },
        ],
    )


class TestBuildHeatrateTable:
    """Long-form ``(id_unidad, P, HR)`` table."""

    def test_only_numeric_unit_appears(self, units_df: pd.DataFrame) -> None:
        out = build_heatrate_table(units_df)
        assert set(out["id_unidad"]) == {100}

    def test_three_rows_for_three_point_curve(self, units_df: pd.DataFrame) -> None:
        out = build_heatrate_table(units_df)
        assert len(out) == 3
        assert set(out["power_mw"]) == {90.0, 112.5, 150.0}

    def test_columns(self, units_df: pd.DataFrame) -> None:
        out = build_heatrate_table(units_df)
        assert list(out.columns) == [
            "id_unidad",
            "unidad_nombre",
            "power_mw",
            "hr_kj_per_kwh",
            "fuel_gj_per_h",
        ]

    def test_empty_input_returns_empty_frame(self) -> None:
        empty = pd.DataFrame(columns=["id_unidad", "unidad_nombre", "curva_consumo"])
        out = build_heatrate_table(empty)
        assert len(out) == 0
        assert list(out.columns) == [
            "id_unidad",
            "unidad_nombre",
            "power_mw",
            "hr_kj_per_kwh",
            "fuel_gj_per_h",
        ]

    def test_missing_required_column_raises(self) -> None:
        bad = pd.DataFrame({"id_unidad": [1], "unidad_nombre": ["X"]})
        with pytest.raises(ValueError, match="curva_consumo"):
            build_heatrate_table(bad)


class TestBuildHeatrateFits:
    """Per-unit linear-fit summary."""

    def test_single_fit_row(self, units_df: pd.DataFrame) -> None:
        out = build_heatrate_fits(units_df)
        assert len(out) == 1
        row = out.iloc[0]
        assert row["id_unidad"] == 100
        assert row["n_points"] == 3
        assert math.isclose(row["p_min_mw"], 90.0)
        assert math.isclose(row["p_max_mw"], 150.0)
        assert row["a_gj_per_h"] > 0.0
        assert row["b_gj_per_mwh"] > 0.0
        assert row["rsq"] > 0.999

    def test_columns(self, units_df: pd.DataFrame) -> None:
        out = build_heatrate_fits(units_df)
        assert list(out.columns) == [
            "id_unidad",
            "unidad_nombre",
            "a_gj_per_h",
            "b_gj_per_mwh",
            "rsq",
            "n_points",
            "p_min_mw",
            "p_max_mw",
        ]

    def test_missing_required_column_raises(self) -> None:
        bad = pd.DataFrame({"id_unidad": [1]})
        with pytest.raises(ValueError, match="curva_consumo"):
            build_heatrate_fits(bad)
