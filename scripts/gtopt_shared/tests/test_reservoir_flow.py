# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the topology-driven reservoir extraction-flow estimator."""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

import pytest

from gtopt_shared.reservoir_flow import (
    apply_reservoir_flow_estimates,
    compute_turbine_maxflow,
    estimate_reservoir_flow_bounds,
    resolve_inflow_peaks,
    resolve_min_production_factors,
)


def _cascade_system() -> dict[str, Any]:
    """A small 3-reservoir cascade with known capacities.

    Topology (junction names == reservoir names)::

        inflow(100) → R1 ──ww(40)──▶ R2 ──ww(30)──▶ R3 ──turbine(50)──▶ (terminal)
                       │                                  ▲
                       └──turbine(20)──────────────────────┘ (R1→R3 bypass)

    * R1 release: ww 40 + turbine 20 = 60
    * R2 release: ww 30
    * R3 release: turbine 50 (terminal drain)

    Max-flow into each (R not its own source):
    * R1: only its own natural inflow feeds R1; with R1's release edge
      removed there is no other source → 0.
    * R2: fed by R1 (release 60) bottlenecked by ww R1→R2 (40) → 40.
    * R3: fed by R1 via two parallel paths: ww chain R1→R2→R3 (min 40,30 =
      30) + turbine R1→R3 (20), plus R2's own release (30) capped by
      ww R2→R3 (30) — but that 30 is the same edge R1's chain uses.
      Maximum flow R1(60)+R2-as-source... R3's inflow = ww R2→R3 (30) +
      turbine R1→R3 (20) = 50.
    """
    return {
        "junction_array": [
            {"uid": 1, "name": "R1"},
            {"uid": 2, "name": "R2"},
            {"uid": 3, "name": "R3"},
        ],
        "reservoir_array": [
            {"uid": 1, "name": "R1", "junction": "R1"},
            {"uid": 2, "name": "R2", "junction": "R2"},
            {"uid": 3, "name": "R3", "junction": "R3"},
        ],
        "waterway_array": [
            {
                "uid": 1,
                "name": "w12",
                "junction_a": "R1",
                "junction_b": "R2",
                "fmax": 40.0,
            },
            {
                "uid": 2,
                "name": "w23",
                "junction_a": "R2",
                "junction_b": "R3",
                "fmax": 30.0,
            },
        ],
        "generator_array": [
            {"uid": 10, "name": "g_byp", "pmax": 40.0},
            {"uid": 11, "name": "g_term", "pmax": 100.0},
        ],
        "turbine_array": [
            # R1 → R3 bypass turbine: pmax 40 / pf 2 = 20 m³/s
            {
                "uid": 1,
                "name": "t_byp",
                "generator": "g_byp",
                "junction_a": "R1",
                "junction_b": "R3",
                "production_factor": 2.0,
            },
            # R3 terminal turbine: pmax 100 / pf 2 = 50 m³/s (no junction_b)
            {
                "uid": 2,
                "name": "t_term",
                "generator": "g_term",
                "junction_a": "R3",
                "production_factor": 2.0,
            },
        ],
        "flow_array": [
            {"uid": 1, "name": "in_r1", "junction": "R1", "discharge": 100.0},
        ],
    }


def test_turbine_maxflow_division() -> None:
    """Turbine max flow == generator pmax / production_factor."""
    sys = _cascade_system()
    mf = compute_turbine_maxflow(sys)
    assert mf["t_byp"] == pytest.approx(20.0)
    assert mf["t_term"] == pytest.approx(50.0)


def test_turbine_maxflow_capacity_fallback_unresolved_pmax() -> None:
    """When the generator ``pmax`` is an unresolved parquet-ref string,
    the turbine's own resolved ``capacity`` [m³/s] is used so the
    reservoir still gets its full release capacity (the RALCO
    infeasibility fix — gen pmax=='pmax' string, capacity=438)."""
    sys = {
        "turbine_array": [
            {
                "uid": 65,
                "name": "RALCO",
                "generator": "RALCO",
                "junction_a": "RALCO",
                "junction_b": "PANGUE",
                "production_factor": 1.575,
                "capacity": 438.095,
            }
        ],
        "generator_array": [{"uid": 65, "name": "RALCO", "pmax": "pmax"}],
    }
    mf = compute_turbine_maxflow(sys)
    # pmax unresolved -> fall back to capacity directly (no /pf).
    assert mf["RALCO"] == pytest.approx(438.095)


def test_inflow_peaks_by_uid_overrides_file_read() -> None:
    """A caller-supplied {flow_uid: peak} (built from the ORIGINAL PLP
    *.dat, e.g. plpaflce) is used for string-ref discharge instead of
    re-reading the emitted output — so the estimate needs no input_dir
    (no read of plp2gtopt's own output = gtopt's input)."""
    system = {
        "junction_array": [{"uid": 1, "name": "J1"}],
        "flow_array": [
            {"uid": 10, "name": "F1", "junction": "J1", "discharge": "discharge"},
        ],
    }
    # No input_dir, but peak supplied in-memory -> resolves.
    peaks = resolve_inflow_peaks(
        system, input_dir=None, inflow_peaks_by_uid={10: 382.2}
    )
    assert peaks["J1"] == pytest.approx(382.2)
    # Without the supplied map and no input_dir -> 0 (would warn).
    empty = resolve_inflow_peaks(system, input_dir=None)
    assert "J1" not in empty


