# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for gtopt_check_topology -- analyser + CLI."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from gtopt_check_topology._analyzer import (
    DEFAULT_DANGER_THRESHOLD,
    analyse_planning,
    build_network_graph,
    cycle_danger_score,
)
from gtopt_check_topology._render import (
    report_to_json,
    sos1_candidates_payload,
)
from gtopt_check_topology.main import main


# ---------------------------------------------------------------------------
# Tiny fixture builders
# ---------------------------------------------------------------------------


def _planning(
    buses: list[str],
    lines: list[dict[str, Any]],
    generators: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    return {
        "system": {
            "bus_array": [{"uid": i + 1, "name": name} for i, name in enumerate(buses)],
            "line_array": lines,
            "generator_array": generators or [],
        }
    }


def _line(
    uid: int,
    a: str,
    b: str,
    reactance: float = 0.1,
    tmax: float = 500.0,
    resistance: float = 0.01,
) -> dict[str, Any]:
    return {
        "uid": uid,
        "name": f"{a}->{b}",
        "bus_a": a,
        "bus_b": b,
        "reactance": reactance,
        "resistance": resistance,
        "tmax_ab": tmax,
    }


# ---------------------------------------------------------------------------
# 1. Empty network
# ---------------------------------------------------------------------------


def test_empty_network_is_graceful():
    plan = _planning([], [])
    report = analyse_planning(plan, bundle="empty.json")
    assert report.n_buses == 0
    assert report.n_lines == 0
    assert report.n_cycles == 0
    assert not report.cycles
    assert not report.bridges
    assert not report.islands
    assert not report.sos1_candidates
    payload = report_to_json(report)
    assert payload["n_cycles"] == 0
    assert not payload["dangerous_cycles"]


# ---------------------------------------------------------------------------
# 2. Two-bus radial -- no cycles, no SOS1, no bridges flagged dangerous
# ---------------------------------------------------------------------------


def test_two_bus_radial_no_cycles():
    plan = _planning(
        ["A", "B"],
        [_line(1, "A", "B")],
        generators=[{"uid": 10, "name": "g1", "bus": "A"}],
    )
    report = analyse_planning(plan)
    assert report.n_cycles == 0
    assert not report.dangerous_cycles
    assert not report.sos1_candidates
    # The single line is a stub (both endpoints have degree 1) and also
    # the only bridge.
    assert len(report.bridges) == 1
    assert len(report.stubs) == 1
    assert len(report.islands) == 1
    assert report.islands[0].has_generator is True


# ---------------------------------------------------------------------------
# 3. Equal-impedance triangle -- benign cycle
# ---------------------------------------------------------------------------


def test_triangle_equal_impedance_is_benign():
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=500),
            _line(2, "B", "C", reactance=0.01, tmax=500),
            _line(3, "A", "C", reactance=0.01, tmax=500),
        ],
    )
    report = analyse_planning(plan)
    assert report.n_cycles == 1
    cyc = report.cycles[0]
    assert cyc.asym_x == pytest.approx(1.0)
    assert cyc.asym_tmax == pytest.approx(1.0)
    # min_tmax(500) is NOT < 500, but n_buses(3) <= 4 -> +2; below threshold.
    assert cyc.score < DEFAULT_DANGER_THRESHOLD
    assert not report.dangerous_cycles
    assert not report.sos1_candidates


# ---------------------------------------------------------------------------
# 4. Triangle with 100x reactance asymmetry -- dangerous
# ---------------------------------------------------------------------------


def test_triangle_with_asymmetry_is_dangerous():
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
    )
    report = analyse_planning(plan)
    assert report.n_cycles == 1
    cyc = report.cycles[0]
    assert cyc.asym_x == pytest.approx(100.0)
    assert cyc.score >= DEFAULT_DANGER_THRESHOLD
    assert len(report.dangerous_cycles) == 1
    # All 3 lines should appear in the SOS1 list.
    sos1_uids = {ref.uid for ref in report.sos1_candidates}
    assert sos1_uids == {1, 2, 3}


