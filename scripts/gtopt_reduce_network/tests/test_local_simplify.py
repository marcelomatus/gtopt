# SPDX-License-Identifier: BSD-3-Clause
"""Tests for parallel-merge + degree-2 elimination."""

from __future__ import annotations

import pytest

from gtopt_reduce_network._io import Case
from gtopt_reduce_network._local_simplify import simplify_local
from gtopt_reduce_network._topology import build_line_graph


def test_parallel_merge_two_identical_lines() -> None:
    """Two parallel X=0.2 lines → one equivalent X=0.1, F=110."""
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": 1}, {"uid": 2}],
            "line_array": [
                {
                    "uid": 10,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.2,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
                {
                    "uid": 11,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.2,
                    "tmax_ab": 60,
                    "tmax_ba": 60,
                },
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    graph = build_line_graph(case)
    assert graph.n_lines == 2
    sr = simplify_local(graph, injection_bus_uids=[1, 2])
    assert len(sr.surviving_line_uids) == 1
    assert sr.surviving_x[0] == pytest.approx(0.1)  # parallel of 0.2 || 0.2
    assert sr.surviving_fab[0] == pytest.approx(110.0)
    assert sr.surviving_buses == [1, 2]
    # Both originals must point to the same equivalent line.
    eqs = sorted({r.equivalent_line_uid for r in sr.linemap})
    assert len(eqs) == 1
    assert all(r.rule == "parallel-merge" for r in sr.linemap)


def test_degree2_elimination_no_injection(tiny_chain_case: Case) -> None:
    """b3 has no injection; the parallel pair to b2 is merged first then
    b3 → degree 2 → eliminated; final graph is b1—b2—b4."""
    graph = build_line_graph(tiny_chain_case)
    # Injection set excludes b3 (no gen/demand/battery/turbine on it).
    sr = simplify_local(graph, injection_bus_uids=[1, 4])
    # b2 also has no injection so it is also degree-2 eliminated; the
    # ultimate chain collapses to a single line b1—b4.
    assert 3 in sr.eliminated_buses
    # Number of surviving buses must include 1 and 4.
    assert 1 in sr.surviving_buses
    assert 4 in sr.surviving_buses


def test_injection_bus_protected() -> None:
    """A degree-2 bus with a load is NOT eliminated."""
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": 1}, {"uid": 2}, {"uid": 3}],
            "line_array": [
                {
                    "uid": 10,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.1,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
                {
                    "uid": 11,
                    "bus_a": 2,
                    "bus_b": 3,
                    "reactance": 0.1,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
            ],
            "demand_array": [{"uid": 1, "bus": 2, "lmax": 10}],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    graph = build_line_graph(case)
    sr = simplify_local(graph, injection_bus_uids=[2])
    assert 2 in sr.surviving_buses
    assert len(sr.surviving_line_uids) == 2
