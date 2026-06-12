# SPDX-License-Identifier: BSD-3-Clause
"""Equivalence tests for the three emission_rate resolution paths.

``topology_from_planning`` resolves each generator's CO2eq factor in
this priority order (matches the C++ ``EmissionSourceLP`` / #522
substitution model — every consumer agrees on the same number):

1. ``emission_source_array`` aggregated × ``EmissionZone`` weight per
   pollutant (LP-emitted ``EmissionSource/rate_sol.parquet`` overrides
   the static per-source rate).
2. Per-gen JSON override ``Generator.emission_rate`` (lossy scalar).
3. Derived from ``heat_rate × heat_content × Σ_e (combustion_e × weight_e)``
   — the multi-pollutant factored form.  Default path for plexos2gtopt
   / plp2gtopt cost-mode JSONs.

These tests pin the equivalence of paths 1, 2, 3 on the same physical
setup so a change in any one path is caught by the same numeric
assertion.  The reference value is the path-3 closed form:

    rate = combustion_CO2 [tCO2/GJ]
         × heat_content   [GJ/fuel-unit]
         × heat_rate      [fuel-unit/MWh]
    with weight_CO2 = 1.0 (single-pollutant CO2 zone)
"""

from __future__ import annotations

import pytest

from gtopt_marginal_units._gtopt_reader import topology_from_planning


# Physical fingerprint shared across the three planning JSONs:
#   coal-class fuel: combustion=0.0961 tCO2/GJ, heat_content=25.8 GJ/ton
#   coal gen:        heat_rate=0.376 ton/MWh
#   expected CO2eq:  0.0961 × 25.8 × 0.376 ≈ 0.9319873…
COMBUSTION_CO2 = 0.0961
HEAT_CONTENT = 25.8
HEAT_RATE = 0.376
EXPECTED_RATE = COMBUSTION_CO2 * HEAT_CONTENT * HEAT_RATE


def _base_system_factored() -> dict:
    """Path-3 setup: factored multi-pollutant data only, no aggregate."""
    return {
        "bus_array": [{"uid": 1, "name": "b1"}],
        "fuel_array": [
            {
                "uid": 50,
                "name": "Coal",
                "type": "coal",
                "heat_content": HEAT_CONTENT,
                "emission_factors": [
                    {"emission": "co2", "combustion": COMBUSTION_CO2},
                ],
            },
        ],
        "emission_zone_array": [
            {
                "uid": 1,
                "name": "z1",
                "emissions": [{"emission": "co2", "weight": 1.0}],
            },
        ],
        "generator_array": [
            {
                "uid": 10,
                "name": "g_coal",
                "bus": "b1",
                "pmin": 0,
                "pmax": 100,
                "gcost": 30,
                "fuel": "Coal",
                "heat_rate": HEAT_RATE,
                "type": "thermal",
            },
        ],
    }


def _gen_emission_rate(planning: dict, uid: int = 10) -> float | None:
    topo = topology_from_planning(planning)
    for g in topo.generators:
        if g.uid == uid:
            return g.emission_rate
    return None


# ---------------------------------------------------------------------------
# Path 3 — derived from heat_rate × heat_content × Σ(combustion × weight)
# ---------------------------------------------------------------------------


def test_path3_derive_from_fuel_and_zone_single_pollutant():
    planning = {"system": _base_system_factored()}
    rate = _gen_emission_rate(planning)
    assert rate is not None
    assert rate == pytest.approx(EXPECTED_RATE, rel=1e-9)


def test_path3_multi_pollutant_co2eq_aggregation():
    """Add CH4 + N2O alongside CO2 with GWP weights; rate must sum
    per-pollutant contributions weighted by ``zone.emissions[].weight``.
    Reproduces the C++ EmissionSourceLP α factor: weight_e × rate_e.
    """
    sys_ = _base_system_factored()
    sys_["fuel_array"][0]["emission_factors"] = [
        {"emission": "co2", "combustion": COMBUSTION_CO2},
        {"emission": "ch4", "combustion": 1e-6},
        {"emission": "n2o", "combustion": 1.5e-6},
    ]
    sys_["emission_zone_array"][0]["emissions"] = [
        {"emission": "co2", "weight": 1.0},
        {"emission": "ch4", "weight": 28.0},  # AR5 GWP100
        {"emission": "n2o", "weight": 265.0},
    ]
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    expected = (
        ((COMBUSTION_CO2 * 1.0) + (1e-6 * 28.0) + (1.5e-6 * 265.0))
        * HEAT_CONTENT
        * HEAT_RATE
    )
    assert rate is not None
    assert rate == pytest.approx(expected, rel=1e-9)


def test_path3_no_zone_means_no_derive():
    """An emission_zone_array with no pollutants ⇒ no weighting ⇒
    no derived rate.  Generator.emission_rate stays None — explicit
    'zone says I don't care about this pollutant' signal preserved.
    """
    sys_ = _base_system_factored()
    sys_["emission_zone_array"] = []  # no zone, no weights
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    assert rate is None


