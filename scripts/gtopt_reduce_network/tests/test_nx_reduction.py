# SPDX-License-Identifier: BSD-3-Clause
"""Tests for DC-line modelling, reservoir anchors, and louvain-mincut."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import pytest

from gtopt_reduce_network._cluster import select_anchors
from gtopt_reduce_network._io import Case, _index_buses
from gtopt_reduce_network._partition_nx import (
    _split_worst_bottleneck,
    build_busmap_nx,
)
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case
from gtopt_reduce_network._topology import build_admittance, build_line_graph
from gtopt_reduce_network.main import main


def _make_case(raw: dict[str, Any]) -> Case:
    case = Case(raw=raw)
    _index_buses(case)
    return case


@pytest.fixture
def hvdc_tie_case() -> Case:
    """Two 2-bus islands joined only by an X-less (HVDC-style) tie.

    b1 ── b2 ┄┄(no X)┄┄ b3 ── b4
    """
    return _make_case(
        {
            "options": {},
            "simulation": {},
            "system": {
                "bus_array": [{"uid": i, "name": f"b{i}"} for i in range(1, 5)],
                "line_array": [
                    {
                        "uid": 1,
                        "bus_a": "b1",
                        "bus_b": "b2",
                        "reactance": 0.1,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        # HVDC sentinel: no reactance field at all.
                        "uid": 2,
                        "bus_a": "b2",
                        "bus_b": "b3",
                        "tmax_ab": 70,
                        "tmax_ba": 70,
                    },
                    {
                        "uid": 3,
                        "bus_a": "b3",
                        "bus_b": "b4",
                        "reactance": 0.3,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                ],
                "generator_array": [
                    {"uid": 1, "name": "g1", "bus": "b1", "pmax": 200, "capacity": 200},
                ],
                "demand_array": [
                    {"uid": 1, "name": "d4", "bus": "b4", "lmax": 80},
                ],
            },
        }
    )


def test_no_x_lines_are_dc(hvdc_tie_case: Case) -> None:
    graph = build_line_graph(hvdc_tie_case)
    assert graph.n_lines == 3
    assert not graph.skipped_line_uids
    assert graph.dc_line_uids == [2]
    pos = graph.line_uids.index(2)
    assert math.isinf(graph.line_x[pos])  # DC: no reactance is fabricated


def test_type_dc_attribute_forces_dc(hvdc_tie_case: Case) -> None:
    # Even with a positive reactance, type "dc" marks the line DC.
    hvdc_tie_case.array("line_array")[0]["type"] = "dc"
    graph = build_line_graph(hvdc_tie_case)
    assert set(graph.dc_line_uids) == {1, 2}


def test_admittance_excludes_dc(hvdc_tie_case: Case) -> None:
    graph = build_line_graph(hvdc_tie_case)
    bmat = build_admittance(graph).toarray()
    # b2–b3 is the DC tie: zero susceptance coupling.
    assert bmat[1, 2] == pytest.approx(0.0)
    # AC entries present.
    assert bmat[0, 1] == pytest.approx(-1.0 / 0.1)


def test_reduce_keeps_hvdc_corridor_as_dc_line(hvdc_tie_case: Case) -> None:
    config = ReduceConfig(
        target_buses=2, skip_local_simplify=True, partition="louvain-mincut"
    )
    result = reduce_case(hvdc_tie_case, config)
    lines = result.case.array("line_array")
    # The DC tie is never a clustering coupling, so the two AC islands
    # cluster apart and the corridor survives as an explicit DC line:
    # no reactance (gtopt then skips its KVL row), tagged type "dc".
    assert len(lines) == 1
    corridor = lines[0]
    assert corridor["tmax_ab"] == 70
    assert corridor["type"] == "dc"
    assert "reactance" not in corridor


def test_reduce_handles_zero_resistance(hvdc_tie_case: Case) -> None:
    # R = 0 everywhere (explicit and implicit) must not blow up any
    # aggregation formula (AC uses R/X²; DC guards the 1/R combine).
    for ln in hvdc_tie_case.array("line_array"):
        ln["resistance"] = 0
    config = ReduceConfig(
        target_buses=2, skip_local_simplify=True, partition="louvain-mincut"
    )
    result = reduce_case(hvdc_tie_case, config)
    lines = result.case.array("line_array")
    assert len(lines) == 1
    assert lines[0].get("resistance", 0.0) == pytest.approx(0.0)


def test_reservoir_anchor_via_generator() -> None:
    case = _make_case(
        {
            "options": {},
            "simulation": {},
            "system": {
                "bus_array": [{"uid": i, "name": f"b{i}"} for i in range(1, 4)],
                "line_array": [
                    {
                        "uid": 1,
                        "bus_a": "b1",
                        "bus_b": "b2",
                        "reactance": 0.1,
                        "tmax_ab": 10,
                        "tmax_ba": 10,
                    },
                    {
                        "uid": 2,
                        "bus_a": "b2",
                        "bus_b": "b3",
                        "reactance": 0.1,
                        "tmax_ab": 10,
                        "tmax_ba": 10,
                    },
                ],
                "generator_array": [
                    {"uid": 7, "name": "g_res", "bus": "b3", "pmax": 50},
                ],
                # C++ Turbine schema: no bus field; host reached via generator.
                "turbine_array": [
                    {"uid": 1, "name": "t1", "generator": "g_res"},
                ],
            },
        }
    )
    sel = select_anchors(
        case,
        target_buses=2,
        surviving_bus_uids=[1, 2, 3],
        include_reservoir_hosts=True,
    )
    assert 3 in sel.bus_uids
    assert "reservoir-host" in sel.by_rule[3]

    # By uid reference too.
    case.array("turbine_array")[0]["generator"] = 7
    sel = select_anchors(
        case,
        target_buses=2,
        surviving_bus_uids=[1, 2, 3],
        include_reservoir_hosts=True,
    )
    assert "reservoir-host" in sel.by_rule[3]


def test_anchor_trim_respects_target_k(two_clusters_case: Case) -> None:
    # Force 5 rule-derived anchors on a K=2 request: the selection must
    # trim to K by injection magnitude instead of raising downstream.
    sel = select_anchors(
        two_clusters_case,
        target_buses=2,
        surviving_bus_uids=[1, 2, 3, 4, 5, 6],
        min_load_mw=0.0,
        min_gen_capacity_mw=0.0,
        include_reservoir_hosts=True,
    )
    assert len(sel.bus_uids) <= 2
    # Gen buses (500 MW) dominate demand buses (100 MW).
    assert set(sel.bus_uids) == {1, 6}


def test_louvain_mincut_two_clusters(two_clusters_case: Case) -> None:
    graph = build_line_graph(two_clusters_case)
    rows = build_busmap_nx(graph, target_buses=2, anchors=[1, 6])
    cluster_of = {r.original_bus_uid: r.cluster_bus_uid for r in rows}
    assert len(set(cluster_of.values())) == 2
    # The weak 50-MW tie b2–b5 is the corridor: sides stay whole.
    assert cluster_of[1] == cluster_of[2] == cluster_of[3] == 1
    assert cluster_of[4] == cluster_of[5] == cluster_of[6] == 6


def test_louvain_mincut_exact_k(two_clusters_case: Case) -> None:
    graph = build_line_graph(two_clusters_case)
    for k in (2, 3, 4, 5):
        rows = build_busmap_nx(graph, target_buses=k, anchors=[])
        assert len({r.cluster_bus_uid for r in rows}) == k


def test_mincut_split_picks_internal_bottleneck() -> None:
    nx = pytest.importorskip("networkx")
    g = nx.Graph()
    # Community {1,2} hides a weak internal link (10) while both corridors
    # attached to it carry 100 each; community {10,11} is internally strong.
    g.add_edge(1, 2, weight=10.0)
    g.add_edge(1, 10, weight=100.0)
    g.add_edge(2, 11, weight=100.0)
    g.add_edge(10, 11, weight=100.0)
    comms = [{1, 2}, {10, 11}]
    split = _split_worst_bottleneck(g, comms)
    assert split is not None
    assert {1} in split
    assert {2} in split
    assert {10, 11} in split


def test_bus_ratio_cli(tmp_path: Path, two_clusters_case: Case) -> None:
    case_path = tmp_path / "two_clusters.json"
    case_path.write_text(json.dumps(two_clusters_case.raw))
    out = tmp_path / "reduced.json"
    with pytest.raises(SystemExit) as excinfo:
        main(
            [
                "reduce",
                str(case_path),
                "--bus-ratio",
                "0.5",
                "--partition",
                "louvain-mincut",
                "-o",
                str(out),
            ]
        )
    assert excinfo.value.code == 0
    data = json.loads(out.read_text())
    assert len(data["system"]["bus_array"]) == 3  # 6 buses × 0.5


def test_bus_ratio_and_k_are_exclusive(tmp_path: Path, two_clusters_case: Case) -> None:
    case_path = tmp_path / "two_clusters.json"
    case_path.write_text(json.dumps(two_clusters_case.raw))
    with pytest.raises(SystemExit) as excinfo:
        main(["reduce", str(case_path), "-K", "2", "--bus-ratio", "0.5"])
    assert excinfo.value.code == 1
    with pytest.raises(SystemExit) as excinfo:
        main(["reduce", str(case_path)])
    assert excinfo.value.code == 1
