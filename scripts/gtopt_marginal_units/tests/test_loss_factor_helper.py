# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``_resolve_loss_factor`` and ``_topology_aux``.

These cover the loss-factor decision tree in isolation (one helper
call per scenario) so a regression on the dispatch / formula side of
``build_recipes_for_cell`` doesn't mask a regression on the
topology-only side.
"""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import Bus, Generator, Line, Topology

from gtopt_marginal_units._recipes import _resolve_loss_factor, _topology_aux
from gtopt_marginal_units.constants import Tolerances


# ---------------------------------------------------------------------------
# _topology_aux: precompute correctness + identity caching
# ---------------------------------------------------------------------------


def _three_bus_chain() -> Topology:
    """A───B───C  (two bridges; B and C are radial; A is the head)."""
    return Topology(
        buses=[Bus(uid=10, name="A"), Bus(uid=20, name="B"), Bus(uid=30, name="C")],
        generators=[],
        lines=[
            Line(
                uid=1,
                bus_a_uid=10,
                bus_b_uid=20,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=2,
                bus_a_uid=20,
                bus_b_uid=30,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
        ],
    )


def _three_bus_ring() -> Topology:
    """Triangle A─B─C─A (no bridges; no radial buses)."""
    return Topology(
        buses=[Bus(uid=10, name="A"), Bus(uid=20, name="B"), Bus(uid=30, name="C")],
        generators=[],
        lines=[
            Line(
                uid=1,
                bus_a_uid=10,
                bus_b_uid=20,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=2,
                bus_a_uid=20,
                bus_b_uid=30,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=3,
                bus_a_uid=10,
                bus_b_uid=30,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
        ],
    )


def test_topology_aux_radial_chain_marks_all_chain_buses():
    """In an A─B─C chain, every bus's 2-edge-connected component has
    size 1 → all three are radial."""
    aux = _topology_aux(_three_bus_chain())
    assert aux.radial_buses == frozenset({10, 20, 30})


def test_topology_aux_ring_marks_no_radial_buses():
    """In a 3-bus ring, the entire cycle is 2-edge-connected →
    no radial buses."""
    aux = _topology_aux(_three_bus_ring())
    assert aux.radial_buses == frozenset()


def test_topology_aux_disconnected_components_radial_per_island():
    """Two disjoint chains share no edges — each component is its own
    bridge structure; all buses are radial."""
    topo = Topology(
        buses=[
            Bus(uid=10, name="A"),
            Bus(uid=20, name="B"),
            Bus(uid=30, name="C"),
            Bus(uid=40, name="D"),
        ],
        generators=[],
        lines=[
            Line(
                uid=1,
                bus_a_uid=10,
                bus_b_uid=20,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=2,
                bus_a_uid=30,
                bus_b_uid=40,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
        ],
    )
    aux = _topology_aux(topo)
    assert aux.radial_buses == frozenset({10, 20, 30, 40})


def test_topology_aux_inactive_lines_treated_as_open():
    """An ``active=False`` line is dropped from the bridge graph —
    so a 3-bus ring with one inactive edge becomes a chain (all
    three buses become radial)."""
    topo = Topology(
        buses=[Bus(uid=10, name="A"), Bus(uid=20, name="B"), Bus(uid=30, name="C")],
        generators=[],
        lines=[
            Line(
                uid=1,
                bus_a_uid=10,
                bus_b_uid=20,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=2,
                bus_a_uid=20,
                bus_b_uid=30,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=True,
            ),
            Line(
                uid=3,
                bus_a_uid=10,
                bus_b_uid=30,
                tmax_ab=100,
                tmax_ba=100,
                reactance=0.1,
                active=False,
            ),
        ],
    )
    aux = _topology_aux(topo)
    assert aux.radial_buses == frozenset({10, 20, 30})


def test_topology_aux_caches_on_identity():
    """Repeated calls with the same Topology object return the SAME
    aux instance — the bridge analysis only runs once per topology."""
    topo = _three_bus_chain()
    aux1 = _topology_aux(topo)
    aux2 = _topology_aux(topo)
    assert aux1 is aux2  # identity-cached
    # But a freshly-constructed equal topology gets its own aux.
    aux3 = _topology_aux(_three_bus_chain())
    assert aux3 is not aux1


# ---------------------------------------------------------------------------
# _resolve_loss_factor: all status branches
# ---------------------------------------------------------------------------


def _coal_gen(bus_uid: int = 10, kind: str = "thermal", em: float = 0.92) -> Generator:
    return Generator(
        uid=1,
        name="COAL",
        bus_uid=bus_uid,
        pmin=0.0,
        pmax=300.0,
        declared_MC=35.0,
        kind=kind,
        emission_rate=em,
    )


def test_phantom_bus_short_circuits_regardless_of_raw():
    """A phantom-bus name (BAT_*_int_bus) wins over any other classification.
    raw is NOT recorded (synthetic LMP isn't a physical signal)."""
    scale, raw, status = _resolve_loss_factor(
        bus_uid=100,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=100),
        bus_name="BAT_FOO_int_bus",
        gens_on_bus=[],
        lmp_by_bus={100: 999.0},
        srmc_by_uid={1: 35.0},
        zone_of={100: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert (scale, raw, status) == (1.0, 1.0, "phantom_bus")


def test_cross_island_records_raw_but_no_scale():
    """When bus and marginal live in different zones, raw is computed
    against marginal-bus LMP for audit but scale = 1.0."""
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="DownstreamBus",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: 200.0},
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 1},  # different zones
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert status == "cross_island"
    assert scale == pytest.approx(1.0)
    assert raw == pytest.approx(200.0 / 35.0, rel=1e-9)


