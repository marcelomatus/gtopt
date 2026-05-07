# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the declared-MC composition helper.

Verifies the join logic between costo-combustible (USD per fuel-unit)
and topologia/v3/fuelcons (heat rate in fuel-units / MWh) → declared
MC in USD/MWh.
"""

from __future__ import annotations

import pandas as pd
import pytest

from cen2gtopt._mc_composition import compose_declared_mc


def _costos_fixture() -> pd.DataFrame:
    """Synthesised costo-combustible long-form (5 centrales, 1 day)."""
    return pd.DataFrame(
        [
            {
                "date_utc": "2024-11-01",
                "central_name": "PMGD TER TIRUA",
                "configuracion": "TIRUA_DIESEL",
                "empresa": "X",
                "tipo_combustible": "Diésel",
                "costo_combustible": 1345.35,
            },
            {
                "date_utc": "2024-11-01",
                "central_name": "BOCAMINA II",
                "configuracion": "BOCAMINA_2",
                "empresa": "ENEL",
                "tipo_combustible": "Carbón",
                "costo_combustible": 150.19,
            },
            {
                "date_utc": "2024-11-01",
                "central_name": "LAUTARO 2 BL1",
                "configuracion": "LAUTARO_2_BL1",
                "empresa": "COMASA",
                "tipo_combustible": "Biomasa",
                "costo_combustible": 20.04,
            },
            # An orphan row — no fuelcons match.
            {
                "date_utc": "2024-11-01",
                "central_name": "GHOST_PLANT",
                "configuracion": "GHOST",
                "empresa": "?",
                "tipo_combustible": "Diésel",
                "costo_combustible": 999.99,
            },
        ]
    )


def _fuelcons_fixture() -> pd.DataFrame:
    """Synthesised topologia/v3/fuelcons long-form (3 plants)."""
    return pd.DataFrame(
        [
            {
                "unit_id": 6765,
                "plant_name": "PMGD TER TIRUA",
                "fuel": "Diésel",
                "heat_rate": 0.2672,
                "fuel_unit": "ton",
                "own_consumption": 0.0,
                "start_day": "2016-01-01",
                "end_day": None,
            },
            {
                "unit_id": 47,
                "plant_name": "Bocamina II",
                "fuel": "Carbón",
                "heat_rate": 0.420,
                "fuel_unit": "ton",
                "own_consumption": 0.05,
                "start_day": "2010-01-01",
                "end_day": None,
            },
            {
                "unit_id": 81,
                "plant_name": "LAUTARO 2 BL1",
                "fuel": "Biomasa",
                "heat_rate": 1.450,
                "fuel_unit": "ton",
                "own_consumption": 0.0,
                "start_day": "2015-01-01",
                "end_day": None,
            },
        ]
    )


def test_compose_declared_mc_basic_join():
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    assert result.n_match == 3  # 3 plants joined; 1 orphan unmatched
    assert len(result.declared_mc) == 3

    # Check declared_MC = heat_rate × fuel_cost.
    by_plant = result.declared_mc.set_index("plant_name")["declared_MC"]
    assert by_plant["PMGD TER TIRUA"] == pytest.approx(0.2672 * 1345.35, rel=1e-6)
    assert by_plant["Bocamina II"] == pytest.approx(0.420 * 150.19, rel=1e-6)
    assert by_plant["LAUTARO 2 BL1"] == pytest.approx(1.450 * 20.04, rel=1e-6)


def test_compose_declared_mc_records_unmatched():
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    # GHOST_PLANT has no fuelcons match.
    assert len(result.unmatched_costos) == 1
    assert result.unmatched_costos.iloc[0]["central_name"] == "GHOST_PLANT"


def test_compose_declared_mc_empty_inputs():
    empty = pd.DataFrame()
    r1 = compose_declared_mc(empty, _fuelcons_fixture())
    assert r1.n_match == 0
    assert len(r1.declared_mc) == 0

    r2 = compose_declared_mc(_costos_fixture(), empty)
    assert r2.n_match == 0


def test_normalisation_handles_accents_and_case():
    """Bocamina II vs BOCAMINA_2 should not match by central_name on
    case-fold alone (different tokens), but Bocamina II in fuelcons
    should match BOCAMINA II in costos. The fixture uses "BOCAMINA II"
    in costos and "Bocamina II" in fuelcons — the normaliser should
    bridge the case difference."""
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    # The Bocamina row should be in declared_mc.
    matched_names = set(result.declared_mc["plant_name"])
    assert "Bocamina II" in matched_names


def test_declared_mc_values_are_realistic_for_chilean_sen():
    """Sanity check: a Diesel peaker should land at >$200/MWh,
    a coal central at $40-100/MWh, biomass below $50/MWh."""
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    by_fuel = result.declared_mc.groupby("fuel")["declared_MC"].first()
    assert by_fuel["Diésel"] > 200  # 0.2672 × 1345 ≈ $359/MWh
    assert 40 < by_fuel["Carbón"] < 100  # 0.420 × 150 ≈ $63/MWh
    assert by_fuel["Biomasa"] < 50  # 1.450 × 20 ≈ $29/MWh


# ----------------------------------------------------------------------
# Emission-factor composition (master plan §4.12 — heat_rate × fuel EF)
# ----------------------------------------------------------------------


def test_emission_factor_per_mwh_for_diesel():
    """Diesel: 0.2672 ton/MWh × 3170 kg CO₂/ton = ~847 kg CO₂/MWh.

    Reference: IPCC 2006 GL Vol. 2 Tab. 1.4 default for distillate
    fuel oil ≈ 74 100 kg CO₂/TJ × 41.9 GJ/ton ≈ 3 100 kg CO₂/ton.
    Our table uses 3 170 (ENGIE Chile 2023 grade). 0.2672 × 3170 ≈ 847.
    """
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    diesel_row = result.declared_mc[result.declared_mc["fuel"] == "Diésel"].iloc[0]
    assert diesel_row["emission_factor_kg_per_unit"] == pytest.approx(3170.0)
    assert diesel_row["emission_factor_kg_per_mwh"] == pytest.approx(0.2672 * 3170.0)
    assert 800 < diesel_row["emission_factor_kg_per_mwh"] < 900


def test_emission_factor_per_mwh_for_coal():
    """Coal: 0.420 ton/MWh × 2530 kg CO₂/ton = ~1062 kg CO₂/MWh.

    This is in the canonical 900-1100 kg/MWh range cited in master
    plan §4.12.1 for sub-bituminous coal generators."""
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    coal_row = result.declared_mc[result.declared_mc["fuel"] == "Carbón"].iloc[0]
    assert coal_row["emission_factor_kg_per_unit"] == pytest.approx(2530.0)
    assert coal_row["emission_factor_kg_per_mwh"] == pytest.approx(0.420 * 2530.0)
    assert 900 < coal_row["emission_factor_kg_per_mwh"] < 1100


def test_biomass_emission_factor_is_zero_per_ipcc_biogenic_rule():
    """Biomass CO₂ is biogenic — IPCC counts it as zero in stationary
    combustion to avoid double-counting against the LULUCF sector."""
    result = compose_declared_mc(_costos_fixture(), _fuelcons_fixture())
    bio_row = result.declared_mc[result.declared_mc["fuel"] == "Biomasa"].iloc[0]
    assert bio_row["emission_factor_kg_per_mwh"] == 0.0


def test_emission_factor_nan_for_unknown_fuel(_costos_fixture=None):
    """An unknown fuel name should yield NaN, not silently 0."""
    import math

    costos = pd.DataFrame(
        [
            {
                "date_utc": "2024-11-01",
                "central_name": "MYSTERY",
                "configuracion": "MYSTERY",
                "empresa": "?",
                "tipo_combustible": "Plutonium",
                "costo_combustible": 99.0,
            },
        ]
    )
    fuelcons = pd.DataFrame(
        [
            {
                "unit_id": 999,
                "plant_name": "MYSTERY",
                "fuel": "Plutonium",
                "heat_rate": 0.001,
                "fuel_unit": "g",
                "own_consumption": 0.0,
                "start_day": "2024-01-01",
                "end_day": None,
            },
        ]
    )
    result = compose_declared_mc(costos, fuelcons)
    assert len(result.declared_mc) == 1
    row = result.declared_mc.iloc[0]
    assert math.isnan(row["emission_factor_kg_per_unit"])
    assert math.isnan(row["emission_factor_kg_per_mwh"])
    # declared_MC still computes from cost × heat_rate.
    assert row["declared_MC"] == pytest.approx(99.0 * 0.001)