def test_locate_and_read_table_csv_fallback(tmp_path) -> None:
    """The shared table reader resolves peaks from CSV (-F csv output),
    not just parquet — otherwise string-ref pmax/fmax never resolve and
    extraction bounds are left far too tight."""
    from gtopt_shared.reservoir_flow import _locate_and_read_table

    gdir = tmp_path / "Generator"
    gdir.mkdir()
    (gdir / "pmax.csv").write_text(
        "block,stage,uid,value\n1,1,37,400.0\n2,1,37,437.3\n1,1,65,900.0\n",
        encoding="utf-8",
    )
    peaks = _locate_and_read_table("pmax", tmp_path, subdir="Generator")
    assert peaks == {37: pytest.approx(437.3), 65: pytest.approx(900.0)}


def test_turbine_maxflow_resolves_time_series_pmax_and_pf() -> None:
    """Inline time-series ``pmax``/``production_factor`` resolve to the
    conservative outer bound: peak pmax / minimum positive pf (not dropped).

    Regression guard: a derated/DLR generator (``pmax`` as a nested
    schedule) must still contribute its peak capacity to a reservoir's
    ``max_release`` — otherwise cascade reservoirs silently fall back to
    the generic C++ default."""
    sys: dict[str, Any] = {
        "generator_array": [
            # peak pmax = 200, time-varying (e.g. derate window of 100)
            {"uid": 1, "name": "G", "pmax": [[[100.0, 200.0, 150.0]]]},
        ],
        "turbine_array": [
            # time-varying pf; min positive = 2.0 ⇒ max flow = 200 / 2 = 100
            {
                "uid": 1,
                "name": "T",
                "generator": "G",
                "junction_a": "J",
                "production_factor": [[[4.0, 2.0, 3.0]]],
            },
        ],
    }
    mf = compute_turbine_maxflow(sys)
    assert mf["T"] == pytest.approx(100.0)


def test_resolve_inflow_peaks_scalar() -> None:
    """Scalar discharge resolves to its value, keyed by junction."""
    sys = _cascade_system()
    peaks = resolve_inflow_peaks(sys, input_dir=None)
    assert peaks == {"R1": pytest.approx(100.0)}


def test_resolve_inflow_peaks_inline_array() -> None:
    """Inline nested-array discharge resolves to its max leaf."""
    sys = _cascade_system()
    sys["flow_array"] = [
        {
            "uid": 1,
            "name": "in_r1",
            "junction": "R1",
            "discharge": [[[10.0, 33.0], [5.0, 12.0]]],
        },
    ]
    peaks = resolve_inflow_peaks(sys, input_dir=None)
    assert peaks["R1"] == pytest.approx(33.0)


def test_resolve_inflow_peaks_summed_per_junction() -> None:
    """Multiple flows at the same junction sum their peaks."""
    sys = _cascade_system()
    sys["flow_array"] = [
        {"uid": 1, "name": "a", "junction": "R1", "discharge": 40.0},
        {"uid": 2, "name": "b", "junction": "R1", "discharge": 60.0},
    ]
    peaks = resolve_inflow_peaks(sys, input_dir=None)
    assert peaks["R1"] == pytest.approx(100.0)


def test_max_release_is_sum_of_downstream_caps() -> None:
    """max_release == Σ downstream waterway + turbine caps (→ fmax)."""
    sys = _cascade_system()
    report = estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R1 release = ww 40 + turbine 20 = 60 → fmax = round(60 * 1.1) = 66
    assert res["R1"]["fmax"] == pytest.approx(66.0)
    # R2 release = ww 30 → 33
    assert res["R2"]["fmax"] == pytest.approx(33.0)
    # R3 release = terminal turbine 50 → 55
    assert res["R3"]["fmax"] == pytest.approx(55.0)
    # report mirrors the applied fmax
    assert report["R1"][1] == pytest.approx(66.0)


def test_max_inflow_is_maxflow_bottleneck() -> None:
    """max_inflow == network-bottlenecked max-flow into the junction."""
    sys = _cascade_system()
    estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R1: its own release edge is removed, but it STILL receives its own
    # natural inflow (100) → max_inflow 100 → fmin = -round(100*1.1) = -110.
    assert res["R1"]["fmin"] == pytest.approx(-110.0)
    # R2: fed by R1 (natural inflow + release) bottlenecked by ww R1→R2 (40)
    #     → 40 → -44
    assert res["R2"]["fmin"] == pytest.approx(-44.0)
    # R3: ww R2→R3 (30) + turbine R1→R3 (20) = 50 → -55
    assert res["R3"]["fmin"] == pytest.approx(-55.0)


