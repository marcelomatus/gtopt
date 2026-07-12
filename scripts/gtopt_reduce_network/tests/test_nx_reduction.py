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


def test_dc_reactance_threshold(hvdc_tie_case: Case) -> None:
    # Line 1 gets a tiny reactance below the threshold → DC; line 3 stays AC.
    hvdc_tie_case.array("line_array")[0]["reactance"] = 5e-4
    graph = build_line_graph(hvdc_tie_case, dc_reactance_threshold=1e-3)
    assert set(graph.dc_line_uids) == {1, 2}  # 1 (tiny X) + 2 (no X)
    # threshold=0 restores the old behaviour: only the X-less line 2 is DC.
    g0 = build_line_graph(hvdc_tie_case, dc_reactance_threshold=0.0)
    assert g0.dc_line_uids == [2]


def test_bus_kv_parser() -> None:
    from gtopt_reduce_network._topology import _bus_kv

    assert _bus_kv("Salar110") == 110.0
    assert _bus_kv("Tamaya033") == 33.0  # leading zero
    assert _bus_kv("CNavia220_Aux_D") == 220.0  # embedded SEN level
    assert _bus_kv("b1") is None  # no kV
    assert _bus_kv(None) is None


def test_dc_voltage_threshold_from_bus_names() -> None:
    # kV is derived from the endpoint BUS NAMES, not the line.voltage field.
    case = _make_case(
        {
            "options": {},
            "simulation": {},
            "system": {
                "bus_array": [
                    {"uid": 1, "name": "Sub066"},  # 66 kV
                    {"uid": 2, "name": "Sub066b"},
                    {"uid": 3, "name": "Grid220"},  # 220 kV
                    {"uid": 4, "name": "Grid220b"},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "L1",
                        "bus_a": "Sub066",
                        "bus_b": "Sub066b",
                        "reactance": 0.2,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        "uid": 2,
                        "name": "L2",
                        "bus_a": "Grid220",
                        "bus_b": "Grid220b",
                        "reactance": 0.2,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                ],
            },
        }
    )
    # rule off by default: both AC (X=0.2 > 1e-4, no power hit at tmax 100).
    assert build_line_graph(case, dc_power_threshold=0.0).dc_line_uids == []
    # <=66 kV → the 66 kV line 1 is DC; the 220 kV line 2 stays AC.
    g = build_line_graph(case, dc_voltage_threshold=66.0, dc_power_threshold=0.0)
    assert g.dc_line_uids == [1]


def test_dc_power_threshold_flattens_schedule() -> None:
    # Schedule-valued (nested-list) tmax must flatten to its max, not read 0.
    case = _make_case(
        {
            "options": {},
            "simulation": {},
            "system": {
                "bus_array": [{"uid": i, "name": f"b{i}"} for i in range(1, 4)],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "Big",
                        "bus_a": "b1",
                        "bus_b": "b2",
                        "reactance": 0.2,
                        "tmax_ab": [[300, 500, 400]],
                        "tmax_ba": 500,
                    },
                    {
                        "uid": 2,
                        "name": "Small",
                        "bus_a": "b2",
                        "bus_b": "b3",
                        "reactance": 0.2,
                        "tmax_ab": 15,
                        "tmax_ba": 15,
                    },
                ],
            },
        }
    )
    g = build_line_graph(case, dc_power_threshold=30.0)
    # Big (schedule max 500) stays AC; Small (15 MW) is DC.
    assert g.dc_line_uids == [2]


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


def test_simplify_writes_nested_model_options(two_clusters_case: Case) -> None:
    from gtopt_reduce_network._simplify import apply_loss_mode, apply_transport_only

    case = two_clusters_case.deepcopy()
    case.options["model_options"] = {"use_kirchhoff": True}
    apply_transport_only(case)
    apply_loss_mode(case, "gen-lossfactor", 2.0)
    model = case.options["model_options"]
    # Keys land inside the nested block (gtopt's strict parser rejects
    # them at the top level when model_options exists), never flat.
    assert model["use_kirchhoff"] is False
    assert model["use_line_losses"] is False
    assert model["loss_segments"] == 0
    assert "use_kirchhoff" not in set(case.options) - {"model_options"} or (
        case.options.get("use_kirchhoff") is None
    )
    for gen in case.array("generator_array"):
        assert gen["lossfactor"] == pytest.approx(0.02)
    # Demands untouched under gen-lossfactor.
    assert all("lossfactor" not in d for d in case.array("demand_array"))