# ---------------------------------------------------------------------------
# 5. Isolated bus
# ---------------------------------------------------------------------------


def test_isolated_bus_is_flagged():
    plan = _planning(
        ["A", "B", "C"],
        [_line(1, "A", "B")],
    )
    report = analyse_planning(plan)
    assert "C" in report.isolated_buses
    assert len(report.isolated_buses) == 1


# ---------------------------------------------------------------------------
# 6. Multiple islands
# ---------------------------------------------------------------------------


def test_multiple_islands_are_flagged():
    plan = _planning(
        ["A", "B", "C", "D"],
        [
            _line(1, "A", "B"),
            _line(2, "C", "D"),
        ],
        generators=[
            {"uid": 10, "name": "g1", "bus": "A"},
            # No generator on island {C, D}
        ],
    )
    report = analyse_planning(plan)
    assert len(report.islands) == 2
    # The bigger / first one should be the one with the generator, but in
    # this tie (2 vs 2) the sort is by length only -- inspect by content.
    no_gen = report.islands_no_generator
    assert len(no_gen) == 1
    assert set(no_gen[0].buses) == {"C", "D"}


# ---------------------------------------------------------------------------
# 7. DC link -- C1 classification
# ---------------------------------------------------------------------------


def test_dc_link_classified_and_excluded_from_cycle_basis():
    # A-B-C triangle but the A-C edge is an HVDC link (reactance=0).
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.1),
            _line(2, "B", "C", reactance=0.1),
            _line(3, "A", "C", reactance=0.0),  # DC link
        ],
    )
    net = build_network_graph(plan)
    assert len(net.dc_lines) == 1
    assert net.dc_lines[0].uid == 3
    # Only two AC edges -> no cycle.
    assert net.graph.number_of_edges() == 2
    report = analyse_planning(plan)
    assert report.n_cycles == 0
    assert len(report.dc_lines) == 1


# ---------------------------------------------------------------------------
# 8. CLI smoke test -- --json-out + --emit-sos1 produce valid JSON
# ---------------------------------------------------------------------------


def test_cli_smoke_emits_valid_json(tmp_path: Path, capsys):
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
        generators=[{"uid": 10, "name": "g1", "bus": "A"}],
    )
    plan_path = tmp_path / "tiny.json"
    plan_path.write_text(json.dumps(plan))

    json_out = tmp_path / "topo.json"
    sos1_out = tmp_path / "sos1.json"

    rc = main(
        [
            str(plan_path),
            "--json-out",
            str(json_out),
            "--emit-sos1",
            str(sos1_out),
            "--no-color",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "TOPOLOGY ANALYSIS" in out
    assert "Summary" in out

    # JSON files must be well-formed and contain the expected keys.
    payload = json.loads(json_out.read_text())
    assert payload["bundle"] == "tiny.json"
    assert payload["n_buses"] == 3
    assert payload["n_cycles"] == 1
    assert len(payload["dangerous_cycles"]) == 1
    assert len(payload["sos1_candidates"]) == 3

    sos1_payload = json.loads(sos1_out.read_text())
    assert sos1_payload["count"] == 3
    uids = {line["uid"] for line in sos1_payload["lines"]}
    assert uids == {1, 2, 3}


# ---------------------------------------------------------------------------
# Extras -- guard against regressions on tricky data shapes
# ---------------------------------------------------------------------------


def test_tmax_nested_list_is_collapsed_to_max():
    # tmax_ab as a per-block time series (nested list).
    line_with_ts = _line(1, "A", "B", reactance=0.01)
    line_with_ts["tmax_ab"] = [[100.0, 200.0, 300.0]]
    plan = _planning(["A", "B"], [line_with_ts])
    report = analyse_planning(plan)
    # The graph should pick up tmax = 300.
    assert report.stubs and report.stubs[0].tmax == pytest.approx(300.0)


def test_negative_reactance_is_flagged_as_data_error():
    plan = _planning(
        ["A", "B"],
        [_line(1, "A", "B", reactance=-0.1)],
    )
    report = analyse_planning(plan)
    assert len(report.negative_impedance) == 1
    assert report.negative_impedance[0].reactance == pytest.approx(-0.1)


def test_voltage_mixing_flagged_via_bus_name():
    plan = _planning(
        ["BusA110", "BusB220"],
        [_line(1, "BusA110", "BusB220", reactance=0.1)],
    )
    report = analyse_planning(plan)
    assert len(report.voltage_mixing) == 1
    mix = report.voltage_mixing[0]
    assert mix.bus_a_voltage == 110
    assert mix.bus_b_voltage == 220


def test_parallel_circuits_all_included_in_sos1():
    # Two parallel circuits A-B + one A-C, B-C path -> 1 cycle but two
    # parallel circuits along A-B; both UIDs should appear in SOS1.
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "A", "B", reactance=0.02, tmax=200),  # parallel
            _line(3, "B", "C", reactance=0.01, tmax=200),
            _line(4, "A", "C", reactance=1.0, tmax=200),  # high X
        ],
    )
    report = analyse_planning(plan)
    sos1_uids = {ref.uid for ref in report.sos1_candidates}
    # All 4 line UIDs (including the parallel circuit on A-B) must be in
    # the SOS1 set when the cycle is dangerous.
    assert sos1_uids == {1, 2, 3, 4}