# ---------------------------------------------------------------------------
# Path 2 — per-gen JSON override
# ---------------------------------------------------------------------------


def test_path2_per_gen_override_wins_over_path3():
    """Setting Generator.emission_rate directly should keep the JSON
    value verbatim, regardless of what fuel/heat_rate would derive."""
    sys_ = _base_system_factored()
    sys_["generator_array"][0]["emission_rate"] = 0.5  # forced value
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    assert rate == pytest.approx(0.5, rel=1e-12)


def test_path2_override_with_no_factored_data():
    """Even without fuel data, the per-gen override is honoured.
    Legacy converters that don't carry Fuel.emission_factors can still
    advertise a per-gen aggregate (e.g. nrel118_to_gtopt)."""
    sys_ = {
        "bus_array": [{"uid": 1, "name": "b1"}],
        "generator_array": [
            {
                "uid": 10,
                "name": "g10",
                "bus": "b1",
                "pmin": 0,
                "pmax": 100,
                "gcost": 30,
                "emission_rate": 0.7,
            },
        ],
    }
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    assert rate == pytest.approx(0.7, rel=1e-12)


# ---------------------------------------------------------------------------
# Path 1 — emission_source_array aggregate
# ---------------------------------------------------------------------------


def test_path1_emission_source_array_wins_over_path2_and_path3():
    """When emission_source_array is non-empty, its weighted sum should
    populate the per-gen rate even if both the per-gen JSON override and
    factored data would suggest a different value."""
    sys_ = _base_system_factored()
    sys_["generator_array"][0]["emission_rate"] = 999.0  # bogus, ignored
    sys_["emission_source_array"] = [
        {
            "uid": 1,
            "generator": 10,
            "zone": 1,
            "emission": "co2",
            "rate": 0.8,  # tCO2 / MWh per (g, e, z) triple
        },
    ]
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    # weight_co2 = 1.0, single source → rate = 1.0 × 0.8 = 0.8
    assert rate == pytest.approx(0.8, rel=1e-9)


def test_path1_aggregates_across_multiple_sources_with_zone_weights():
    """EmissionSourceLP per-pollutant α: weight_e × rate_e summed across
    all sources tied to the same generator.  Multi-pollutant fixture."""
    sys_ = _base_system_factored()
    sys_["emission_zone_array"][0]["emissions"] = [
        {"emission": "co2", "weight": 1.0},
        {"emission": "ch4", "weight": 28.0},
    ]
    sys_["emission_source_array"] = [
        {"uid": 1, "generator": 10, "zone": 1, "emission": "co2", "rate": 0.9},
        {"uid": 2, "generator": 10, "zone": 1, "emission": "ch4", "rate": 0.0002},
    ]
    planning = {"system": sys_}
    rate = _gen_emission_rate(planning)
    expected = 1.0 * 0.9 + 28.0 * 0.0002
    assert rate == pytest.approx(expected, rel=1e-9)


# ---------------------------------------------------------------------------
# Equivalence — paths produce identical numbers on identical physics
# ---------------------------------------------------------------------------


def test_three_paths_agree_when_pointed_at_same_physical_rate():
    """The factored data, JSON override, and emission_source_array
    should all produce the same per-gen rate when they encode the
    SAME physical fuel ↔ heat_rate relationship.

    This is the load-bearing invariant: downstream consumers
    (gtopt_marginal_units recipes, C++ EmissionSourceLP α coefficient,
    --only-emissions overlay) MUST see the same number regardless of
    which converter populated the JSON.
    """
    # Path 3: factored
    p3 = {"system": _base_system_factored()}
    r3 = _gen_emission_rate(p3)

    # Path 2: stamped per-gen aggregate, same value
    p2_sys = _base_system_factored()
    p2_sys["generator_array"][0]["emission_rate"] = EXPECTED_RATE
    # Wipe the fuel-side data so path 3 cannot fire
    p2_sys["fuel_array"][0]["emission_factors"] = []
    p2 = {"system": p2_sys}
    r2 = _gen_emission_rate(p2)

    # Path 1: emission_source_array, single source @ EXPECTED_RATE × weight=1
    p1_sys = _base_system_factored()
    p1_sys["fuel_array"][0]["emission_factors"] = []  # disable path 3
    p1_sys["emission_source_array"] = [
        {
            "uid": 1,
            "generator": 10,
            "zone": 1,
            "emission": "co2",
            "rate": EXPECTED_RATE,
        },
    ]
    p1 = {"system": p1_sys}
    r1 = _gen_emission_rate(p1)

    assert r1 == pytest.approx(EXPECTED_RATE, rel=1e-9)
    assert r2 == pytest.approx(EXPECTED_RATE, rel=1e-9)
    assert r3 == pytest.approx(EXPECTED_RATE, rel=1e-9)
    # Cross-path equivalence (the invariant under test)
    assert r1 == pytest.approx(r2, rel=1e-9)
    assert r2 == pytest.approx(r3, rel=1e-9)
