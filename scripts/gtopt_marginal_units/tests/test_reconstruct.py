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


# ---------------------------------------------------------------------------
# Negative-λ clamp — sister case to the demand_fail_cost upper-clamp test.
# A catalogue with declared_MC < 0 (data error) must be pinned to zero with
# confidence=fallback. Without this test the second arm of ``_maybe_clamp``
# is effectively dead code from the test suite's point of view.
# ---------------------------------------------------------------------------


def test_negative_declared_mc_clamps_lambda_to_zero():
    gens = [
        _gen(10, 0, 100, mc=10.0),
        _gen(20, 0, 100, mc=-5.0),  # data error: negative MC
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        # Make the negative-MC unit the strict-interior price-setter.
        dispatch_by_uid={10: 100.0, 20: 50.0},
        zone_load=150.0,
        demand_fail_cost=_DFC,
    )
    assert res.lambda_z == 0.0
    assert res.clamped
    assert "clamped_negative_to_zero" in res.reason
    assert res.confidence == Confidence.FALLBACK


# ---------------------------------------------------------------------------
# Hydro-only interior fall-through (master §4.5 row): when no thermal unit
# is interior and only hydro/battery units are interior with declared_MC,
# the merit-order rule still fires (hydro participates in the max-MC pick)
# and the formula collapses to ``single_unit`` / ``tied_units``. The
# dedicated ``hydro_marginal`` formula only fires when interior units
# have ``declared_MC=None`` (water-value not externalised).
# ---------------------------------------------------------------------------


def test_hydro_with_declared_mc_interior_uses_merit_order_formula():
    gens = [
        _gen(10, 0, 100, mc=15.0, kind="thermal"),  # at pmax
        _gen(20, 0, 100, mc=8.0, kind="hydro"),  # interior with declared MC
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0, 20: 50.0},
        zone_load=150.0,
        demand_fail_cost=_DFC,
    )
    # ``with_mc`` branch wins because hydro has a declared_MC; the formula
    # is the merit-order single_unit form, NOT the hydro_marginal fallback.
    assert res.formula_kind == "single_unit"
    assert res.marginal_gen_uids == [20]
    assert res.lambda_z == 8.0
    assert res.confidence == Confidence.MERIT_ORDER
    assert not res.degenerate


def test_hydro_only_interior_with_no_declared_mc_is_degenerate():
    # Hydro with declared_MC=None: lambda_z falls back to 0 and the cell
    # is flagged degenerate so the consumer can audit via the recipe table.
    gens = [
        Generator(
            uid=20,
            name="g20",
            bus_uid=1,
            pmin=0,
            pmax=100,
            declared_MC=None,
            kind="hydro",
            emission_factor=0.0,
        ),
    ]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={20: 50.0},
        zone_load=50.0,
        demand_fail_cost=_DFC,
    )
    assert res.formula_kind == "hydro_marginal"
    assert res.lambda_z == 0.0
    assert res.degenerate


# ---------------------------------------------------------------------------
# ``merit_eligible_by_uid`` override — the picker must honour an explicit
# False entry by excluding that unit's pmax from the capacity-exhaustion
# sum (review P1-1: profile units never count toward Σpmax). Without this,
# a zone where the only excess capacity comes from curtailable solar would
# fail to flag demand_fail when the dispatch is structurally infeasible.
# ---------------------------------------------------------------------------


def test_merit_eligible_override_excludes_profile_from_pmax_sum():
    gens = [
        _gen(10, 0, 100, mc=10.0, kind="thermal"),  # eligible
        _gen(20, 0, 100, mc=0.0, kind="profile"),  # NOT eligible
    ]
    # Without the override, sum_pmax = 200 ≥ load=150 — interior path fires.
    # With profile excluded, eligible_pmax_sum = 100 < load=150 → demand_fail.
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0, 20: 50.0},
        zone_load=150.0,
        demand_fail_cost=_DFC,
        merit_eligible_by_uid={10: True, 20: False},
    )
    assert res.formula_kind == "demand_fail"
    assert res.reason == "zone_load_exceeds_sum_pmax"
    assert res.lambda_z == _DFC