def test_explicit_bounds_not_overridden() -> None:
    """Finite user-provided fmin/fmax are never replaced."""
    sys = _cascade_system()
    sys["reservoir_array"][1]["fmin"] = -7.0
    sys["reservoir_array"][1]["fmax"] = 13.0
    estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    assert sys["reservoir_array"][1]["fmin"] == pytest.approx(-7.0)
    assert sys["reservoir_array"][1]["fmax"] == pytest.approx(13.0)


def test_sentinel_infinity_is_replaced() -> None:
    """The ±1e30 effective-infinity sentinel counts as 'absent'."""
    sys = _cascade_system()
    sys["reservoir_array"][0]["fmin"] = -1.0e30
    sys["reservoir_array"][0]["fmax"] = 1.0e30
    estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    # R1 had sentinel fmax → replaced by the topology estimate (66).
    assert sys["reservoir_array"][0]["fmax"] == pytest.approx(66.0)


def test_missing_parquet_ref_degrades_to_zero(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """A string parquet ref that can't be resolved → peak 0 + a warning."""
    sys = _cascade_system()
    sys["flow_array"] = [
        {"uid": 1, "name": "in_r1", "junction": "R1", "discharge": "discharge"},
    ]
    with caplog.at_level(logging.WARNING):
        peaks = resolve_inflow_peaks(sys, input_dir=Path("/nonexistent/dir"))
    assert peaks.get("R1", 0.0) == pytest.approx(0.0)
    assert any("parquet" in rec.message.lower() for rec in caplog.records)


def test_resolve_inflow_peaks_long_parquet(tmp_path: Path) -> None:
    """A string discharge ref resolves from a long-layout parquet table.

    Mirrors what plp2gtopt writes: ``<input_dir>/Flow/discharge.parquet``
    in long form (columns ``scenario, stage, block, uid, value``) keyed by
    the flow uid.
    """
    pa = pytest.importorskip("pyarrow")
    pq = pytest.importorskip("pyarrow.parquet")

    flow_dir = tmp_path / "Flow"
    flow_dir.mkdir(parents=True)
    table = pa.table(
        {
            "scenario": [0, 0, 0, 0],
            "stage": [0, 0, 1, 1],
            "block": [0, 1, 0, 1],
            "uid": [1, 1, 1, 1],
            "value": [10.0, 80.0, 25.0, 40.0],
        }
    )
    pq.write_table(table, flow_dir / "discharge.parquet")

    sys = _cascade_system()
    sys["flow_array"] = [
        {"uid": 1, "name": "in_r1", "junction": "R1", "discharge": "discharge"},
    ]
    peaks = resolve_inflow_peaks(sys, input_dir=tmp_path)
    # Peak over the flow-uid=1 column is 80.0.
    assert peaks["R1"] == pytest.approx(80.0)


def test_waterway_junctions_by_uid() -> None:
    """References resolve by integer uid as well as by name."""
    sys = _cascade_system()
    # Re-key the R1 waterway to reference junctions by uid.
    sys["waterway_array"][0]["junction_a"] = 1  # R1
    sys["waterway_array"][0]["junction_b"] = 2  # R2
    report = estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    # R1 release still 60 (ww 40 + turbine 20) → fmax 66.
    assert report["R1"][1] == pytest.approx(66.0)


def test_turbine_waterway_link_mode() -> None:
    """A turbine referencing a waterway inherits that waterway's junctions."""
    sys = _cascade_system()
    # Add a link-mode turbine on an existing waterway w23 (R2→R3).
    sys["waterway_array"].append(
        {"uid": 5, "name": "w_link", "junction_a": "R2", "junction_b": "R3"}
    )
    sys["generator_array"].append({"uid": 20, "name": "g_link", "pmax": 80.0})
    sys["turbine_array"].append(
        {
            "uid": 9,
            "name": "t_link",
            "generator": "g_link",
            "waterway": "w_link",
            "production_factor": 4.0,
        }
    )
    mf = compute_turbine_maxflow(sys)
    assert mf["t_link"] == pytest.approx(20.0)  # 80 / 4
    report = estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=mf,
    )
    # R2 release now ww 30 + link-turbine 20 = 50 → fmax round(50*1.1)=55.
    res = {r["name"]: r for r in sys["reservoir_array"]}
    assert res["R2"]["fmax"] == pytest.approx(55.0)
    assert report["R2"][1] == pytest.approx(55.0)


@pytest.mark.parametrize(
    "discharge,expected",
    [
        (12.5, 12.5),
        ([[[1.0, 9.0, 4.0]]], 9.0),
        ([1.0, 2.0, 3.0], 3.0),
    ],
)
def test_apply_with_inline_inflows_sets_bounds(discharge: Any, expected: float) -> None:
    """End-to-end apply on inline-array inflows sets reservoir fmin/fmax."""
    sys = _cascade_system()
    sys["flow_array"] = [
        {"uid": 1, "name": "in_r1", "junction": "R1", "discharge": discharge},
    ]
    planning = {"system": sys}
    report = apply_reservoir_flow_estimates(planning, input_dir=None)
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # fmax (release side) is independent of the inflow magnitude.
    assert res["R1"]["fmax"] == pytest.approx(66.0)
    # The peak inflow is recorded but only bounds the accept side via
    # max-flow; the release-side fmax assertion is the stable invariant.
    assert report["R1"][1] == pytest.approx(66.0)
    # Peak resolution honoured the schedule form.
    peaks = resolve_inflow_peaks(sys, input_dir=None)
    assert peaks["R1"] == pytest.approx(expected)