def test_danger_score_components():
    # asym_X>=5 (+3), asym_tmax<3 (+0), min_tmax>=500 (+0), n_buses>4 (+0)
    assert cycle_danger_score(5.0, 1.0, 500.0, 5) == 3
    # asym_X>=10 also fires the +3 and +5 tiers
    assert cycle_danger_score(10.0, 1.0, 500.0, 5) == 8
    # asym_X>=20 stacks the third tier
    assert cycle_danger_score(20.0, 1.0, 500.0, 5) == 13
    # min_tmax<200 contributes +2 (the <500 tier) + +3 (the <200 tier)
    assert cycle_danger_score(1.0, 1.0, 100.0, 5) == 5


# ---------------------------------------------------------------------------
# Tier-2 (D1/D2/D3/E1/E2/B6/B7) -- new collectors
# ---------------------------------------------------------------------------


def test_reserve_zone_with_no_provider_is_flagged_d1():
    plan = _planning(["A"], [])
    plan["system"]["reserve_zone_array"] = [
        {"uid": 1, "name": "SR_UP"},
        {"uid": 2, "name": "SR_DN"},
    ]
    plan["system"]["reserve_provision_array"] = [
        {
            "uid": 10,
            "name": "p1",
            "reserve_zones": ["SR_UP"],
            "urmax": 10.0,
        }
    ]
    report = analyse_planning(plan)
    flagged = {iss.zone: iss for iss in report.reserve_zone_coherence}
    assert "SR_DN" in flagged
    assert flagged["SR_DN"].issue == "no providers"
    assert "SR_UP" not in flagged  # one provider with capacity -- OK


def test_reserve_zone_zero_capacity_provider_flagged_d1():
    plan = _planning(["A"], [])
    plan["system"]["reserve_zone_array"] = [{"uid": 1, "name": "Z"}]
    plan["system"]["reserve_provision_array"] = [
        {"uid": 10, "name": "p", "reserve_zones": ["Z"], "urmax": 0.0, "drmax": 0.0}
    ]
    report = analyse_planning(plan)
    assert len(report.reserve_zone_coherence) == 1
    iss = report.reserve_zone_coherence[0]
    assert iss.zone == "Z"
    assert "zero capacity" in iss.issue