def test_missing_when_bus_not_in_zone_map():
    scale, raw, status = _resolve_loss_factor(
        bus_uid=999,  # not in zone_of
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="OrphanBus",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 999: 35.0},
        srmc_by_uid={1: 35.0},
        zone_of={10: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert (scale, raw, status) == (1.0, 1.0, "missing")


def test_missing_when_thermal_marginal_has_no_srmc():
    scale, raw, status = _resolve_loss_factor(
        bus_uid=10,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="A",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0},
        srmc_by_uid={},  # empty — no SRMC for the marginal
        zone_of={10: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert (scale, raw, status) == (1.0, 1.0, "missing")


def test_storage_marginal_uses_lmp_at_storage_bus():
    """Storage marginal → ref = LMP[bus(marginal)]; SRMC ignored."""
    bat = Generator(
        uid=1,
        name="BAT",
        bus_uid=10,
        pmin=0,
        pmax=100,
        declared_MC=20.0,
        kind="battery",
        emission_rate=0.0,
    )
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=bat,
        bus_name="DownstreamReal",
        gens_on_bus=[],
        lmp_by_bus={10: 20.0, 20: 24.0},
        srmc_by_uid={},  # empty — storage doesn't need SRMC
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    # raw = 24/20 = 1.2 → within ok envelope
    assert status == "ok"
    assert raw == pytest.approx(1.2, rel=1e-9)
    assert scale == pytest.approx(1.2, rel=1e-9)


def test_negative_raw_skips_scaling():
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="DownstreamReal",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: -5.0},
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert status == "negative"
    assert scale == pytest.approx(1.0)
    assert raw < 0.0


def test_warn_radial_vs_warn_meshed():
    """Same raw value, different topology → different status, same scale."""
    common = dict(
        bus_uid=20,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="DownstreamReal",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: 105.0},  # raw = 3.0
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 0},
        tol=Tolerances.default(),
    )
    sc_r, raw_r, st_r = _resolve_loss_factor(radial_buses=frozenset({20}), **common)
    sc_m, raw_m, st_m = _resolve_loss_factor(radial_buses=frozenset(), **common)
    assert st_r == "warn_radial"
    assert st_m == "warn_meshed"
    # Both apply the raw scaling — only the audit status differs.
    assert sc_r == pytest.approx(3.0, rel=1e-9)
    assert sc_m == pytest.approx(3.0, rel=1e-9)
    assert raw_r == pytest.approx(3.0, rel=1e-9)


def test_critical_radial_vs_critical_meshed():
    """raw > error → critical_*; still applied (no abort)."""
    common = dict(
        bus_uid=20,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="DownstreamReal",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: 350.0},  # raw = 10.0
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 0},
        tol=Tolerances.default(),  # default error=5.0
    )
    sc_r, _raw, st_r = _resolve_loss_factor(radial_buses=frozenset({20}), **common)
    sc_m, _raw, st_m = _resolve_loss_factor(radial_buses=frozenset(), **common)
    assert st_r == "critical_radial"
    assert st_m == "critical_meshed"
    assert sc_r == pytest.approx(10.0, rel=1e-9)
    assert sc_m == pytest.approx(10.0, rel=1e-9)


