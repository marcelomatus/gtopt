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


# ---------------------------------------------------------------------------
# Issue #525 — phantom-bus / storage-marginal classification
# ---------------------------------------------------------------------------
from gtopt_marginal_units._zones import is_phantom_bus


def _make_gen(uid: int, name: str, bus_uid: int = 0, kind: str = "thermal"):
    from gtopt_canonical_feed import Generator

    return Generator(
        uid=uid,
        name=name,
        bus_uid=bus_uid,
        pmin=0.0,
        pmax=100.0,
        declared_MC=0.0,
        kind=kind,
        emission_rate=None,
    )


def test_525_is_phantom_bus_by_name_pattern():
    """Bus whose name ends with _int_bus is phantom."""
    assert is_phantom_bus("BAT_ARENALES_int_bus", []) is True
    assert is_phantom_bus("BAT_TOCOPILLA_INT_BUS", []) is True
    assert is_phantom_bus("Cardones220", []) is False
    assert is_phantom_bus("Polpaico500", []) is False


def test_525_is_phantom_bus_by_synthetic_gens():
    """Bus hosting only BAT_*_LOAD / BAT_*_gen synthetic gens is phantom."""
    bat_load = _make_gen(1, "BAT_DEL_DESIERTO_LOAD")
    bat_gen = _make_gen(2, "BAT_VICTOR_JARA_FV_gen")
    assert is_phantom_bus("some_bus", [bat_load]) is True
    assert is_phantom_bus("some_bus", [bat_gen]) is True
    assert is_phantom_bus("some_bus", [bat_load, bat_gen]) is True


def test_525_not_phantom_when_real_thermal_present():
    """Bus with a real thermal gen is NOT phantom (even with BAT_* siblings)."""
    bat_load = _make_gen(1, "BAT_X_LOAD")
    real = _make_gen(2, "ANGAMOS_1")
    assert is_phantom_bus("Crucero220", [bat_load, real]) is False


def test_525_not_phantom_for_real_bus_with_no_gens():
    """A real (non-_int_bus) bus with no gens is NOT phantom (it's a load-only bus)."""
    assert is_phantom_bus("Cardones220", []) is False


def test_525_phantom_classifier_returns_hydro_marginal():
    """End-to-end: a synthetic phantom-bus zone goes through
    _zone_results_from_lp_duals and ends up as hydro_marginal.
    Smoke test via a minimal topology fixture."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=10, name="BAT_TEST_int_bus")
    gen = _make_gen(100, "BAT_TEST_LOAD", bus_uid=10, kind="thermal")
    topo = Topology(buses=[bus], generators=[gen], lines=[])
    bus_to_zone, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={10: 47.5},
        dispatch_by_uid={},
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={100: True},
        demand_fail_cost=1000.0,
        topology_zone_of={10: 0},
    )
    assert 0 in zone_results
    zres = zone_results[0]
    assert zres.formula_kind == "hydro_marginal"
    assert zres.lambda_z == 47.5
    assert zres.marginal_gen_uids == [100]
    assert zres.reason == "phantom_bus_storage_marginal"


def test_525_real_bus_still_unattributed_when_no_match():
    """Real bus (not phantom) with no matching marginal still
    classifies as unattributed."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    # A bus with a positive-pmax thermal but NO LP dispatch (e.g. the
    # LP put the load somewhere else, gen sits idle) and LMP far above
    # demand_fail_cost — genuinely unattributed (no candidate, no
    # phantom, has capacity so NOT empty_island, not at demand_fail).
    bus = Bus(uid=20, name="Cardones220")
    idle_thermal = _make_gen(999, "IDLE", bus_uid=20)  # default pmax=100 > 0
    topo = Topology(buses=[bus], generators=[idle_thermal], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={20: 0.001},  # tiny non-zero, no match
        dispatch_by_uid={},  # idle thermal not dispatching
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={999: True},
        demand_fail_cost=1000.0,
        topology_zone_of={20: 0},
    )
    assert zone_results[0].formula_kind == "unattributed"


# ---------------------------------------------------------------------------
# Issue #526 — renewable_curtailment classification for em=0 + LMP=0 cells
# ---------------------------------------------------------------------------