def test_island_demand_without_generation_d2():
    # Island {C, D} has demand on D but no generator.
    plan = _planning(
        ["A", "B", "C", "D"],
        [_line(1, "A", "B"), _line(2, "C", "D")],
        generators=[{"uid": 100, "name": "g", "bus": "A", "pmax": 100.0}],
    )
    plan["system"]["demand_array"] = [
        {"uid": 200, "name": "load_D", "bus": "D", "lmax": 50.0}
    ]
    report = analyse_planning(plan)
    issues = report.island_feasibility
    assert any(i.issue == "demand without generation" for i in issues)


def test_battery_eini_zero_with_uc_reference_d3():
    plan = _planning(["A"], [])
    plan["system"]["battery_array"] = [
        {
            "uid": 1,
            "name": "BAT_MANZANO_FV",
            "bus": "A",
            "eini": 0.0,
            "emin": 0.0,
            "emax": 10.0,
        },
        {
            "uid": 2,
            "name": "BAT_OK",
            "bus": "A",
            "eini": 5.0,
            "emin": 0.0,
            "emax": 10.0,
        },
    ]
    plan["system"]["user_constraint_array"] = [
        {
            "uid": 1,
            "name": "UPStorageBound_BAT_MANZANO_FV",
            "expression": (
                '1 * battery("BAT_MANZANO_FV").energy '
                '+ 1 * reserve_provision("p").up <= 0'
            ),
        }
    ]
    report = analyse_planning(plan)
    flagged = {iss.name: iss for iss in report.battery_eini_issues}
    assert "BAT_MANZANO_FV" in flagged
    assert "UPStorageBound_BAT_MANZANO_FV" in flagged["BAT_MANZANO_FV"].in_ucs
    assert "BAT_OK" not in flagged  # eini=5.0 within [0,10] and no UC ref


def test_battery_eini_above_emax_data_error_d3():
    plan = _planning(["A"], [])
    plan["system"]["battery_array"] = [
        {"uid": 1, "name": "BAT_BAD", "bus": "A", "eini": 99.0, "emax": 10.0}
    ]
    report = analyse_planning(plan)
    assert any(
        b.name == "BAT_BAD" and "eini > emax" in b.issue
        for b in report.battery_eini_issues
    )


def test_time_varying_capacity_mostly_zero_e1():
    line = _line(1, "A", "B", reactance=0.01)
    # 6 of 10 blocks zero -> 60% > 50% threshold -> flagged.
    line["tmax_ab"] = [[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 100.0, 100.0, 100.0, 100.0]]
    plan = _planning(["A", "B"], [line])
    report = analyse_planning(plan)
    assert len(report.time_varying_capacity_issues) == 1
    iss = report.time_varying_capacity_issues[0]
    assert iss.blocks_at_zero == 6
    assert iss.blocks_total == 10
    assert iss.max_tmax == pytest.approx(100.0)


def test_reservoir_no_outflow_flagged_e2():
    plan = _planning(["A"], [])
    plan["system"]["reservoir_array"] = [
        {
            "uid": 1,
            "name": "R1",
            "junction": "J1",
            "eini": 100.0,
        }
    ]
    plan["system"]["junction_array"] = [{"uid": 1, "name": "J1"}]
    # No waterway -> J1 has zero out_degree.
    plan["system"]["waterway_array"] = []
    report = analyse_planning(plan)
    assert any("no outflow path" in i.issue for i in report.reservoir_cascade_issues)


def test_reservoir_cascade_cycle_flagged_e2():
    plan = _planning(["A"], [])
    plan["system"]["reservoir_array"] = []
    plan["system"]["junction_array"] = [
        {"uid": 1, "name": "J1"},
        {"uid": 2, "name": "J2"},
    ]
    plan["system"]["waterway_array"] = [
        {"uid": 1, "name": "w_ab", "junction_a": "J1", "junction_b": "J2"},
        {"uid": 2, "name": "w_ba", "junction_a": "J2", "junction_b": "J1"},
    ]
    report = analyse_planning(plan)
    assert any("water-flow cycle" in i.issue for i in report.reservoir_cascade_issues)