def test_guard_zero_srmc_hydro_marginal_skips_scaling():
    """A hydro marginal whose own bus has near-zero LMP cannot be the
    real price-setter — raw bus_LMP[i] / LMP[hydro_bus] explodes
    mechanically.  Status 'zero_srmc_hydro', scale=1, no audit raw."""
    hydro = Generator(
        uid=1,
        name="HYDRO",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=0.0,
        kind="hydro",
        emission_rate=0.0,
    )
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=hydro,
        bus_name="DeepSouthBus",
        gens_on_bus=[],
        lmp_by_bus={10: 0.005, 20: 60.0},  # hydro at near-zero LMP
        srmc_by_uid={},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset({20}),
        tol=Tolerances.default(),
    )
    assert status == "zero_srmc_hydro"
    assert scale == pytest.approx(1.0)
    assert raw == pytest.approx(1.0)


def test_guard_no_emission_data_thermal_marginal_skips_scaling():
    """A thermal marginal with emission_rate=None (e.g. cogen biomass)
    has no carbon to scale.  Status 'no_emission_data', scale=1."""
    biomass = Generator(
        uid=1,
        name="ARAUCO",
        bus_uid=10,
        pmin=0.0,
        pmax=100.0,
        declared_MC=35.0,
        kind="thermal",
        emission_rate=None,  # cogen biomass: not stamped
    )
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=biomass,
        bus_name="FarBus",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: 24500.0},  # would-be raw = 700
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert status == "no_emission_data"
    assert scale == pytest.approx(1.0)
    assert raw == pytest.approx(1.0)


def test_guard_zero_srmc_hydro_catches_extreme_raw_above_lmp_floor():
    """Even when LMP[hydro_bus] is above the tol_lmp floor (e.g. 0.4),
    if the resulting raw exceeds loss_factor_error the cell is the
    same LP-tie-break pathology and gets reclassified to
    'zero_srmc_hydro' instead of polluting critical_*."""
    hydro = Generator(
        uid=1,
        name="HYDRO",
        bus_uid=10,
        pmin=0.0,
        pmax=300.0,
        declared_MC=0.0,
        kind="hydro",
        emission_rate=0.0,
    )
    # LMP[hydro_bus] = 0.4 (above default tol_lmp=0.01) → Guard A
    # absolute-LMP test does NOT fire.  But raw = 280/0.4 = 700 →
    # extended guard catches it.
    _, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=hydro,
        bus_name="DeepSouthBus",
        gens_on_bus=[],
        lmp_by_bus={10: 0.4, 20: 280.0},
        srmc_by_uid={},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset({20}),
        tol=Tolerances.default(),
    )
    assert status == "zero_srmc_hydro"
    assert raw == pytest.approx(700.0, rel=1e-9)  # preserved for audit


def test_guard_no_emission_data_with_em_zero_also_fires():
    """A thermal with emission_rate=0.0 (not None) triggers the same
    guard — EF=0 still means no carbon to attribute."""
    bat_gen_wrapper = Generator(
        uid=1,
        name="BAT_X_gen",
        bus_uid=10,
        pmin=0.0,
        pmax=20.0,
        declared_MC=15.0,
        kind="thermal",  # plexos2gtopt tags BAT_*_gen as thermal
        emission_rate=0.0,
    )
    _, _, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=bat_gen_wrapper,
        bus_name="FarBus",
        gens_on_bus=[],
        lmp_by_bus={10: 15.0, 20: 100.0},
        srmc_by_uid={1: 15.0},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset(),
        tol=Tolerances.default(),
    )
    assert status == "no_emission_data"


def test_ok_branch_below_warn_threshold():
    """raw ≤ warn → ok; no badge."""
    scale, raw, status = _resolve_loss_factor(
        bus_uid=20,
        marginal_uid=1,
        g0=_coal_gen(bus_uid=10),
        bus_name="DownstreamReal",
        gens_on_bus=[],
        lmp_by_bus={10: 35.0, 20: 42.0},  # raw = 1.2
        srmc_by_uid={1: 35.0},
        zone_of={10: 0, 20: 0},
        radial_buses=frozenset({20}),
        tol=Tolerances.default(),
    )
    assert status == "ok"
    assert raw == pytest.approx(1.2, rel=1e-9)
    assert scale == pytest.approx(1.2, rel=1e-9)
