"""Unit tests for the NREL-118 → gtopt converter."""

from __future__ import annotations

import math
from pathlib import Path

import pytest

from nrel118_to_gtopt._converter import (
    BTU_PER_KWH_TO_GJ_PER_MWH,
    IPCC_AR6_TCO2_PER_GJ,
    collapse_piecewise_heat_rate,
    to_gtopt_json,
    unit_type_to_fuel,
)


def test_btu_per_kwh_conversion_constant():
    """Sanity-check the BTU/kWh → GJ/MWh constant: 0.001055."""

    assert math.isclose(BTU_PER_KWH_TO_GJ_PER_MWH, 1.055e-3, rel_tol=1e-6)


def test_ipcc_factors_coal_natural_gas_diesel():
    """Cross-check against IPCC AR6 Table A.III.2 (rounded to 4 dp)."""

    assert IPCC_AR6_TCO2_PER_GJ["coal"] == pytest.approx(0.0946, abs=1e-6)
    assert IPCC_AR6_TCO2_PER_GJ["natural_gas"] == pytest.approx(0.0561, abs=1e-6)
    assert IPCC_AR6_TCO2_PER_GJ["diesel"] == pytest.approx(0.0741, abs=1e-6)
    # Renewables / nuclear must be exactly zero.
    for renewable in ("wind", "solar", "hydro", "nuclear", "biomass"):
        assert IPCC_AR6_TCO2_PER_GJ[renewable] == 0.0


def test_unit_type_to_fuel_mapping_covers_nrel118_categories():
    """Every category that appears in NREL-118 must map to a known fuel."""

    cases = {
        "Biomass 01": "biomass",
        "CC NG 02": "natural_gas",
        "CT NG 03": "natural_gas",
        "CT Oil 04": "diesel",
        "Geo 01": "geothermal",
        "Hydro 12": "hydro",
        "ICE NG 05": "natural_gas",
        "Solar 18": "solar",
        "ST Coal 06": "coal",
        "ST NG 07": "natural_gas",
        "ST Other 08": "other",
        "Wind 09": "wind",
    }
    for name, expected_kind in cases.items():
        assert unit_type_to_fuel(name) == expected_kind, name


def test_collapse_piecewise_heat_rate_single_band():
    """Single-band collapse: ST_Coal_01 in NREL-118.

    Generators.csv:  HR_Base=108.8 MMBTU/hr, Inc1=10171.43 BTU/kWh,
                     pmin=19.84 MW (= Load Point Band 1 start), pmax=20.
    """

    hr_gj_per_mwh = collapse_piecewise_heat_rate(
        hr_base_mmbtu_per_hr=108.8,
        hr_inc_bands_btu_per_kwh=[10171.43, 0, 0, 0, 0],
        load_points_mw=[19.84, 0, 0, 0, 0],
        pmin=19.84,
        pmax=20.0,
    )
    # No incremental band fires (LP1 == pmin), so HR = HR_Base / pmax × 1.055.
    expected = 108.8 / 20.0 * 1.055
    assert hr_gj_per_mwh == pytest.approx(expected, rel=1e-5)


def test_collapse_piecewise_heat_rate_with_band_extension():
    """Real CC_NG_01: pmin = LP1 = pmax, so HR_Base alone determines HR."""

    hr = collapse_piecewise_heat_rate(
        hr_base_mmbtu_per_hr=316.78,
        hr_inc_bands_btu_per_kwh=[6489.0, 0, 0, 0, 0],
        load_points_mw=[41.5, 0, 0, 0, 0],
        pmin=41.08,
        pmax=41.5,
    )
    # HR_Base contribution alone = 316.78 / 41.5 = 7.633 MMBTU/MWh →
    # 7.633 × 1.055 = 8.053 GJ/MWh.  Plus a small Inc1 contribution
    # over the (41.5 − 41.08) MW band.
    base = 316.78 / 41.5 * 1.055
    assert hr == pytest.approx(base, rel=0.01)


def test_collapse_piecewise_heat_rate_zero_pmax_returns_zero():
    """Defensive guard: pmax <= 0 → 0 (no division-by-zero)."""

    assert (
        collapse_piecewise_heat_rate(0, [0, 0, 0, 0, 0], [0, 0, 0, 0, 0], 0, 0) == 0.0
    )


def test_to_gtopt_json_baseline_has_no_renewable_gen(tmp_path: Path):
    """``renewables_share=0`` → no synthetic aggregate_renewables generator."""

    from nrel118_to_gtopt._converter import Conversion, NrelGen

    conv = Conversion(
        week=1,
        n_hours=2,
        bus_count=1,
        gen_count=1,
        line_count=0,
        total_load_mw=[100.0, 90.0],
        fuels=[],
        generators=[
            NrelGen(
                name="Solar 01",
                bus="bus1",
                pmax=200.0,
                pmin=0.0,
                vom=0.0,
                fuel="solar",
                heat_rate_gj_per_mwh=0.0,
                is_renewable=True,
            ),
        ],
    )
    payload = to_gtopt_json(conv, renewables_share=0.0)
    names = {g["name"] for g in payload["system"]["generator_array"]}
    assert "aggregate_renewables" not in names
    assert "Solar 01" in names


def test_to_gtopt_json_with_renewables_share_adds_aggregate(tmp_path: Path):
    """``renewables_share=0.33`` → adds the aggregate renewable + derates."""

    from nrel118_to_gtopt._converter import Conversion, NrelGen

    conv = Conversion(
        week=1,
        n_hours=2,
        bus_count=1,
        gen_count=1,
        line_count=0,
        total_load_mw=[100.0, 100.0],
        fuels=[],
        generators=[
            NrelGen(
                name="ST Coal 01",
                bus="bus1",
                pmax=100.0,
                pmin=0.0,
                vom=2.99,
                fuel="coal",
                heat_rate_gj_per_mwh=10.7,
                is_renewable=False,
            ),
        ],
    )
    payload = to_gtopt_json(conv, renewables_share=0.33)
    coal = next(
        g for g in payload["system"]["generator_array"] if g["name"] == "ST Coal 01"
    )
    # Capacity derated by (1 − 0.33).
    assert coal["capacity"] == pytest.approx(100.0 * 0.67, rel=1e-9)
    renew = next(
        g
        for g in payload["system"]["generator_array"]
        if g["name"] == "aggregate_renewables"
    )
    # Aggregate renewable capacity = peak × share.
    assert renew["capacity"] == pytest.approx(100.0 * 0.33, rel=1e-9)