def test_parallel_identical_circuits_b6():
    plan = _planning(
        ["A", "B", "C"],
        [
            # 2 identical parallels A-B
            _line(1, "A", "B", reactance=0.02, tmax=1000),
            _line(2, "A", "B", reactance=0.02, tmax=1000),
            # 1 non-parallel
            _line(3, "B", "C", reactance=0.05, tmax=500),
        ],
    )
    report = analyse_planning(plan)
    identical = report.parallel_circuit_identical
    assert len(identical) == 1
    p = identical[0]
    assert {p.bus_a, p.bus_b} == {"A", "B"}
    assert p.n_circuits == 2
    assert p.reactance == pytest.approx(0.02)
    assert p.tmax == pytest.approx(1000.0)


def test_kvl_saturation_threshold_b7():
    # Triangle, X = [0.01, 0.01, 1.0] -> sum=1.02, min=0.01,
    # share = 1 - 0.01/1.02 = 0.9902, threshold = 200 / 0.9902 ~ 202 MW.
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
    )
    report = analyse_planning(plan)
    assert len(report.dangerous_cycles) == 1
    cyc = report.dangerous_cycles[0]
    assert cyc.kvl_saturation_threshold_mw is not None
    expected = 200.0 / (1.0 - 0.01 / 1.02)
    assert cyc.kvl_saturation_threshold_mw == pytest.approx(expected, rel=1e-6)
    # And the JSON serialisation carries it through.
    payload = report_to_json(report)
    assert payload["dangerous_cycles"][0][
        "kvl_saturation_threshold_mw"
    ] == pytest.approx(expected, rel=1e-6)


def test_emit_sos1_alias_warns_but_works(tmp_path: Path):
    """The legacy --emit-sos1 flag still writes the same payload as
    --strict-direction-lines but emits a DeprecationWarning."""
    import warnings as _warnings

    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
        generators=[{"uid": 10, "name": "g1", "bus": "A"}],
    )
    plan_path = tmp_path / "tiny.json"
    plan_path.write_text(json.dumps(plan))

    legacy_out = tmp_path / "legacy.json"
    with _warnings.catch_warnings(record=True) as caught:
        _warnings.simplefilter("always")
        rc = main(
            [
                str(plan_path),
                "--emit-sos1",
                str(legacy_out),
                "--no-color",
                "--quiet",
            ]
        )
    assert rc == 0
    assert any(issubclass(w.category, DeprecationWarning) for w in caught)
    payload = json.loads(legacy_out.read_text())
    assert payload["count"] == 3


def test_strict_direction_lines_flag_writes_payload(tmp_path: Path):
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
        generators=[{"uid": 10, "name": "g1", "bus": "A"}],
    )
    plan_path = tmp_path / "tiny.json"
    plan_path.write_text(json.dumps(plan))

    new_out = tmp_path / "strict.json"
    rc = main(
        [
            str(plan_path),
            "--strict-direction-lines",
            str(new_out),
            "--no-color",
            "--quiet",
        ]
    )
    assert rc == 0
    payload = json.loads(new_out.read_text())
    assert payload["count"] == 3
    uids = {line["uid"] for line in payload["lines"]}
    assert uids == {1, 2, 3}


def test_sos1_payload_schema():
    plan = _planning(
        ["A", "B", "C"],
        [
            _line(1, "A", "B", reactance=0.01, tmax=200),
            _line(2, "B", "C", reactance=0.01, tmax=200),
            _line(3, "A", "C", reactance=1.0, tmax=200),
        ],
    )
    report = analyse_planning(plan, bundle="fixture.json")
    payload = sos1_candidates_payload(report)
    assert payload["bundle"] == "fixture.json"
    assert payload["count"] == 3
    assert payload["danger_threshold"] == DEFAULT_DANGER_THRESHOLD
    assert all(
        set(line.keys()) == {"uid", "name", "bus_a", "bus_b"}
        for line in payload["lines"]
    )
