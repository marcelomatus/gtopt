# SPDX-License-Identifier: BSD-3-Clause
"""Zone-partition tests — master §4.3 + §4.7 R1 PTDF builder.

Covers:
* 3-bus chain with mid-line saturated → 2 zones (master spec).
* 3-bus ring all-saturated → 3 zones.
* Single-bus mode → 1 zone.
* Disconnected (islanded) topology → PTDF builds per component
  with no singularity (lp-numerics P0.3 fix).
"""

from __future__ import annotations

import numpy as np
import pytest

from gtopt_canonical_feed import Bus, Line, Topology
from gtopt_marginal_units._zones import (
    build_ptdf,
    estimate_flows,
    partition_zones,
)
from gtopt_marginal_units.errors import InputValidationError


def _chain_topology(n: int = 3) -> Topology:
    """Linear chain b1 — b2 — b3, all lines reactance 0.05, tmax 100."""
    buses = [Bus(uid=i, name=f"b{i}") for i in range(1, n + 1)]
    lines = [
        Line(
            uid=100 + i,
            bus_a_uid=i,
            bus_b_uid=i + 1,
            tmax_ab=100.0,
            tmax_ba=100.0,
            reactance=0.05,
        )
        for i in range(1, n)
    ]
    return Topology(buses=buses, generators=[], lines=lines)


def _ring_topology() -> Topology:
    """3-bus ring."""
    buses = [Bus(uid=i, name=f"b{i}") for i in range(1, 4)]
    lines = [
        Line(
            uid=100, bus_a_uid=1, bus_b_uid=2, tmax_ab=100, tmax_ba=100, reactance=0.05
        ),
        Line(
            uid=101, bus_a_uid=2, bus_b_uid=3, tmax_ab=100, tmax_ba=100, reactance=0.05
        ),
        Line(
            uid=102, bus_a_uid=3, bus_b_uid=1, tmax_ab=100, tmax_ba=100, reactance=0.05
        ),
    ]
    return Topology(buses=buses, generators=[], lines=lines)


# ---------------------------------------------------------------------------
# Connected-components tests
# ---------------------------------------------------------------------------


def test_chain_no_saturation_one_zone():
    topo = _chain_topology(3)
    zone_of = partition_zones(topo)
    assert sorted(set(zone_of.values())) == [0]
    assert all(zone_of[u] == 0 for u in (1, 2, 3))


def test_chain_mid_line_saturated_two_zones():
    topo = _chain_topology(3)
    # Chain has lines 101 (b1-b2) and 102 (b2-b3); drop 102 → b1+b2 share, b3 alone.
    zone_of = partition_zones(topo, saturated_line_uids=[102])
    assert zone_of[1] == zone_of[2]
    assert zone_of[3] != zone_of[1]


def test_ring_all_saturated_three_zones():
    topo = _ring_topology()
    zone_of = partition_zones(topo, saturated_line_uids=[100, 101, 102])
    # Every bus in its own zone.
    assert len(set(zone_of.values())) == 3


def test_single_bus_topology_one_zone():
    topo = Topology(buses=[Bus(uid=1, name="b1")], generators=[], lines=[])
    zone_of = partition_zones(topo)
    assert zone_of == {1: 0}


def test_inactive_lines_split_topology():
    topo = _chain_topology(3)
    topo.lines[1] = Line(
        uid=topo.lines[1].uid,
        bus_a_uid=topo.lines[1].bus_a_uid,
        bus_b_uid=topo.lines[1].bus_b_uid,
        tmax_ab=topo.lines[1].tmax_ab,
        tmax_ba=topo.lines[1].tmax_ba,
        reactance=topo.lines[1].reactance,
        active=False,
    )
    zone_of = partition_zones(topo)
    assert zone_of[1] == zone_of[2]
    assert zone_of[3] != zone_of[1]


# ---------------------------------------------------------------------------
# PTDF tests
# ---------------------------------------------------------------------------


