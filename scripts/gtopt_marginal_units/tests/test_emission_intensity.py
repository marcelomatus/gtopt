# SPDX-License-Identifier: BSD-3-Clause
"""Emission-intensity recipe tests — master §4.12.

CI invariant: the price recipe and emission-intensity recipe must
share identical ``marginal_gen_uids`` for every (cell, bus) pair
(Lin & Tang 2024 — both formulas share the generation-sensitivity
term; only the per-unit datum differs).
"""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import Generator, Topology
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units._recipes import build_recipes_for_cell
from gtopt_marginal_units.constants import Confidence, FormulaKind


_CELL = (1, 1, 1, None, None, "simulated")


def _gen_thermal(uid: int, mc: float, ef: float) -> Generator:
    return Generator(
        uid=uid,
        name=f"g{uid}",
        bus_uid=1,
        pmin=0,
        pmax=100,
        declared_MC=mc,
        kind="thermal",
        emission_rate=ef,
    )


def _zres(kind: str, uids: list[int], lambda_z: float):
    return ZoneR3Result(
        zone_id=0,
        lambda_z=lambda_z,
        formula_kind=kind,
        marginal_gen_uids=uids,
        confidence=Confidence.MERIT_ORDER,
        degenerate=False,
        reason="test",
        clamped=False,
    )


def test_three_unit_emission_attribution():
    """Gas (low MC, low EF) | coal (mid MC, high EF) | hydro (low EF).

    Coal sets the price → ε_b should equal coal's emission factor."""
    topo = Topology(
        buses=[],
        generators=[
            _gen_thermal(10, mc=10.0, ef=400.0),  # gas
            _gen_thermal(20, mc=30.0, ef=950.0),  # coal — sets price
            Generator(
                uid=30,
                name="hydro",
                bus_uid=1,
                pmin=0,
                pmax=100,
                declared_MC=None,
                kind="hydro",
                emission_rate=0.0,
            ),
        ],
        lines=[],
    )
    zr = {0: _zres(FormulaKind.SINGLE_UNIT.value, [20], lambda_z=30.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert price[0].recomputed_value == pytest.approx(30.0)
    # Marginal unit is coal → ε_b = 950.
    assert em[0].recomputed_value == pytest.approx(950.0)


def test_lin_tang_invariant_same_uids_in_both_recipes():
    """The Lin & Tang 2024 result mandates that price and emission
    recipes share marginal_gen_uids for each (cell, bus). Verify
    on tied units (where the invariant is least obvious)."""
    topo = Topology(
        buses=[],
        generators=[
            _gen_thermal(10, mc=20.0, ef=400.0),
            _gen_thermal(20, mc=20.0, ef=900.0),  # tied MC, very different EF
        ],
        lines=[],
    )
    zr = {0: _zres(FormulaKind.TIED_UNITS.value, [10, 20], lambda_z=20.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    # Same UIDs, same weights — only the per-unit data differ.
    for p_row, e_row in zip(price, em):
        assert p_row.marginal_gen_uids == e_row.marginal_gen_uids
        assert p_row.marginal_weights == e_row.marginal_weights
    # Mean MC = 20; mean EF = 650.
    assert price[0].recomputed_value == pytest.approx(20.0)
    assert em[0].recomputed_value == pytest.approx(650.0)


def test_demand_fail_emission_zero_with_explanation():
    """Master §4.12.2 corner: demand_fail snaps price to the cap but
    emission to zero (no MWh generated)."""
    topo = Topology(buses=[], generators=[_gen_thermal(10, 10.0, 400.0)], lines=[])
    zr = {0: _zres(FormulaKind.DEMAND_FAIL.value, [], lambda_z=1000.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
        demand_fail_cost=1000.0,
    )
    assert price[0].recomputed_value == 1000.0
    assert em[0].recomputed_value == 0.0
    assert "rationing" in em[0].formula_explanation.lower()
