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
from gtopt_marginal_units._zones import is_phantom_bus  # noqa: E402


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


def _coal_two_bus_topology():
    """Helper: 2 buses, one thermal at bus 10, line 10↔20.

    Bus 20 is a radial leaf (degree 1) → on a single-bridge path to
    the marginal generator.  Use this fixture in tests where the
    'radial' classification is the intended behaviour.  For warn-
    clamp / error-raise tests use ``_coal_meshed_topology()`` instead
    so bus 20 sits in a 2-edge-connected component.
    """
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

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
    return Topology(buses=[bus_a, bus_b], generators=[coal], lines=[line])


def _coal_meshed_topology():
    """Helper: 3-bus triangle (no bridges → no radial buses) used in
    warn-clamp / error-raise tests where a meshed topology is needed
    to exercise the non-radial branch of the loss-factor scaling.
    Bus 20 is reported as the downstream bus."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

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
    bus_c = Bus(uid=30, name="C")
    lines = [
        Line(
            uid=100,
            bus_a_uid=10,
            bus_b_uid=20,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=101,
            bus_a_uid=20,
            bus_b_uid=30,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=102,
            bus_a_uid=10,
            bus_b_uid=30,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        ),
    ]
    return Topology(buses=[bus_a, bus_b, bus_c], generators=[coal], lines=lines)


def _coal_zone_result():
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )

    return ZoneR3Result(
        zone_id=0,
        lambda_z=35.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )


def test_523_warn_band_meshed_applies_raw_with_warn_meshed_status():
    """raw ∈ (warn, error] on a MESHED bus → APPLY raw (no clamp);
    status 'warn_meshed' so the end-of-run summary can warn about
    suspicious meshed-network ratios.  (Radial gets the same raw
    scaling but status 'warn_radial' — see the radial test.)"""
    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units.constants import Tolerances

    # raw = 105/35 = 3.0 (between warn=2.0 and error=5.0).
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=_coal_meshed_topology(),
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: _coal_zone_result()},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 105.0, 30: 35.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Raw scaling APPLIED (no clamp).  Status distinguishes the case.
    assert rows_by_bus[20].recomputed_value == pytest.approx(0.92 * 3.0, rel=1e-9)
    assert rows_by_bus[20].loss_factor_status == "warn_meshed"
    assert rows_by_bus[20].loss_factor_raw == pytest.approx(3.0, rel=1e-9)
    # Unscaled (raw == 1.0) cell on bus 10 → status "ok".
    assert rows_by_bus[10].loss_factor_status == "ok"
    assert rows_by_bus[10].loss_factor_raw == pytest.approx(1.0, rel=1e-9)


def test_523_above_error_threshold_meshed_is_critical_not_raises():
    """raw > error on a MESHED bus does NOT raise.  We trust the LP
    ratio (we have no principled way to say it's wrong) and apply raw,
    but flag it as ``critical_meshed`` so the end-of-run summary
    surfaces it loudly."""
    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units.constants import Tolerances

    # raw = 350 / 35 = 10 → above default error threshold (5.0)
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=_coal_meshed_topology(),
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: _coal_zone_result()},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 350.0, 30: 35.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    assert rows_by_bus[20].loss_factor_status == "critical_meshed"
    assert rows_by_bus[20].recomputed_value == pytest.approx(0.92 * 10.0, rel=1e-9)
    assert rows_by_bus[20].loss_factor_raw == pytest.approx(10.0, rel=1e-9)


def test_523_error_threshold_pure_warning_boundary():
    """``--loss-factor-error`` is now a warning-bucket boundary, not
    an abort condition.  Raising it changes a 'critical' to a 'warn'
    bucket but doesn't change the applied scale."""
    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units.constants import Tolerances

    tol_wide = Tolerances(loss_factor_warn=2.0, loss_factor_error=20.0)
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=_coal_meshed_topology(),
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: _coal_zone_result()},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 350.0, 30: 35.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=tol_wide,
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # raw=10 within the wider envelope → warn (not critical), still applied raw.
    assert rows_by_bus[20].loss_factor_status == "warn_meshed"
    assert rows_by_bus[20].recomputed_value == pytest.approx(0.92 * 10.0, rel=1e-9)
    assert rows_by_bus[20].loss_factor_raw == pytest.approx(10.0, rel=1e-9)