def test_apply_no_reservoirs_is_noop() -> None:
    """apply on a system without reservoirs returns an empty report."""
    assert not apply_reservoir_flow_estimates({"system": {}}, input_dir=None)
    assert not apply_reservoir_flow_estimates({}, input_dir=None)


def test_generator_pmax_parquet_ref_resolved(tmp_path: Path) -> None:
    """A turbine whose generator ``pmax`` is a parquet-ref string resolves.

    Mirrors plp2gtopt: generator ``pmax`` == the string ``"pmax"``, a
    reference to ``<input_dir>/Generator/pmax.parquet`` (long layout keyed
    by generator uid).  The resolved peak feeds the turbine max-flow and
    hence the reservoir's release-side ``fmax``.
    """
    pa = pytest.importorskip("pyarrow")
    pq = pytest.importorskip("pyarrow.parquet")

    gen_dir = tmp_path / "Generator"
    gen_dir.mkdir(parents=True)
    # gen uid 11 (terminal turbine) peak pmax = 120 (time-varying).
    table = pa.table(
        {
            "stage": [0, 0, 1, 1],
            "block": [0, 1, 0, 1],
            "uid": [11, 11, 11, 11],
            "value": [80.0, 120.0, 100.0, 60.0],
        }
    )
    pq.write_table(table, gen_dir / "pmax.parquet")

    sys = _cascade_system()
    # Drop g_term's inline pmax; reference the parquet table by name.
    for g in sys["generator_array"]:
        if g["name"] == "g_term":
            g["pmax"] = "pmax"
    planning = {"system": sys}
    apply_reservoir_flow_estimates(planning, input_dir=tmp_path)
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R3 terminal turbine: pmax peak 120 / pf 2 = 60 → fmax round(60*1.1)=66.
    assert res["R3"]["fmax"] == pytest.approx(66.0)


def test_waterway_fmax_parquet_ref_resolved(tmp_path: Path) -> None:
    """A waterway whose ``fmax`` is a parquet-ref string resolves.

    Mirrors plp2gtopt: waterway ``fmax`` == the string ``"fmax"``, a
    reference to ``<input_dir>/Waterway/fmax.parquet`` (long layout keyed
    by waterway uid).
    """
    pa = pytest.importorskip("pyarrow")
    pq = pytest.importorskip("pyarrow.parquet")

    ww_dir = tmp_path / "Waterway"
    ww_dir.mkdir(parents=True)
    # ww uid 1 (R1→R2) peak fmax = 45 (time-varying).
    table = pa.table(
        {
            "stage": [0, 0, 1, 1],
            "block": [0, 1, 0, 1],
            "uid": [1, 1, 1, 1],
            "value": [20.0, 45.0, 30.0, 10.0],
        }
    )
    pq.write_table(table, ww_dir / "fmax.parquet")

    sys = _cascade_system()
    # Replace the R1→R2 waterway inline fmax with a parquet-ref string.
    sys["waterway_array"][0]["fmax"] = "fmax"
    planning = {"system": sys}
    apply_reservoir_flow_estimates(planning, input_dir=tmp_path)
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R1 release = ww peak 45 + turbine 20 = 65 → fmax round(65*1.1)=72 (71.5→72).
    assert res["R1"]["fmax"] == pytest.approx(72.0)
    # And the resolved ww edge feeds R2's accept side: bottleneck now 45.
    assert res["R2"]["fmin"] == pytest.approx(-round(45.0 * 1.1))


def test_reservoir_without_spillway_gets_conduit_only_fmax() -> None:
    """A reservoir with NO ``spillway_cost`` (the un-drained ELTORO
    sentinel) is limited to its conduit release — ``fmax = conduit`` only,
    no spill-flood term."""
    sys = _cascade_system()  # no reservoir has a spillway_cost
    estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R3 conduit = terminal turbine 50 → round(50*1.1) = 55 (no spill term).
    assert res["R3"]["fmax"] == pytest.approx(55.0)
    assert res["R1"]["fmax"] == pytest.approx(66.0)  # ww 40 + turbine 20 = 60


