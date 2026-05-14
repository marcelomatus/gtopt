# SPDX-License-Identifier: BSD-3-Clause
"""Tests for anchor-constrained HAC clustering."""

from __future__ import annotations

import pytest

from gtopt_reduce_network._cluster import build_busmap, select_anchors
from gtopt_reduce_network._io import Case
from gtopt_reduce_network._topology import build_line_graph
from gtopt_reduce_network._distance import reactance_shortest_path_matrix


def test_anchors_split_into_distinct_clusters(two_clusters_case: Case) -> None:
    graph = build_line_graph(two_clusters_case)
    distance = reactance_shortest_path_matrix(graph)
    # Force b1 and b6 as anchors → must end up in different clusters.
    rows = build_busmap(graph.bus_uids, distance, target_buses=2, anchors=[1, 6])
    cluster_of = {r.original_bus_uid: r.cluster_bus_uid for r in rows}
    assert cluster_of[1] != cluster_of[6]
    # Natural cut: b3 sticks with b1, b4 sticks with b6.
    assert cluster_of[3] == cluster_of[1]
    assert cluster_of[4] == cluster_of[6]


def test_target_equal_n_returns_identity(two_clusters_case: Case) -> None:
    graph = build_line_graph(two_clusters_case)
    distance = reactance_shortest_path_matrix(graph)
    rows = build_busmap(
        graph.bus_uids, distance, target_buses=graph.n_buses, anchors=[]
    )
    assert all(r.original_bus_uid == r.cluster_bus_uid for r in rows)


def test_target_below_anchor_count_raises(two_clusters_case: Case) -> None:
    graph = build_line_graph(two_clusters_case)
    distance = reactance_shortest_path_matrix(graph)
    with pytest.raises(ValueError, match="below anchor count"):
        build_busmap(graph.bus_uids, distance, target_buses=1, anchors=[1, 6])


def test_select_anchors_includes_user_list(two_clusters_case: Case) -> None:
    sel = select_anchors(
        two_clusters_case,
        target_buses=4,
        surviving_bus_uids=[1, 2, 3, 4, 5, 6],
        user_anchor_uids=[3],
    )
    assert 3 in sel.bus_uids
    assert "user-list" in sel.by_rule[3]


def test_select_anchors_top_injection(two_clusters_case: Case) -> None:
    sel = select_anchors(
        two_clusters_case,
        target_buses=4,
        surviving_bus_uids=[1, 2, 3, 4, 5, 6],
        top_n_by_injection=2,
    )
    # b1 and b6 carry the only generators (capacity 500 each); they must
    # be among the top-injection anchors.
    assert {1, 6}.issubset(set(sel.bus_uids))


def test_disconnected_components_clustered_separately() -> None:
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": i} for i in (1, 2, 3, 4)],
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
                    "bus_a": 3,
                    "bus_b": 4,
                    "reactance": 0.1,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    graph = build_line_graph(case)
    distance = reactance_shortest_path_matrix(graph)
    rows = build_busmap(graph.bus_uids, distance, target_buses=2, anchors=[])
    cluster_of = {r.original_bus_uid: r.cluster_bus_uid for r in rows}
    assert cluster_of[1] != cluster_of[3]