def test_526_renewable_curtailment_classified_when_lmp_zero_and_profile_marginal():
    """LMP ≈ 0 + only profile gens dispatching → renewable_curtailment."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=30, name="SolarBus220")
    # One solar gen dispatching, no other candidates
    solar = _make_gen(200, "TARAPACA_FV", bus_uid=30, kind="profile")
    topo = Topology(buses=[bus], generators=[solar], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={30: 0.0},
        dispatch_by_uid={200: 50.0},
        gen_rc_by_uid={200: 0.0},
        gen_srmc_by_uid={200: 0.0},
        tol=Tolerances.default(),
        merit_eligible={200: True},  # last-resort allowance for profiles
        demand_fail_cost=1000.0,
        topology_zone_of={30: 0},
    )
    zres = zone_results[0]
    assert zres.formula_kind == "renewable_curtailment"
    assert zres.lambda_z == 0.0
    assert zres.marginal_gen_uids == [200]


def test_526_no_curtailment_when_lmp_positive():
    """LMP > tol → tied/single regardless of profile-only marginal."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=31, name="MixedBus")
    solar = _make_gen(201, "X_FV", bus_uid=31, kind="profile")
    topo = Topology(buses=[bus], generators=[solar], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={31: 50.0},  # non-zero LMP
        dispatch_by_uid={201: 30.0},
        gen_rc_by_uid={201: 0.0},
        gen_srmc_by_uid={201: 0.0},
        tol=Tolerances.default(),
        merit_eligible={201: True},
        demand_fail_cost=1000.0,
        topology_zone_of={31: 0},
    )
    assert zone_results[0].formula_kind != "renewable_curtailment"


def test_526_thermal_marginal_at_zero_lmp_still_tied():
    """LMP=0 + thermal marginal → tied_units (not renewable_curtailment)."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=32, name="ThermalBus")
    thermal = _make_gen(202, "CHEAP_THERMAL", bus_uid=32, kind="thermal")
    topo = Topology(buses=[bus], generators=[thermal], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={32: 0.0},
        dispatch_by_uid={202: 100.0},
        gen_rc_by_uid={202: 0.0},
        gen_srmc_by_uid={202: 0.0},
        tol=Tolerances.default(),
        merit_eligible={202: True},
        demand_fail_cost=1000.0,
        topology_zone_of={32: 0},
    )
    # Thermal at $0 (e.g. zero-cost coal in cost mode) still gets tagged
    # tied/single, NOT renewable_curtailment — the kind matters for
    # downstream emission accounting (thermal can emit; profile can't).
    assert zone_results[0].formula_kind in ("single_unit", "tied_units")


# ---------------------------------------------------------------------------
# Issue #523 — per-bus emission scaling via bus_LMP / Generator.srmc_sol
# ---------------------------------------------------------------------------


def test_523_scaling_one_when_bus_lmp_equals_srmc():
    """At the marginal's own bus (no losses), scaling = 1.0 → emission unchanged."""
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    coal = Generator(
        uid=1,
        name="ANGAMOS_1",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus = Bus(uid=10, name="Crucero220")
    topo = Topology(buses=[bus], generators=[coal], lines=[])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=35.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    # At g.bus: bus_LMP/SRMC = 35/35 = 1 → emission = 0.92 unchanged
    assert len(em_rows) == 1
    assert em_rows[0].recomputed_value == pytest.approx(0.92, abs=1e-6)


def test_523_scaling_above_one_for_lossy_downstream_bus():
    """Downstream bus with LMP > SRMC → emission scaled up by loss factor."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    coal = Generator(
        uid=1,
        name="COAL",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    line = Line(
        uid=100,
        bus_a_uid=10,
        bus_b_uid=20,
        tmax_ab=200.0,
        tmax_ba=200.0,
        reactance=0.1,
        active=True,
    )
    topo = Topology(buses=[bus_a, bus_b], generators=[coal], lines=[line])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=35.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 38.5},  # 10 % loss to B
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    # bus A: scaling = 1.0 → 0.92
    # bus B: scaling = 38.5/35 = 1.10 → 0.92 × 1.10 = 1.012
    em_by_bus = {r.bus_uid: r.recomputed_value for r in em_rows}
    assert em_by_bus[10] == pytest.approx(0.92, abs=1e-6)
    assert em_by_bus[20] == pytest.approx(0.92 * 1.10, abs=1e-4)


def test_523_scaling_capped_at_1_5():
    """Wild LMP jump (likely congestion leak) gets capped at 1.5×."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    coal = Generator(
        uid=1,
        name="COAL",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    line = Line(
        uid=100,
        bus_a_uid=10,
        bus_b_uid=20,
        tmax_ab=200.0,
        tmax_ba=200.0,
        reactance=0.1,
        active=True,
    )
    topo = Topology(buses=[bus_a, bus_b], generators=[coal], lines=[line])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=35.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 350.0},  # 10x LMP (congestion leak)
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    em_by_bus = {r.bus_uid: r.recomputed_value for r in em_rows}
    # 10× scaling clamps to 1.5×
    assert em_by_bus[20] == pytest.approx(0.92 * 1.5, abs=1e-4)