# ---------------------------------------------------------------------------
# Empty zone (no eligible units) → unattributed terminal (review P0-6 / Step 5).
# ---------------------------------------------------------------------------


def test_empty_zone_yields_unattributed():
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=[],
        dispatch_by_uid={},
        zone_load=0.0,
        demand_fail_cost=_DFC,
    )
    assert res.formula_kind == "unattributed"
    assert res.lambda_z == 0.0
    assert res.degenerate
    assert res.confidence == Confidence.FALLBACK
    assert not res.marginal_gen_uids


def test_zone_with_no_interior_no_forced_pmin_match_yields_unattributed():
    # Single unit pinned at pmax (so Step 3 interior list is empty), but
    # Step 4 forced_pmin requires ``pmin > eps`` — the unit has pmin=0 so
    # the forced-pmin fallback also fails → unattributed.
    # Step 2 (all_at_pmax) does fire here, so to reach the unattributed
    # terminal we need pmin>0 forcing-no-fit but with the load mismatched.
    gens = [_gen(10, pmin=20, pmax=80, mc=15.0)]
    res = reconstruct_zone_lambda(
        zone_id=0,
        zone_buses=[1],
        generators_in_zone=gens,
        # dispatch at pmin = 20 → no interior, forced_pmin candidate.
        # Mismatched zone_load = 50 (>> sum_pmin=20, < pmax) so the
        # forced-pmin band-pass fails; load also < pmax so demand_fail
        # capacity exhaustion does not fire.
        dispatch_by_uid={10: 20.0},
        zone_load=50.0,
        demand_fail_cost=_DFC,
    )
    assert res.formula_kind == "unattributed"
    assert res.lambda_z == 0.0
    assert res.degenerate
    assert res.reason == "no_eligible_marginal_unit"


# ---------------------------------------------------------------------------
# Multi-zone aggregator: a zone with no generators (pure sink) must still
# emit a result so the consumer can detect it. With non-zero load on the
# sink, the capacity-exhaustion branch (Step 1) fires correctly. Locks in
# the iteration over ``sorted(set(zone_of.values()))`` in
# ``reconstruct_all_zones``.
# ---------------------------------------------------------------------------


def test_reconstruct_all_zones_emits_result_for_pure_sink_zone():
    gens = [
        Generator(
            uid=10,
            name="g10",
            bus_uid=1,
            pmin=0,
            pmax=100,
            declared_MC=10.0,
            kind="thermal",
            emission_factor=400.0,
        ),
    ]
    topo = Topology(buses=[], generators=gens, lines=[])
    zone_of = {1: 0, 2: 1}  # bus 2 is a pure sink (no generator)
    out = reconstruct_all_zones(
        topology=topo,
        zone_of=zone_of,
        dispatch_by_uid={10: 50.0},
        load_by_bus={1: 0.0, 2: 50.0},
        demand_fail_cost=_DFC,
    )
    assert set(out.keys()) == {0, 1}
    # Zone 1 has no generators → eligible_pmax_sum=0 < load=50 →
    # demand_fail (capacity exhaustion).
    assert out[1].formula_kind == "demand_fail"
    assert out[1].reason == "zone_load_exceeds_sum_pmax"
    assert out[1].lambda_z == _DFC
    assert out[1].degenerate


def test_reconstruct_all_zones_pure_sink_with_zero_load_is_unattributed():
    """A pure-sink zone with zero load has no capacity-exhaustion either —
    falls through every branch to the ``unattributed`` terminal."""
    gens = [
        Generator(
            uid=10,
            name="g10",
            bus_uid=1,
            pmin=0,
            pmax=100,
            declared_MC=10.0,
            kind="thermal",
            emission_factor=400.0,
        ),
    ]
    topo = Topology(buses=[], generators=gens, lines=[])
    zone_of = {1: 0, 2: 1}
    out = reconstruct_all_zones(
        topology=topo,
        zone_of=zone_of,
        dispatch_by_uid={10: 50.0},
        load_by_bus={1: 50.0, 2: 0.0},  # sink is idle
        demand_fail_cost=_DFC,
    )
    assert set(out.keys()) == {0, 1}
    assert out[1].formula_kind == "unattributed"
    assert out[1].degenerate