def test_simplify_flat_options_fallback(two_clusters_case: Case) -> None:
    from gtopt_reduce_network._simplify import apply_loss_mode, apply_transport_only

    case = two_clusters_case.deepcopy()  # no model_options block
    apply_transport_only(case)
    apply_loss_mode(case, "uplift", 3.0)
    assert case.options["use_kirchhoff"] is False
    assert case.options["use_line_losses"] is False
    for d in case.array("demand_array"):
        assert d["lossfactor"] == pytest.approx(0.03)


def test_filter_pampl_drops_line_constraints() -> None:
    from gtopt_reduce_network._user_constraints import _filter_pampl_text

    text = (
        "# header\n"
        "var slack_TxFoo;\n"
        "\n"
        "# PLEXOS Constraint 'GenBudget'\n"
        "constraint GenBudget:\n"
        '  1 * generator("G1").generation <= 10;\n'
        "\n"
        "# PLEXOS Constraint 'TxFoo'\n"
        "#   soft: slack column 'slack_TxFoo'\n"
        "constraint TxFoo penalty soft_floor_penalty:\n"
        '  1 * line("A->B").flow >= 0;\n'
    )
    filtered, dropped = _filter_pampl_text(text)
    assert dropped == ["TxFoo"]
    assert "GenBudget" in filtered
    assert "TxFoo" not in filtered
    assert "slack_TxFoo" not in filtered
    assert 'generator("G1")' in filtered


def test_protected_lines_survive_with_constraint() -> None:
    from gtopt_reduce_network._protected_lines import collect_protected_lines

    case = _make_case(
        {
            "options": {},
            "simulation": {},
            "system": {
                "bus_array": [{"uid": i, "name": f"b{i}"} for i in range(1, 7)],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "L12",
                        "bus_a": "b1",
                        "bus_b": "b2",
                        "reactance": 0.1,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        "uid": 2,
                        "name": "L23",
                        "bus_a": "b2",
                        "bus_b": "b3",
                        "reactance": 0.1,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        "uid": 3,
                        "name": "L45",
                        "bus_a": "b4",
                        "bus_b": "b5",
                        "reactance": 0.1,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        "uid": 4,
                        "name": "L56",
                        "bus_a": "b5",
                        "bus_b": "b6",
                        "reactance": 0.1,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                    {
                        "uid": 5,
                        "name": "TIE",
                        "bus_a": "b3",
                        "bus_b": "b4",
                        "reactance": 5.0,
                        "tmax_ab": 50,
                        "tmax_ba": 50,
                    },
                ],
                "generator_array": [
                    {"uid": 1, "bus": "b1", "pmax": 500, "capacity": 500},
                    {"uid": 2, "bus": "b6", "pmax": 500, "capacity": 500},
                ],
                "demand_array": [{"uid": 1, "bus": "b4", "lmax": 100}],
                # a Tx constraint referencing the weak tie line
                "user_constraint_array": [
                    {
                        "uid": 1,
                        "name": "TxTie",
                        "expression": '1 * line("TIE").flow <= 40',
                    },
                ],
            },
        }
    )
    puids, pbuses = collect_protected_lines(case)
    assert 5 in puids  # TIE line protected
    assert {case.bus_uid_by_name["b3"], case.bus_uid_by_name["b4"]} <= pbuses

    cfg = ReduceConfig(
        target_buses=3, partition="louvain-mincut", protect_constraint_lines=True
    )
    result = reduce_case(case, cfg)
    red_lines = {ln.get("name") for ln in result.case.array("line_array")}
    assert "TIE" in red_lines  # kept with original identity, not agg_*
    # its endpoints survive as buses
    red_buses = {b.get("name") for b in result.case.array("bus_array")}
    assert {"b3", "b4"} <= red_buses


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