def test_523_no_scaling_for_storage_marginal():
    """Storage marginal (kind=battery) keeps scaling=1 by LP duality."""
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    bat = Generator(
        uid=1,
        name="BAT_1",
        bus_uid=10,
        pmin=0.0,
        pmax=100.0,
        declared_MC=0.0,
        kind="battery",
        emission_rate=0.0,
    )
    bus = Bus(uid=10, name="X")
    topo = Topology(buses=[bus], generators=[bat], lines=[])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=25.0,
        formula_kind="hydro_marginal",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 50.0},
        lmp_by_bus={10: 25.0},
        srmc_by_uid={1: 25.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    # Storage marginal — hydro_marginal kind → no scaling, em stays 0
    assert em_rows[0].recomputed_value == pytest.approx(0.0)


def test_523_no_scaling_when_srmc_unavailable():
    """Without srmc_by_uid, scaling = 1 (fall back to no correction)."""
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    coal = Generator(
        uid=1,
        name="C",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus = Bus(uid=10, name="X")
    topo = Topology(buses=[bus], generators=[coal], lines=[])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=35.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0},
        srmc_by_uid=None,  # not available
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    assert em_rows[0].recomputed_value == pytest.approx(0.92)


# ---------------------------------------------------------------------------
# Issue #43 — empty_island classification (real island with no demand,
# no gen-with-pmax, LMP=0).  Faithful "nobody's home" tag.
# ---------------------------------------------------------------------------


def test_43_empty_island_single_bus_no_gens():
    """Real single-bus island with NO generators at all + LMP=0 → empty_island."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=40, name="Cardones220")
    topo = Topology(buses=[bus], generators=[], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={40: 0.0},
        dispatch_by_uid={},
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={},
        demand_fail_cost=1000.0,
        topology_zone_of={40: 0},
    )
    assert zone_results[0].formula_kind == "empty_island"
    assert zone_results[0].lambda_z == 0.0
    assert zone_results[0].marginal_gen_uids == []


def test_43_empty_island_single_bus_decommissioned_gens():
    """Single-bus island with gens but ALL have pmax=0 → empty_island.

    The Ralco220 pattern: RALCO + PALMUCHO present but unavailable
    this stage (pmax=0).
    """
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=175, name="Ralco220")
    ralco = Generator(
        uid=65, name="RALCO", bus_uid=175, pmin=0.0, pmax=0.0,
        declared_MC=0.0, kind="thermal", emission_rate=None,
    )
    palmucho = Generator(
        uid=66, name="PALMUCHO", bus_uid=175, pmin=0.0, pmax=0.0,
        declared_MC=0.0, kind="thermal", emission_rate=None,
    )
    topo = Topology(buses=[bus], generators=[ralco, palmucho], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={175: 0.0},
        dispatch_by_uid={},
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={65: True, 66: True},
        demand_fail_cost=1000.0,
        topology_zone_of={175: 0},
    )
    assert zone_results[0].formula_kind == "empty_island"
    assert zone_results[0].reason == "island_has_no_active_capacity"


def test_43_empty_island_multi_bus():
    """Multi-bus disconnected sub-network with no demand or active
    generation → all classified as one empty_island zone."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    b1 = Bus(uid=41, name="DeadBus1")
    b2 = Bus(uid=42, name="DeadBus2")
    b3 = Bus(uid=43, name="DeadBus3")
    topo = Topology(buses=[b1, b2, b3], generators=[], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={41: 0.0, 42: 0.0, 43: 0.0},
        dispatch_by_uid={},
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={},
        demand_fail_cost=1000.0,
        topology_zone_of={41: 0, 42: 0, 43: 0},  # all same zone
    )
    assert zone_results[0].formula_kind == "empty_island"


def test_43_not_empty_when_gen_has_capacity_but_idle():
    """Bus with a positive-pmax gen that's NOT dispatching + LMP=0
    → NOT empty_island (capacity is there; LP just didn't pick it).
    Falls through to unattributed."""
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=44, name="X")
    gen = Generator(
        uid=900, name="IDLE_THERMAL", bus_uid=44, pmin=0.0, pmax=100.0,
        declared_MC=50.0, kind="thermal", emission_rate=0.5,
    )
    topo = Topology(buses=[bus], generators=[gen], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={44: 0.0},
        dispatch_by_uid={},  # gen idle
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={900: True},
        demand_fail_cost=1000.0,
        topology_zone_of={44: 0},
    )
    # Capacity present (pmax=100) → NOT empty_island; classifier falls
    # through to unattributed (no candidate, not at demand_fail, etc.)
    assert zone_results[0].formula_kind != "empty_island"


def test_43_not_empty_when_lmp_nonzero():
    """Real bus with no capacity but LMP > 0 → NOT empty_island.

    This protects against false positives when the LP price is
    elevated (something upstream is forcing flow); the bus might
    have no LOCAL gen but inherits the zone via tie lines.
    """
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units.constants import Tolerances
    from gtopt_marginal_units.main import _zone_results_from_lp_duals

    bus = Bus(uid=45, name="X")
    topo = Topology(buses=[bus], generators=[], lines=[])
    _, zone_results = _zone_results_from_lp_duals(
        topology=topo,
        lmp_by_bus={45: 47.5},  # non-zero LMP
        dispatch_by_uid={},
        gen_rc_by_uid=None,
        gen_srmc_by_uid=None,
        tol=Tolerances.default(),
        merit_eligible={},
        demand_fail_cost=1000.0,
        topology_zone_of={45: 0},
    )
    assert zone_results[0].formula_kind != "empty_island"