def test_ptdf_chain_shape_and_reference_column_zero():
    topo = _chain_topology(3)
    ptdf, line_uids, bus_uids = build_ptdf(topo)
    assert ptdf.shape == (2, 3)
    # Chain lines uid = 101 (b1-b2) and 102 (b2-b3).
    assert line_uids == [101, 102]
    assert bus_uids == [1, 2, 3]
    # Bus 1 (lowest uid) is the reference; its column should be all-zero.
    np.testing.assert_allclose(ptdf[:, 0], 0.0, atol=1e-12)


def test_ptdf_chain_injection_at_b3_flows_through_both_lines():
    topo = _chain_topology(3)
    # Inject 100 MW at b3, withdraw at b1 (the slack). Power must
    # travel b3 → b2 → b1, i.e. *against* the (a→b) sense of both lines.
    # Line 101 is b1→b2 (so flow against = -100); line 102 is b2→b3
    # (so flow against = -100).
    flows = estimate_flows(topo, bus_net_injection={3: 100.0, 1: -100.0})
    assert flows[101] == pytest.approx(-100.0, abs=1e-9)
    assert flows[102] == pytest.approx(-100.0, abs=1e-9)


def test_ptdf_ring_balanced_injection_splits_evenly():
    topo = _ring_topology()
    # Inject 60 MW at bus 2, withdraw 60 MW at bus 1 (the slack).
    # Direct path (line 100, a=1, b=2): 40 MW flows b2→b1 = -40 on line 100.
    # Indirect path b2→b3→b1: 20 MW flows in the +sense of line 101 (a=2,b=3)
    # AND in the +sense of line 102 (a=3, b=1).
    flows = estimate_flows(topo, bus_net_injection={2: 60.0, 1: -60.0})
    assert flows[100] == pytest.approx(-40.0, abs=1e-9)
    assert flows[101] == pytest.approx(20.0, abs=1e-9)
    assert flows[102] == pytest.approx(20.0, abs=1e-9)


def test_ptdf_islanded_topology_no_singularity():
    """Two disconnected sub-networks — PTDF must still be invertible
    per component (lp-numerics P0.3 fix)."""
    buses = [Bus(uid=i, name=f"b{i}") for i in (1, 2, 10, 11)]
    lines = [
        Line(uid=100, bus_a_uid=1, bus_b_uid=2, tmax_ab=50, tmax_ba=50, reactance=0.05),
        Line(
            uid=200, bus_a_uid=10, bus_b_uid=11, tmax_ab=50, tmax_ba=50, reactance=0.05
        ),
    ]
    topo = Topology(buses=buses, generators=[], lines=lines)
    ptdf, line_uids, bus_uids = build_ptdf(topo)
    assert ptdf.shape == (2, 4)
    # Cross-component PTDF must be zero (an injection in component 1
    # cannot affect line 200 in component 2).
    line_idx = {u: i for i, u in enumerate(line_uids)}
    bus_idx = {u: i for i, u in enumerate(bus_uids)}
    assert ptdf[line_idx[100], bus_idx[10]] == 0.0
    assert ptdf[line_idx[200], bus_idx[1]] == 0.0


def test_ptdf_missing_reactance_raises():
    """Per master §4.7 R1 + lp-numerics finding: missing reactance
    must refuse to run rather than silently fall back to uniform."""
    topo = _chain_topology(3)
    # Strip reactance from one line.
    topo.lines[0] = Line(
        uid=topo.lines[0].uid,
        bus_a_uid=topo.lines[0].bus_a_uid,
        bus_b_uid=topo.lines[0].bus_b_uid,
        tmax_ab=topo.lines[0].tmax_ab,
        tmax_ba=topo.lines[0].tmax_ba,
        reactance=None,
        active=True,
    )
    with pytest.raises(InputValidationError, match="reactance"):
        build_ptdf(topo)


def test_ptdf_non_positive_reactance_raises():
    topo = _chain_topology(3)
    topo.lines[0] = Line(
        uid=topo.lines[0].uid,
        bus_a_uid=topo.lines[0].bus_a_uid,
        bus_b_uid=topo.lines[0].bus_b_uid,
        tmax_ab=topo.lines[0].tmax_ab,
        tmax_ba=topo.lines[0].tmax_ba,
        reactance=-0.05,
        active=True,
    )
    with pytest.raises(InputValidationError, match="reactance"):
        build_ptdf(topo)
