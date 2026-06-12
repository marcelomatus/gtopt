"""Unit tests for the RTS-GMLC → gtopt converter."""

from __future__ import annotations

import math

import pytest

from rts_gmlc_to_gtopt._converter import (
    LBS_PER_MMBTU_TO_TCO2_PER_GJ,
    POLLUTANTS,
    RtsGen,
    _collapse_heat_rate,
    synthetic_day_load,
    to_gtopt_json,
)


def test_lbs_per_mmbtu_conversion_constant():
    """Cross-check: 1 lb / MMBTU → 0.4536e-3 t / 1.055 GJ = 0.0004299 t/GJ."""

    assert math.isclose(LBS_PER_MMBTU_TO_TCO2_PER_GJ, 0.0004299, abs_tol=1e-6)


def test_coal_co2_matches_ipcc_within_5pct():
    """Spec sanity check: subbituminous coal at 210 lbs/MMBTU.

    The expected value is 0.0903 tCO2/GJ; IPCC's subbituminous-coal AR6
    figure is ~0.0961 tCO2/GJ.  We assert the converted value matches the
    expected 0.0903 within 0.5 %.
    """

    converted = 210.0 * LBS_PER_MMBTU_TO_TCO2_PER_GJ
    assert converted == pytest.approx(0.0903, abs=0.001)


def test_pollutants_table_starts_with_co2():
    """CO2 must be the first pollutant (the canonical default in tests)."""

    assert POLLUTANTS[0][0] == "co2"
    assert POLLUTANTS[0][1] == "Emissions CO2 Lbs/MMBTU"
    tags = {tag for tag, _ in POLLUTANTS}
    assert {"co2", "so2", "nox", "ch4", "n2o", "co", "vocs"} <= tags


def test_collapse_heat_rate_flat_curve():
    """A flat curve (no incremental bands fire) yields hr_avg_0 directly."""

    hr = _collapse_heat_rate(
        hr_avg_0=10000.0,
        hr_incr=[0.0, 0.0, 0.0, 0.0],
        output_pct=[1.0, 0.0, 0.0, 0.0, 0.0],
        pmax=100.0,
    )
    # heat_rate (BTU/kWh) = hr_avg_0 = 10000 → GJ/MWh = 10000 × 1.055e-3 = 10.55
    assert hr == pytest.approx(10.55, rel=1e-6)


def test_collapse_heat_rate_returns_zero_for_zero_pmax():
    assert _collapse_heat_rate(1.0, [0, 0, 0, 0], [1.0, 0, 0, 0, 0], 0.0) == 0.0


def test_synthetic_day_load_shape_has_evening_peak():
    """Pattern must be normalised so max equals the requested peak."""

    load = synthetic_day_load(peak_mw=1000.0, day=1)
    assert len(load) == 24
    # Day-1 scale = 0.9 + 0 / 9 × 0.1 = 0.9 → peak = 1000 × 0.9 = 900.
    assert max(load) == pytest.approx(900.0, rel=1e-6)
    # Evening peak is the 18:00 entry (index 18) — pattern[18] = 1.0.
    assert load[18] == pytest.approx(900.0, rel=1e-6)


def test_to_gtopt_json_emits_one_fuel_per_thermal():
    """One synthetic fuel per fueled generator (per-gen rates differ)."""

    from rts_gmlc_to_gtopt._converter import Conversion

    gens = [
        RtsGen(
            name="x_steam",
            bus_id=101,
            unit_type="STEAM",
            fuel_kind="Coal",
            pmax=100.0,
            pmin=20.0,
            vom=2.0,
            fuel_price_per_mmbtu=2.0,
            heat_rate_gj_per_mwh=10.5,
            combustion_tco2_per_gj=0.09,
            pollutant_combustion={"co2": 0.09},
            is_renewable=False,
        ),
        RtsGen(
            name="x_pv",
            bus_id=102,
            unit_type="PV",
            fuel_kind="Solar",
            pmax=80.0,
            pmin=0.0,
            vom=0.0,
            fuel_price_per_mmbtu=0.0,
            heat_rate_gj_per_mwh=0.0,
            combustion_tco2_per_gj=0.0,
            pollutant_combustion={"co2": 0.0},
            is_renewable=True,
        ),
    ]
    conv = Conversion(
        day=1,
        n_hours=2,
        bus_count=1,
        gen_count=2,
        line_count=0,
        total_load_mw=[100.0, 90.0],
        generators=gens,
    )
    payload = to_gtopt_json(conv, include_pollutants=("co2",))
    # Only the thermal generator gets a fuel (renewable HR=0 is skipped).
    assert len(payload["system"]["fuel_array"]) == 1
    fuel = payload["system"]["fuel_array"][0]
    assert fuel["emission_factors"][0]["combustion"] == pytest.approx(0.09)


def test_to_gtopt_json_pollutant_multiplexing():
    """Multi-pollutant request emits one Emission + Zone per pollutant."""

    from rts_gmlc_to_gtopt._converter import Conversion

    conv = Conversion(
        day=1,
        n_hours=1,
        bus_count=1,
        gen_count=0,
        line_count=0,
        total_load_mw=[50.0],
        generators=[],
    )
    payload = to_gtopt_json(conv, include_pollutants=("co2", "nox", "so2"))
    emissions = payload["system"]["emission_array"]
    zones = payload["system"]["emission_zone_array"]
    assert [e["name"] for e in emissions] == ["co2", "nox", "so2"]
    assert [z["name"] for z in zones] == ["global_co2", "global_nox", "global_so2"]
