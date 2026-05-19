# SPDX-License-Identifier: BSD-3-Clause
"""Recipe-table tests — bus_price_recipe + bus_emission_intensity_recipe."""

from __future__ import annotations

import math

import pytest

from gtopt_canonical_feed import Generator, Topology
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units._recipes import build_recipes_for_cell
from gtopt_marginal_units.constants import Confidence, FormulaKind


_CELL = (1, 1, 1, None, None, "simulated")


def _gen(uid: int, mc: float, ef: float = 400.0, kind: str = "thermal") -> Generator:
    return Generator(
        uid=uid,
        name=f"g{uid}",
        bus_uid=1,
        pmin=0.0,
        pmax=100.0,
        declared_MC=mc,
        kind=kind,
        emission_rate=ef,
    )


def _zone_result(formula_kind: str, marginal_uids, lambda_z=20.0, **kw):
    return ZoneR3Result(
        zone_id=0,
        lambda_z=lambda_z,
        formula_kind=formula_kind,
        marginal_gen_uids=list(marginal_uids),
        confidence=Confidence.MERIT_ORDER,
        degenerate=False,
        reason="test",
        clamped=False,
        **kw,
    )


def test_single_unit_recipe_round_trip():
    topo = Topology(buses=[], generators=[_gen(10, mc=20.0, ef=400.0)], lines=[])
    zone_of = {1: 0}
    zr = {0: _zone_result(FormulaKind.SINGLE_UNIT.value, [10], lambda_z=20.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of=zone_of,
        zone_results=zr,
    )
    assert len(price) == 1 and len(em) == 1
    assert price[0].marginal_gen_uids == [10]
    assert price[0].marginal_data == [20.0]
    assert price[0].recomputed_value == pytest.approx(20.0)
    assert em[0].marginal_data == [400.0]
    assert em[0].recomputed_value == pytest.approx(400.0)
    # Lin & Tang invariant: identical uids on both sides.
    assert price[0].marginal_gen_uids == em[0].marginal_gen_uids
    assert price[0].marginal_weights == em[0].marginal_weights


def test_tied_units_recipe_uses_uniform_weights():
    topo = Topology(
        buses=[],
        generators=[
            _gen(10, mc=20.0, ef=400.0),
            _gen(20, mc=20.0, ef=600.0),
        ],
        lines=[],
    )
    zr = {0: _zone_result(FormulaKind.TIED_UNITS.value, [10, 20], lambda_z=20.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert price[0].marginal_weights == [0.5, 0.5]
    # Recomputed price = mean of MCs.
    assert price[0].recomputed_value == pytest.approx(20.0)
    # Recomputed emission = mean of emission factors = 500.
    assert em[0].recomputed_value == pytest.approx(500.0)


def test_demand_fail_yields_zero_emission():
    topo = Topology(buses=[], generators=[_gen(10, mc=10.0)], lines=[])
    zr = {0: _zone_result(FormulaKind.DEMAND_FAIL.value, [], lambda_z=1000.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
        demand_fail_cost=1000.0,
    )
    assert price[0].recomputed_value == 1000.0
    assert price[0].marginal_gen_uids == []
    # Emission must be zero per master §4.12.2 (load shedding has no MWh).
    assert em[0].recomputed_value == 0.0


def test_renewable_curtailment_yields_zero_price_and_emission():
    topo = Topology(buses=[], generators=[], lines=[])
    zr = {
        0: _zone_result(
            FormulaKind.RENEWABLE_CURTAILMENT.value,
            [],
            lambda_z=0.0,
        )
    }
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert price[0].recomputed_value == 0.0
    assert em[0].recomputed_value == 0.0


def test_unattributed_yields_recipe_row_with_explanation():
    topo = Topology(buses=[], generators=[], lines=[])
    zr = {0: _zone_result(FormulaKind.UNATTRIBUTED.value, [], lambda_z=0.0)}
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert price[0].formula_kind == "unattributed"
    assert "NA" in price[0].formula_explanation
    assert "NA" in em[0].formula_explanation


def test_hydro_marginal_emission_is_zero():
    topo = Topology(
        buses=[],
        generators=[
            _gen(10, mc=15.0, ef=0.0, kind="hydro"),
        ],
        lines=[],
    )
    zr = {
        0: _zone_result(
            FormulaKind.HYDRO_MARGINAL.value,
            [10],
            lambda_z=15.0,
        )
    }
    price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert price[0].recomputed_value == pytest.approx(15.0)
    # Hydro at the bus bar is zero-emission by the master plan convention.
    assert em[0].recomputed_value == 0.0


def test_missing_emission_rate_yields_nan():
    topo = Topology(
        buses=[],
        generators=[
            Generator(
                uid=10,
                name="g10",
                bus_uid=1,
                pmin=0,
                pmax=100,
                declared_MC=20.0,
                kind="thermal",
                emission_rate=None,
            ),
        ],
        lines=[],
    )
    zr = {0: _zone_result(FormulaKind.SINGLE_UNIT.value, [10], lambda_z=20.0)}
    _price, em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )
    assert math.isnan(em[0].marginal_data[0])