def test_max_extraction_is_physical_not_affluent() -> None:
    """MAX EXTRACTION (``fmax``) is a PHYSICAL release limit (conduit) and
    must NOT depend on inflow.  Enabling the drain on every reservoir AND
    10×-ing the inflows must leave every ``fmax`` unchanged — the spillway
    is a separate LP column, and inflow is affluent data, not a release
    capacity."""
    base = _cascade_system()
    estimate_reservoir_flow_bounds(
        base,
        inflow_peaks=resolve_inflow_peaks(base, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(base),
    )
    fmax_base = {r["name"]: r["fmax"] for r in base["reservoir_array"]}
    assert fmax_base["R3"] == pytest.approx(55.0)  # terminal turbine 50 ×1.1

    sys = _cascade_system()
    for r in sys["reservoir_array"]:
        r["spillway_cost"] = 0.0  # drain ON everywhere
    inflows = {
        k: v * 10.0 for k, v in resolve_inflow_peaks(sys, input_dir=None).items()
    }
    estimate_reservoir_flow_bounds(
        sys, inflow_peaks=inflows, turbine_maxflow=compute_turbine_maxflow(sys)
    )
    for r in sys["reservoir_array"]:
        assert r["fmax"] == pytest.approx(fmax_base[r["name"]])


def test_drained_upstream_reservoir_does_not_inflate_downstream_fmin() -> None:
    """A drained UPSTREAM reservoir spills OUT of the basin, so it must NOT
    inflate a downstream reservoir's ``fmin`` — the cascade feed uses the
    FINITE conduit-out capacity, never the drain."""
    sys = _cascade_system()
    for r in sys["reservoir_array"]:
        if r["name"] == "R1":
            r["spillway_cost"] = 0.0
    estimate_reservoir_flow_bounds(
        sys,
        inflow_peaks=resolve_inflow_peaks(sys, input_dir=None),
        turbine_maxflow=compute_turbine_maxflow(sys),
    )
    res = {r["name"]: r for r in sys["reservoir_array"]}
    # R2 fed by R1's FINITE conduit-out, bottlenecked by ww R1→R2 (40) → -44.
    assert res["R2"]["fmin"] == pytest.approx(-44.0)
    assert res["R3"]["fmin"] == pytest.approx(-55.0)


def test_resolve_min_production_factors_endpoint_min() -> None:
    """min_PF == min(PF(emin), PF(emax)) over the concave envelope.

    Two segments, one with a NEGATIVE slope so the minimum production
    factor is attained at ``emax`` (not ``emin``) — this confirms the
    endpoint-min logic actually evaluates both ends.

    Reservoir volume range ``[emin=10, emax=110]``.  Segments (point-slope
    form ``PF = constant + slope * (V - volume)``)::

        seg A: volume=0,   slope=+0.05, constant=4.0
        seg B: volume=100, slope=-0.02, constant=3.0

    PF(V) = min(4.0 + 0.05*(V-0), 3.0 - 0.02*(V-100)).
    * PF(10)  = min(4.0+0.5,  3.0+1.8) = min(4.5, 4.8) = 4.5
    * PF(110) = min(4.0+5.5,  3.0-0.2) = min(9.5, 2.8) = 2.8
    min_PF = min(4.5, 2.8) = 2.8  (attained at emax).
    """
    sys: dict[str, Any] = {
        "reservoir_array": [
            {"uid": 7, "name": "RX", "emin": 10.0, "emax": 110.0},
        ],
        "reservoir_production_factor_array": [
            {
                "uid": 1,
                "name": "RX_pfac",
                "turbine": "TX",
                "reservoir": "RX",
                "mean_production_factor": 4.0,
                "segments": [
                    {"volume": 0.0, "slope": 0.05, "constant": 4.0},
                    {"volume": 100.0, "slope": -0.02, "constant": 3.0},
                ],
            },
        ],
    }
    pfs = resolve_min_production_factors(sys)
    assert pfs["TX"] == pytest.approx(2.8)


def test_compute_turbine_maxflow_uses_min_production_factor() -> None:
    """compute_turbine_maxflow divides pmax by the resolved min_PF.

    A turbine WITH a reservoir_production_factor uses min_PF; a turbine
    WITHOUT one falls back to its scalar ``production_factor``.
    """
    sys: dict[str, Any] = {
        "generator_array": [
            {"uid": 1, "name": "gx", "pmax": 56.0},
            {"uid": 2, "name": "gy", "pmax": 40.0},
        ],
        "turbine_array": [
            # TX: scalar pf 4.8 but min_PF 2.8 ⇒ 56 / 2.8 = 20.0
            {
                "uid": 1,
                "name": "TX",
                "generator": "gx",
                "junction_a": "J",
                "production_factor": 4.8,
            },
            # TY: no reservoir_production_factor ⇒ scalar pf 2.0 ⇒ 40/2 = 20
            {
                "uid": 2,
                "name": "TY",
                "generator": "gy",
                "junction_a": "J",
                "production_factor": 2.0,
            },
        ],
    }
    min_pfs = {"TX": 2.8}
    mf = compute_turbine_maxflow(sys, min_production_factors=min_pfs)
    assert mf["TX"] == pytest.approx(20.0)  # 56 / 2.8 (min_PF, not 4.8)
    assert mf["TY"] == pytest.approx(20.0)  # 40 / 2.0 (scalar fallback)
    # Without the map, TX falls back to its scalar pf 4.8.
    mf_scalar = compute_turbine_maxflow(sys)
    assert mf_scalar["TX"] == pytest.approx(56.0 / 4.8)


def test_resolve_min_production_factors_keys_name_and_uid() -> None:
    """The resolver keys the turbine ref by both name and str(uid)."""
    sys: dict[str, Any] = {
        "reservoir_array": [{"uid": 7, "name": "RX", "emax": 100.0}],
        "reservoir_production_factor_array": [
            {
                "uid": 1,
                "name": "RX_pfac",
                "turbine": 42,  # int uid reference
                "reservoir": 7,  # int uid reference
                "segments": [{"volume": 0.0, "slope": 0.0, "constant": 5.0}],
            },
        ],
    }
    pfs = resolve_min_production_factors(sys)
    assert pfs["42"] == pytest.approx(5.0)


def test_resolve_min_production_factors_segment_fallback() -> None:
    """Empty segments fall back to mean_production_factor."""
    sys: dict[str, Any] = {
        "reservoir_array": [{"uid": 7, "name": "RX", "emax": 100.0}],
        "reservoir_production_factor_array": [
            {
                "uid": 1,
                "name": "RX_pfac",
                "turbine": "TX",
                "reservoir": "RX",
                "mean_production_factor": 3.3,
                "segments": [],
            },
        ],
    }
    pfs = resolve_min_production_factors(sys)
    assert pfs["TX"] == pytest.approx(3.3)


def test_resolve_min_production_factors_multi_element_takes_min() -> None:
    """Multiple elements for one turbine → smallest min_PF wins."""
    sys: dict[str, Any] = {
        "reservoir_array": [{"uid": 7, "name": "RX", "emax": 100.0}],
        "reservoir_production_factor_array": [
            {
                "uid": 1,
                "name": "a",
                "turbine": "TX",
                "reservoir": "RX",
                "segments": [{"volume": 0.0, "slope": 0.0, "constant": 5.0}],
            },
            {
                "uid": 2,
                "name": "b",
                "turbine": "TX",
                "reservoir": "RX",
                "segments": [{"volume": 0.0, "slope": 0.0, "constant": 3.0}],
            },
        ],
    }
    pfs = resolve_min_production_factors(sys)
    assert pfs["TX"] == pytest.approx(3.0)


def test_outflow_direction_skipped() -> None:
    """A flow flagged as an outflow draw does not contribute to inflow."""
    sys = _cascade_system()
    sys["flow_array"] = [
        {
            "uid": 1,
            "name": "draw",
            "junction": "R1",
            "discharge": 50.0,
            "direction": "out",
        },
    ]
    peaks = resolve_inflow_peaks(sys, input_dir=None)
    assert peaks.get("R1", 0.0) == pytest.approx(0.0)


def test_piecewise_seepage_intercept_form_and_segment_selection() -> None:
    """Seepage = constant_i + slope_i·V, segment chosen by greatest
    breakpoint ≤ V; segments are continuous at the breakpoints."""
    from gtopt_shared.reservoir_flow import _piecewise_seepage  # noqa: PLC0415

    segs = [
        {"volume": 0.0, "slope": 0.043150359, "constant": 0.0},
        {"volume": 400.0, "slope": 0.005429197, "constant": 15.08846449},
        {"volume": 2700.0, "slope": 0.007149312, "constant": 10.44415451},
    ]
    # continuity at the 400 breakpoint (both segments agree)
    assert _piecewise_seepage(segs, 400.0) == pytest.approx(17.26, abs=1e-2)
    # extrapolated above the last breakpoint (Lago Laja at full pool)
    assert _piecewise_seepage(segs, 5585.888) == pytest.approx(50.38, abs=1e-2)
    # below the first breakpoint clamps to the first segment
    assert _piecewise_seepage(segs, 100.0) == pytest.approx(4.315, abs=1e-3)


def test_resolve_reservoir_seepage_caps_keys_uid_and_name() -> None:
    """A ``reservoir_seepage`` element resolves its ``filt_*`` waterway cap
    (evaluated at emax when numeric) keyed by BOTH uid and name."""
    from gtopt_shared.reservoir_flow import (  # noqa: PLC0415
        resolve_reservoir_seepage_caps,
    )

    system = {
        "reservoir_array": [{"name": "ELTORO", "junction": "ELTORO", "emax": 2700.0}],
        "waterway_array": [
            {
                "uid": 7,
                "name": "filt_ELTORO",
                "junction_a": "ELTORO",
                "junction_b": "D",
            },
        ],
        "reservoir_seepage_array": [
            {
                "name": "s1",
                "reservoir": "ELTORO",
                "waterway": "filt_ELTORO",
                "segments": [
                    {"volume": 0.0, "slope": 0.01, "constant": 0.0},
                    {"volume": 2700.0, "slope": 0.0, "constant": 27.0},
                ],
            }
        ],
    }
    caps = resolve_reservoir_seepage_caps(system)
    assert caps[7] == pytest.approx(27.0)
    assert caps["filt_ELTORO"] == pytest.approx(27.0)


def test_seepage_cap_feeds_conduit_and_fmax() -> None:
    """A ``filt_*`` waterway with no numeric fmax contributes its seepage
    rate to the reservoir's outflow conduit → larger ``fmax``."""
    from gtopt_shared.reservoir_flow import (  # noqa: PLC0415
        resolve_reservoir_seepage_caps,
    )

    system = {
        "reservoir_array": [{"name": "R", "junction": "RJ", "emax": 100.0}],
        "waterway_array": [
            # primary release conduit (numeric)
            {
                "uid": 1,
                "name": "rel",
                "junction_a": "RJ",
                "junction_b": "X",
                "fmax": 40.0,
            },
            # seepage waterway (no numeric fmax → cap from the seepage element)
            {"uid": 2, "name": "filt_R", "junction_a": "RJ", "junction_b": "X"},
        ],
        "junction_array": [{"name": "RJ"}, {"name": "X"}],
        "turbine_array": [],
        "reservoir_seepage_array": [
            {
                "name": "s",
                "reservoir": "R",
                "waterway": "filt_R",
                "segments": [{"volume": 0.0, "slope": 0.0, "constant": 10.0}],
            }
        ],
    }
    seepage_caps = resolve_reservoir_seepage_caps(system)
    estimate_reservoir_flow_bounds(
        system,
        inflow_peaks={},
        turbine_maxflow={},
        seepage_caps=seepage_caps,
    )
    res = system["reservoir_array"][0]
    # conduit = release 40 + seepage 10 = 50; no drain → fmax = round(50*1.1).
    assert res["fmax"] == pytest.approx(55.0)


def test_generator_capacity_override_uses_nameplate() -> None:
    """A maintenance-offline unit (dispatch pmax=0) still contributes its
    NAMEPLATE to the turbine maxflow when an override is supplied — while
    the JSON dispatch pmax stays untouched."""
    system = {
        "generator_array": [
            {"uid": 1, "name": "COLBUN_U1", "pmax": 240.0},
            {"uid": 2, "name": "COLBUN_U2", "pmax": 0.0},  # offline this week
        ],
        "turbine_array": [
            {
                "uid": 10,
                "name": "t_u1",
                "generator": "COLBUN_U1",
                "production_factor": 2.0,
            },
            {
                "uid": 11,
                "name": "t_u2",
                "generator": "COLBUN_U2",
                "production_factor": 2.0,
            },
        ],
    }
    # Without override: offline unit contributes 0.
    mf = compute_turbine_maxflow(system)
    assert mf["t_u1"] == pytest.approx(120.0)  # 240 / 2
    assert "t_u2" not in mf  # pmax 0 → skipped

    # With nameplate override (by name): both units contribute.
    mf2 = compute_turbine_maxflow(
        system, generator_capacities={"COLBUN_U1": 240.0, "COLBUN_U2": 240.0}
    )
    assert mf2["t_u1"] == pytest.approx(120.0)
    assert mf2["t_u2"] == pytest.approx(120.0)  # nameplate 240 / 2, not 0
    # Dispatch pmax in the JSON is untouched.
    assert system["generator_array"][1]["pmax"] == pytest.approx(0.0)


def test_extra_turbines_count_offline_units_for_bound() -> None:
    """A maintenance-offline unit that PLEXOS drops from ``turbine_array``
    can be passed back via ``extra_turbines`` (with its nameplate) so its
    physical flow still sizes the reservoir bound — dispatch untouched."""
    system = {
        "junction_array": [{"name": "RJ"}, {"name": "DN"}],
        "reservoir_array": [{"name": "R", "junction": "RJ"}],
        "waterway_array": [],
        "generator_array": [
            {"uid": 1, "name": "R_U1", "pmax": 240.0},
            {"uid": 2, "name": "R_U2", "pmax": 0.0},  # offline → dropped turbine
        ],
        # only the online unit's turbine is in the dispatch array
        "turbine_array": [
            {
                "uid": 10,
                "name": "t_R_U1",
                "generator": "R_U1",
                "junction_a": "RJ",
                "junction_b": "DN",
                "production_factor": 2.0,
            }
        ],
    }
    # Online-only: conduit = 240/2 = 120 → fmax round(120*1.1)=132.
    base = json.loads(json.dumps(system))
    apply_reservoir_flow_estimates({"system": base}, input_dir=None)
    assert base["reservoir_array"][0]["fmax"] == pytest.approx(132.0)

    # Add the dropped offline unit back for the BOUND only, with nameplate.
    sys2 = json.loads(json.dumps(system))
    extra = [
        {
            "uid": 11,
            "name": "t_R_U2",
            "generator": "R_U2",
            "junction_a": "RJ",
            "junction_b": "DN",
            "production_factor": 2.0,
        }
    ]
    apply_reservoir_flow_estimates(
        {"system": sys2},
        input_dir=None,
        generator_capacities={"R_U2": 240.0},
        extra_turbines=extra,
    )
    # conduit = U1 240/2 + U2 nameplate 240/2 = 240 → fmax round(240*1.1)=264.
    assert sys2["reservoir_array"][0]["fmax"] == pytest.approx(264.0)
    # dispatch turbine_array still has only the online unit.
    assert len(sys2["turbine_array"]) == 1


def test_uncapped_outlet_folds_downstream_to_nonreservoir() -> None:
    """An unbounded outlet to a NON-reservoir junction (Ext_Maule-style)
    folds that junction's onward conveyance; spills to a sink or to a
    downstream reservoir do NOT."""
    base: dict[str, Any] = {
        "junction_array": [{"name": "RJ"}, {"name": "MID"}, {"name": "OUT"}],
        "reservoir_array": [{"name": "R", "junction": "RJ"}],
        "generator_array": [{"uid": 1, "name": "g", "pmax": 40.0}],
        "turbine_array": [
            {
                "uid": 9,
                "name": "t",
                "generator": "g",
                "junction_a": "RJ",
                "junction_b": "OUT",
                "production_factor": 2.0,
            },
        ],
        "waterway_array": [
            {"uid": 1, "name": "Ext", "junction_a": "RJ", "junction_b": "MID"},
            {
                "uid": 2,
                "name": "mid_out",
                "junction_a": "MID",
                "junction_b": "OUT",
                "fmax": 50.0,
            },
        ],
    }
    apply_reservoir_flow_estimates({"system": base}, input_dir=None)
    # conduit = turbine 40/2=20 + MID onward 50 = 70 -> fmax round(70*1.1)=77.
    assert base["reservoir_array"][0]["fmax"] == pytest.approx(77.0)

    # MID is a reservoir → no fold (cascade owns it).
    sys2: dict[str, Any] = {
        "junction_array": [{"name": "RJ"}, {"name": "MID"}, {"name": "OUT"}],
        "reservoir_array": [
            {"name": "R", "junction": "RJ"},
            {"name": "R2", "junction": "MID"},
        ],
        "generator_array": [{"uid": 1, "name": "g", "pmax": 40.0}],
        "turbine_array": [
            {
                "uid": 9,
                "name": "t",
                "generator": "g",
                "junction_a": "RJ",
                "junction_b": "OUT",
                "production_factor": 2.0,
            },
        ],
        "waterway_array": [
            {"uid": 1, "name": "Ext", "junction_a": "RJ", "junction_b": "MID"},
            {
                "uid": 2,
                "name": "mid_out",
                "junction_a": "MID",
                "junction_b": "OUT",
                "fmax": 50.0,
            },
        ],
    }
    apply_reservoir_flow_estimates({"system": sys2}, input_dir=None)
    assert {r["name"]: r for r in sys2["reservoir_array"]}["R"][
        "fmax"
    ] == pytest.approx(22.0)


def test_widen_extraction_bounds_symmetric() -> None:
    """Symmetric ±factor·max(|fmin|,|fmax|) override of estimator bounds."""
    from gtopt_shared.reservoir_flow import (  # noqa: PLC0415
        widen_extraction_bounds_symmetric,
    )

    planning = {
        "system": {
            "reservoir_array": [
                {"name": "A", "fmin": -51.0, "fmax": 110.0},  # e=110 -> ±220
                {"name": "B", "fmin": -387.0, "fmax": 156.0},  # e=387 -> ±774
                {"name": "C"},  # no bounds -> untouched
            ]
        }
    }
    out = widen_extraction_bounds_symmetric(planning, factor=2.0)
    res = {r["name"]: r for r in planning["system"]["reservoir_array"]}
    assert res["A"]["fmin"] == pytest.approx(-220.0)
    assert res["A"]["fmax"] == pytest.approx(220.0)
    assert res["B"]["fmin"] == pytest.approx(-774.0)
    assert res["B"]["fmax"] == pytest.approx(774.0)
    assert "fmin" not in res["C"] and "fmax" not in res["C"]
    assert out["A"] == (pytest.approx(-220.0), pytest.approx(220.0))


def _spillway_system() -> dict[str, Any]:
    """A LARGE reservoir + a SMALL one, each with a ``Vert_*`` spill arc.

    ``BIG_gen`` is a non-spill arc that must always survive.
    """
    return {
        "waterway_array": [
            {"name": "Vert_BIG", "junction_a": "BIG", "junction_b": "ocean"},
            {"name": "Vert_SMALL", "junction_a": "SMALL", "junction_b": "ocean"},
            {"name": "BIG_gen", "junction_a": "BIG", "junction_b": "down"},
        ]
    }


def test_drop_large_reservoir_spillways() -> None:
    """Only large-reservoir ``Vert_*`` arcs are dropped; others survive."""
    from gtopt_shared.reservoir_flow import (  # noqa: PLC0415
        drop_large_reservoir_spillways,
    )

    caps = {"BIG": 500.0, "SMALL": 50.0}

    # threshold disabled (<= 0) → nothing dropped.
    sys_off = _spillway_system()
    assert not drop_large_reservoir_spillways(sys_off, caps, 0.0)
    assert len(sys_off["waterway_array"]) == 3

    # threshold 300 → only the large reservoir's Vert_ arc is dropped;
    # the small reservoir's arc and the non-spill BIG_gen arc are kept.
    sys_on = _spillway_system()
    dropped = drop_large_reservoir_spillways(sys_on, caps, 300.0)
    assert dropped == ["Vert_BIG"]
    kept = {w["name"] for w in sys_on["waterway_array"]}
    assert kept == {"Vert_SMALL", "BIG_gen"}

    # a protected waterway (e.g. referenced by a UserConstraint) is spared.
    sys_prot = _spillway_system()
    assert not drop_large_reservoir_spillways(
        sys_prot, caps, 300.0, protected_waterways=frozenset({"Vert_BIG"})
    )
    assert len(sys_prot["waterway_array"]) == 3
