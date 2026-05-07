# SPDX-License-Identifier: BSD-3-Clause
"""§4.7 R3 reconstruction tests — five regimes, with the all-pmax
corner asserting the lp-numerics P0.4 fix."""

from __future__ import annotations


from gtopt_canonical_feed import Generator, Topology
from gtopt_marginal_units._reconstruct import (
    reconstruct_all_zones,
    reconstruct_zone_lambda,
)
from gtopt_marginal_units.constants import Confidence


_DFC = 1000.0  # demand_fail_cost


def _gen(
    uid: int, pmin: float, pmax: float, mc: float, kind: str = "thermal"
) -> Generator:
    return Generator(
        uid=uid,
        name=f"g{uid}",
        bus_uid=1,
        pmin=pmin,
        pmax=pmax,
        declared_MC=mc,
        kind=kind,
        emission_factor=400.0,
    )


# ---------------------------------------------------------------------------
# Regime 1 — interior unit sets price (textbook merit-order)
# ---------------------------------------------------------------------------


def test_interior_unit_sets_price():
    gens = [
        _gen(10, 0, 100, mc=10.0),  # cheap
        _gen(20, 0, 100, mc=30.0),  # interior, sets price
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0, 20: 50.0},  # gen 20 strictly interior
        zone_load=150.0,
        demand_fail_cost=_DFC,
    )
    assert res.lambda_z == 30.0
    assert res.formula_kind == "single_unit"
    assert res.marginal_gen_uids == [20]
    assert res.confidence == Confidence.MERIT_ORDER
    assert not res.degenerate


# ---------------------------------------------------------------------------
# Regime 2 — all units pinned at pmax → demand_fail (lp-numerics P0.4)
# ---------------------------------------------------------------------------


def test_all_pmax_pinned_yields_demand_fail():
    gens = [
        _gen(10, 0, 100, mc=10.0),
        _gen(20, 0, 100, mc=30.0),
    ]
    # Both at pmax — earlier drafts wrongly used cheapest pmax MC
    # (10.0). Master plan now mandates λ_z = demand_fail_cost.
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0, 20: 100.0},
        zone_load=200.0,
        demand_fail_cost=_DFC,
    )
    assert res.lambda_z == _DFC
    assert res.formula_kind == "demand_fail"
    assert res.confidence == Confidence.FALLBACK
    assert res.reason == "all_units_at_pmax"


# ---------------------------------------------------------------------------
# Regime 3 — forced-pmin zone-picker fallback
# ---------------------------------------------------------------------------


def test_forced_pmin_zone_picker_fallback():
    # Single must-run unit at pmin = 20, demand exactly 20.
    gens = [_gen(10, pmin=20, pmax=80, mc=15.0)]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 20.0},
        zone_load=20.0,
        demand_fail_cost=_DFC,
    )
    assert res.formula_kind == "forced_pmin_marginal"
    assert res.lambda_z == 15.0
    assert res.marginal_gen_uids == [10]
    assert res.confidence == Confidence.MERIT_ORDER
    assert res.degenerate


# ---------------------------------------------------------------------------
# Regime 4 — load > Σ pmax → demand_fail
# ---------------------------------------------------------------------------


def test_load_exceeds_sum_pmax_yields_demand_fail():
    gens = [_gen(10, 0, 100, mc=10.0)]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0},
        zone_load=150.0,  # exceeds pmax
        demand_fail_cost=_DFC,
    )
    assert res.lambda_z == _DFC
    assert res.formula_kind == "demand_fail"
    assert res.reason == "zone_load_exceeds_sum_pmax"


# ---------------------------------------------------------------------------
# Regime 5 — λ_z above demand_fail_cost gets clamped (master §4.7 final clamp)
# ---------------------------------------------------------------------------


def test_lambda_above_cap_clamps_to_demand_fail_cost():
    # A generator with declared MC above the rationing cap is a
    # catalogue error; the script must clamp rather than emit a
    # bogus price.
    gens = [
        _gen(10, 0, 100, mc=10.0),
        _gen(20, 0, 100, mc=2_000.0),  # MC > demand_fail_cost = 1000
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0, 20: 50.0},
        zone_load=150.0,
        demand_fail_cost=_DFC,
    )
    assert res.lambda_z == _DFC
    assert res.clamped
    assert "clamped_to_demand_fail_cost" in res.reason
    assert res.confidence == Confidence.FALLBACK


# ---------------------------------------------------------------------------
# Tied MCs → tied_units formula
# ---------------------------------------------------------------------------


def test_tied_mc_produces_tied_units_formula():
    gens = [
        _gen(10, 0, 100, mc=20.0),
        _gen(20, 0, 100, mc=20.0),  # exact tie
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 50.0, 20: 50.0},
        zone_load=100.0,
        demand_fail_cost=_DFC,
    )
    assert res.formula_kind == "tied_units"
    assert sorted(res.marginal_gen_uids) == [10, 20]
    assert res.lambda_z == 20.0


# ---------------------------------------------------------------------------
# Multi-zone end-to-end
# ---------------------------------------------------------------------------


def test_multi_zone_independent_pricing():
    # Zone 0: cheap interior unit. Zone 1: peaker interior.
    gens = [
        _gen(10, 0, 100, mc=10.0),
        _gen(20, 0, 100, mc=80.0),
    ]
    # Patch bus uids so they're in different zones.
    gens[0] = Generator(
        uid=10,
        name="g10",
        bus_uid=1,
        pmin=0,
        pmax=100,
        declared_MC=10.0,
        kind="thermal",
        emission_factor=400.0,
    )
    gens[1] = Generator(
        uid=20,
        name="g20",
        bus_uid=2,
        pmin=0,
        pmax=100,
        declared_MC=80.0,
        kind="thermal",
        emission_factor=700.0,
    )
    topo = Topology(buses=[], generators=gens, lines=[])
    zone_of = {1: 0, 2: 1}
    out = reconstruct_all_zones(
        topology=topo,
        zone_of=zone_of,
        dispatch_by_uid={10: 50.0, 20: 50.0},
        load_by_bus={1: 50.0, 2: 50.0},
        demand_fail_cost=_DFC,
    )
    assert out[0].lambda_z == 10.0
    assert out[1].lambda_z == 80.0