def test_523_negative_raw_skips_scaling():
    """Negative bus_LMP (oversupply) → scale=1.0, status='negative'."""
    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units.constants import Tolerances

    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=_coal_two_bus_topology(),
        zone_of={10: 0, 20: 0},
        zone_results={0: _coal_zone_result()},
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: -5.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Unscaled emission factor preserved.
    assert rows_by_bus[20].recomputed_value == pytest.approx(0.92, rel=1e-9)
    assert rows_by_bus[20].loss_factor_status == "negative"
    assert rows_by_bus[20].loss_factor_raw < 0.0


def test_523_cross_island_marks_status_does_not_scale():
    """When bus_i and bus(marginal_gen) live in DIFFERENT zones (i.e.
    different connected components of the topology after dropping
    saturated lines), the loss-factor concept is undefined.  The
    recipe should record ``loss_factor_status='cross_island'`` and
    leave the emission factor unscaled."""
    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units.constants import Tolerances

    # Coal at bus 10, but caller passes a zone_of where bus 10 is in
    # zone 0 and bus 20 is in zone 1 (saturated tie ⇒ separate
    # components).  Without the cross-island guard, raw = 350/35 = 10
    # would trip the loss-factor error.
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=_coal_two_bus_topology(),
        zone_of={10: 0, 20: 1},  # cross-island
        zone_results={
            0: _coal_zone_result(),
            1: _coal_zone_result(),  # same gen synthetically the marginal of zone 1 too
        },
        dispatch_by_uid={1: 150.0},
        lmp_by_bus={10: 35.0, 20: 350.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Bus 20 is cross-island → status cross_island, EF unscaled.
    assert rows_by_bus[20].loss_factor_status == "cross_island"
    assert rows_by_bus[20].recomputed_value == pytest.approx(0.92, rel=1e-9)
    # raw still recorded for audit.
    assert rows_by_bus[20].loss_factor_raw == pytest.approx(10.0, rel=1e-9)


def test_523_radial_bus_applies_full_scale_no_clamp():
    """On a radial corridor (bus reachable from the marginal only via
    bridges), the LP-derived bus_LMP / SRMC ratio IS the legitimate
    R-driven loss factor.  Apply the full raw scale — do NOT clamp to
    loss_factor_warn — even when raw > warn.  Status 'radial'."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    # 3-bus radial chain:  bus_a (coal) ──── bus_b ──── bus_c
    # Every edge is a bridge → bus_b and bus_c are radial (their
    # 2-edge-connected components have size 1).
    coal = Generator(
        uid=1,
        name="COAL",
        bus_uid=10,
        pmin=0.0,
        pmax=500.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    bus_c = Bus(uid=30, name="C")  # tail of radial
    lines = [
        Line(
            uid=100,
            bus_a_uid=10,
            bus_b_uid=20,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=101,
            bus_a_uid=20,
            bus_b_uid=30,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        ),
    ]
    topo = Topology(buses=[bus_a, bus_b, bus_c], generators=[coal], lines=lines)
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
    # raw = 175/35 = 5.0 on bus 30 (tail of radial)
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 100.0},
        lmp_by_bus={10: 35.0, 20: 70.0, 30: 175.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Radial tail → full raw scaling applied; status warn_radial (raw=5
    # is at the error threshold = critical_radial under default tols).
    assert rows_by_bus[30].loss_factor_status in (
        "warn_radial",
        "critical_radial",
    )
    assert rows_by_bus[30].recomputed_value == pytest.approx(0.92 * 5.0, rel=1e-9)
    assert rows_by_bus[30].loss_factor_raw == pytest.approx(5.0, rel=1e-9)


def test_523_meshed_bus_still_clamps_on_warn():
    """On a meshed network (multiple paths between buses), high raw is
    suspicious and gets clamped to loss_factor_warn — NOT applied as raw."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    # 3-bus ring:  bus_a (coal) ────── bus_b
    #                  ╲           ╱
    #                    ──bus_c──
    # Every edge is part of a cycle → no bridges → no radial buses.
    coal = Generator(
        uid=1,
        name="COAL",
        bus_uid=10,
        pmin=0.0,
        pmax=500.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    bus_c = Bus(uid=30, name="C")
    lines = [
        Line(
            uid=100,
            bus_a_uid=10,
            bus_b_uid=20,
            tmax_ab=200,
            tmax_ba=200,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=101,
            bus_a_uid=20,
            bus_b_uid=30,
            tmax_ab=200,
            tmax_ba=200,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=102,
            bus_a_uid=10,
            bus_b_uid=30,
            tmax_ab=200,
            tmax_ba=200,
            reactance=0.1,
            active=True,
        ),
    ]
    topo = Topology(buses=[bus_a, bus_b, bus_c], generators=[coal], lines=lines)
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
    # raw = 175/35 = 5.0 on bus 30 (in a ring)
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 100.0},
        lmp_by_bus={10: 35.0, 20: 35.0, 30: 175.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Ring (meshed) → raw applied, status warn_meshed / critical_meshed
    # depending on whether raw=5 is at or above the error boundary.
    assert rows_by_bus[30].loss_factor_status in (
        "warn_meshed",
        "critical_meshed",
    )
    assert rows_by_bus[30].recomputed_value == pytest.approx(0.92 * 5.0, rel=1e-9)
    assert rows_by_bus[30].loss_factor_raw == pytest.approx(5.0, rel=1e-9)


def test_523_phantom_bus_skips_loss_factor():
    """Synthetic BAT_*_int_bus phantom nodes (#525) have LP-internal
    LMP / SRMC values that aren't physical transmission quantities.
    The loss-factor scaling must skip them (status='phantom_bus',
    scale=1.0) even if the same-island + same-zone check would
    otherwise pass.
    """
    from gtopt_canonical_feed import Bus, Generator, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    # Phantom BESS wrap: BAT_SANTA_MARTA_int_bus + a single thermal
    # wrapper SANTA_MARTA at it.  Real PLEXOS pattern.
    g_san = Generator(
        uid=1,
        name="SANTA_MARTA",
        bus_uid=100,
        pmin=0.0,
        pmax=20.0,
        declared_MC=15.0,
        kind="thermal",
        emission_rate=0.4,
    )
    phantom_bus = Bus(uid=100, name="BAT_SANTA_MARTA_int_bus")
    topo = Topology(buses=[phantom_bus], generators=[g_san], lines=[])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=15.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    # Wild LP-internal ratio bus_LMP / SRMC = 111.6 / 15 = 7.44 — exactly
    # the SANTA_MARTA pattern seen in the 2y sc51 diagnostic.
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={100: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 10.0},
        lmp_by_bus={100: 111.6},
        srmc_by_uid={1: 15.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Phantom guard fired — status=phantom_bus, EF unscaled.
    assert rows_by_bus[100].loss_factor_status == "phantom_bus"
    assert rows_by_bus[100].recomputed_value == pytest.approx(0.4, rel=1e-9)
    # raw NOT recorded (synthetic LMP is not a physical signal).
    assert rows_by_bus[100].loss_factor_raw == pytest.approx(1.0, rel=1e-9)


def test_effective_emission_intensity_uses_direct_when_thermal_marginal():
    """When the LP marginal is a real thermal (direct EF > 0), the
    derived 'effective' column equals the direct EF — not the
    consequential walked-up value (which would be a different
    second-up gen).  Pins the column contract."""
    from gtopt_canonical_feed import Bus, Generator, Topology

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
    bus = Bus(uid=10, name="A")
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
        dispatch_by_uid={1: 100.0},
        lmp_by_bus={10: 35.0},
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    row = em_rows[0].to_dict("em")
    assert row["recomputed_emission_intensity"] == pytest.approx(0.92, rel=1e-9)
    # Effective column equals direct when direct > 0.
    assert row["effective_emission_intensity_t_per_mwh"] == pytest.approx(
        0.92, rel=1e-9
    )


def test_effective_emission_intensity_uses_consequential_when_hydro_marginal():
    """When the LP marginal is a hydro (direct EF = 0) BUT the
    consequential walk-up finds a real thermal, 'effective' picks up
    the consequential value.  This is the Issue #1 light fix:
    downstream tools get the economically-meaningful marginal CO2
    without needing a conditional join on loss_factor_status."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    hydro = Generator(
        uid=1,
        name="HYDRO",
        bus_uid=10,
        pmin=0.0,
        pmax=200.0,
        declared_MC=0.0,
        kind="hydro",
        emission_rate=0.0,
    )
    backup_coal = Generator(
        uid=2,
        name="BACKUP_COAL",
        bus_uid=20,
        pmin=0.0,
        pmax=100.0,
        declared_MC=30.0,
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
    topo = Topology(buses=[bus_a, bus_b], generators=[hydro, backup_coal], lines=[line])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=0.05,  # near-zero hydro LMP
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
        zone_of={10: 0, 20: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 100.0, 2: 0.0},  # coal idle (backup)
        lmp_by_bus={10: 0.05, 20: 0.06},
        srmc_by_uid={1: 0.0, 2: 30.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    for bus_uid in (10, 20):
        d = rows_by_bus[bus_uid].to_dict("em")
        # Direct = 0 (hydro marginal); consequential picks up backup coal.
        assert d["recomputed_emission_intensity"] == pytest.approx(0.0)
        assert d["consequential_co2eq_t_per_mwh"] > 0.0
        # Effective falls back to consequential.
        assert d["effective_emission_intensity_t_per_mwh"] == pytest.approx(
            d["consequential_co2eq_t_per_mwh"], rel=1e-9
        )


def test_523_consequential_uses_walked_up_gen_lossfactor():
    """When the LP-elected marginal is a storage (em ≈ 0) and the
    consequential MOER walks up to a different (non-dispatching)
    thermal, the consequential scaling MUST use the walked-up gen's
    static ``lossfactor`` — NOT the LP-derived bus_LMP / SRMC ratio
    (which is meaningless for a unit that isn't generating)."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    # Battery sets the marginal at bus 10 (em=0).  A non-dispatching
    # thermal sits at bus 30 with emission_rate=0.92 and
    # lossfactor=0.04 (4% aux-use → 1/(1-0.04) ≈ 1.0417 scale).
    bat = Generator(
        uid=1,
        name="BAT_STORAGE",
        bus_uid=10,
        pmin=0.0,
        pmax=100.0,
        declared_MC=20.0,
        kind="battery",
        emission_rate=0.0,
    )
    coal = Generator(
        uid=2,
        name="COAL_BACKUP",
        bus_uid=30,
        pmin=0.0,
        pmax=200.0,
        declared_MC=30.0,
        kind="thermal",
        emission_rate=0.92,
        lossfactor=0.04,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    bus_c = Bus(uid=30, name="C")
    lines = [
        Line(
            uid=100,
            bus_a_uid=10,
            bus_b_uid=20,
            tmax_ab=100.0,
            tmax_ba=100.0,
            reactance=0.1,
            active=True,
        ),
        Line(
            uid=101,
            bus_a_uid=20,
            bus_b_uid=30,
            tmax_ab=100.0,
            tmax_ba=100.0,
            reactance=0.1,
            active=True,
        ),
    ]
    topo = Topology(buses=[bus_a, bus_b, bus_c], generators=[bat, coal], lines=lines)
    # Battery is the marginal (hydro_marginal formula_kind in the recipe).
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=20.0,
        formula_kind="hydro_marginal",  # bypasses loss-factor scaling
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0, 30: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 50.0, 2: 0.0},  # coal idle (not dispatching)
        lmp_by_bus={10: 20.0, 20: 22.0, 30: 24.0},
        srmc_by_uid={1: 20.0, 2: 30.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Direct attribution: battery em=0 → direct = 0 everywhere.
    # Consequential attribution: COAL_BACKUP em=0.92, scaled by
    # 1/(1-0.04) = 1.04167 — NOT by the LP-derived 22/20 ratio at bus 20
    # (battery is not the consequential source).
    expected_cons = 0.92 / (1.0 - 0.04)
    # All buses in the zone share the same consequential MOER (it's a
    # per-zone quantity); per-bus value differs only via direct scaling.
    for bus_uid in (10, 20, 30):
        assert rows_by_bus[bus_uid].consequential_co2eq == pytest.approx(
            expected_cons, rel=1e-9
        )
        assert rows_by_bus[bus_uid].consequential_gen_uid == 2
    # Direct emission stays at 0 (battery is the marginal).
    for bus_uid in (10, 20, 30):
        assert rows_by_bus[bus_uid].recomputed_value == pytest.approx(0.0)


def test_523_consequential_shortcut_uses_lp_scale_when_marginal_emits():
    """When the LP-marginal IS a real combustion thermal (em > 0), the
    'shortcut' kicks in: cons_rate = direct_em.  In that case the
    LP-derived ``scale`` IS valid for the marginal (it actually
    dispatches) — we should NOT switch to its lossfactor."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    # Single dispatching coal marginal with emission_rate=0.92 and a
    # lossfactor=0.04.  raw = 35/35 = 1.0 → no scaling.
    coal = Generator(
        uid=1,
        name="COAL",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=0.92,
        lossfactor=0.04,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    lines = [
        Line(
            uid=100,
            bus_a_uid=10,
            bus_b_uid=20,
            tmax_ab=200.0,
            tmax_ba=200.0,
            reactance=0.1,
            active=True,
        )
    ]
    topo = Topology(buses=[bus_a, bus_b], generators=[coal], lines=lines)
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
        dispatch_by_uid={1: 100.0},
        lmp_by_bus={10: 35.0, 20: 35.0},  # raw = 1.0
        srmc_by_uid={1: 35.0},
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Direct == Consequential shortcut hit: cons_rate = direct (not
    # rescaled via lossfactor).  Both equal 0.92 (raw=1 → scale=1).
    assert rows_by_bus[10].consequential_co2eq == pytest.approx(0.92, rel=1e-9)
    assert rows_by_bus[10].consequential_gen_uid == 1
    assert rows_by_bus[10].recomputed_value == pytest.approx(0.92, rel=1e-9)


def test_523_storage_marginal_uses_lmp_at_storage_bus():
    """When the marginal is a storage element (battery/hydro), the
    scaling denominator is LMP at the storage's bus — not SRMC."""
    from gtopt_canonical_feed import Bus, Generator, Line, Topology

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
        # Battery bids at its opportunity cost (water/storage value);
        # must match lambda_z so the price-recipe round-trip succeeds.
        declared_MC=20.0,
        kind="battery",
        emission_rate=0.0,
    )
    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    line = Line(
        uid=100,
        bus_a_uid=10,
        bus_b_uid=20,
        tmax_ab=50.0,
        tmax_ba=50.0,
        reactance=0.1,
        active=True,
    )
    topo = Topology(buses=[bus_a, bus_b], generators=[bat], lines=[line])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=20.0,
        formula_kind="single_unit",
        marginal_gen_uids=[1],
        confidence=Confidence.LP_DUAL,
        degenerate=False,
        reason="test",
        clamped=False,
    )
    # Battery sets price at bus 10 = 20; downstream bus 20 LMP = 24
    # (typical 20% loss-factor regime).  raw = 24 / 20 = 1.2 (≤ warn).
    _, em_rows = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0},
        zone_results={0: zres},
        dispatch_by_uid={1: 50.0},
        lmp_by_bus={10: 20.0, 20: 24.0},
        srmc_by_uid={},  # no SRMC for batteries; storage path uses LMP
        demand_fail_cost=1000.0,
        tol=Tolerances.default(),
    )
    rows_by_bus = {r.bus_uid: r for r in em_rows}
    # Battery has emission_rate=0 → direct stays 0; loss factor still tracked.
    assert rows_by_bus[20].loss_factor_status == "ok"
    assert rows_by_bus[20].loss_factor_raw == pytest.approx(1.2, rel=1e-9)
    # Storage marginal at its own bus → loss factor = 1.0.
    assert rows_by_bus[10].loss_factor_raw == pytest.approx(1.0, rel=1e-9)


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
        uid=65,
        name="RALCO",
        bus_uid=175,
        pmin=0.0,
        pmax=0.0,
        declared_MC=0.0,
        kind="thermal",
        emission_rate=None,
    )
    palmucho = Generator(
        uid=66,
        name="PALMUCHO",
        bus_uid=175,
        pmin=0.0,
        pmax=0.0,
        declared_MC=0.0,
        kind="thermal",
        emission_rate=None,
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
        uid=900,
        name="IDLE_THERMAL",
        bus_uid=44,
        pmin=0.0,
        pmax=100.0,
        declared_MC=50.0,
        kind="thermal",
        emission_rate=0.5,
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
