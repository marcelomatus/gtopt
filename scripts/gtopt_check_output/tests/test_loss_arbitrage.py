# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the loss-arbitrage detector (synthetic outputs, no solver)."""

from pathlib import Path

import pandas as pd

from gtopt_check_output._checks import check_loss_arbitrage, run_all_checks
from gtopt_check_output._loss_arbitrage import (
    MIGRATION_NOTE,
    SYMPTOM_IDLE,
    SYMPTOM_INFLATION,
    SYMPTOM_PHANTOM,
    build_overrides,
    detect_loss_arbitrage,
    format_table_lines,
)

# Line 1: R=0.01, V=1 → coef 0.01; line 2: R=0.04, V=2 → coef 0.01.
_PLANNING = {
    "options": {},
    "system": {
        "bus_array": [
            {"uid": 1, "name": "b1"},
            {"uid": 2, "name": "b2"},
            {"uid": 3, "name": "b3"},
        ],
        "generator_array": [
            {"uid": 1, "name": "g1", "type": "termica", "bus": "b1", "pmax": 100},
        ],
        "demand_array": [{"uid": 1, "name": "d1", "bus": "b2"}],
        "line_array": [
            {
                "uid": 1,
                "name": "L1",
                "bus_a": "b1",
                "bus_b": "b2",
                "tmax_ab": 100,
                "resistance": 0.01,
                "voltage": 1.0,
            },
            {
                "uid": 2,
                "name": "L2",
                "bus_a": 2,  # uid reference form
                "bus_b": "b3",
                "tmax_ab": 100,
                "resistance": 0.04,
                "voltage": 2.0,
            },
        ],
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 2.0}, {"uid": 2, "duration": 4.0}],
    },
}


def _write(path: Path, df: pd.DataFrame) -> None:
    """Write *df* as a hive-partitioned parquet dataset (the layout the
    streaming readers open) — same convention as ``test_checks.py``."""
    target = path.with_suffix(".parquet")
    part_dir = target / "scene=0" / "phase=0"
    part_dir.mkdir(parents=True, exist_ok=True)
    df.to_parquet(part_dir / "part.parquet", index=False)


def _long(rows: list[tuple[int, int, int, int, float]]) -> pd.DataFrame:
    """Long-form (scenario, stage, block, uid, value) frame."""
    return pd.DataFrame(rows, columns=["scenario", "stage", "block", "uid", "value"])


def _make_clean_results(tmp_path: Path) -> Path:
    """Flow at 10 MW, booked loss below the quadratic (tangent under-
    approximation), positive LMPs everywhere."""
    results = tmp_path / "results"
    _write(
        results / "Line" / "flow_sol.csv",
        _long([(1, 1, 1, 1, 10.0), (1, 1, 2, 1, 10.0)]),
    )
    # analytic = 0.01 · 100 = 1.0 → booked 0.9 is clean.
    _write(
        results / "Line" / "loss_sol.csv",
        _long([(1, 1, 1, 1, 0.9), (1, 1, 2, 1, 0.9)]),
    )
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, 20.0), (1, 1, 1, 2, 21.0)]),
    )
    return results


def test_clean_case_no_flags(tmp_path: Path):
    results = _make_clean_results(tmp_path)
    report = detect_loss_arbitrage(results, _PLANNING)
    assert report.ok
    assert not report.diagnoses
    assert report.total_fictitious_mwh == 0.0
    assert format_table_lines(report) == ["no loss-arbitrage symptoms detected"]
    assert build_overrides(report) == {"line_array": []}


def test_loss_inflation_flagged(tmp_path: Path):
    """Booked loss above (R/V²)·f² flags loss-inflation; the fictitious
    energy is duration-weighted and the pair-sum sizes the override."""
    results = tmp_path / "results"
    # Line 1 flows 10 MW in both blocks; analytic loss = 1.0 MW.
    _write(
        results / "Line" / "flow_sol.csv",
        _long([(1, 1, 1, 1, 10.0), (1, 1, 2, 1, 10.0)]),
    )
    # Booked loss 5.0 MW in block 1 (excess 4.0 × 2 h), clean in block 2.
    _write(
        results / "Line" / "loss_sol.csv",
        _long([(1, 1, 1, 1, 5.0), (1, 1, 2, 1, 1.0)]),
    )
    # Negative LMPs at both ends in block 1: pair-sum = −80.
    _write(
        results / "Bus" / "balance_dual.csv",
        _long(
            [
                (1, 1, 1, 1, -50.0),
                (1, 1, 1, 2, -30.0),
                (1, 1, 2, 1, 20.0),
                (1, 1, 2, 2, 20.0),
            ]
        ),
    )
    report = detect_loss_arbitrage(results, _PLANNING)
    assert not report.ok
    assert len(report.diagnoses) == 1
    d = report.diagnoses[0]
    assert d.uid == 1
    assert d.name == "L1"
    assert d.worst_symptom == SYMPTOM_INFLATION
    assert d.arbitrage_mwh == 8.0  # (5.0 − 1.0) × 2 h
    assert d.worst_pair_sum == -80.0
    assert report.total_fictitious_mwh == 8.0
    assert MIGRATION_NOTE in report.notes

    overrides = build_overrides(report)
    # ceil(80 / 2 × 1.3) = ceil(52) = 52
    assert overrides == {"line_array": [{"uid": 1, "loss_cost_eps": 52}]}


def test_idle_line_loss_flagged(tmp_path: Path):
    """Loss booked on a line with no flow row at all (long form drops
    zeros) flags the idle-line channel."""
    results = tmp_path / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 2, 10.0)]))
    # Line 1 books 3 MW loss with zero flow.
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 3.0)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, -10.0), (1, 1, 1, 2, -12.0)]),
    )
    report = detect_loss_arbitrage(results, _PLANNING)
    assert len(report.diagnoses) == 1
    d = report.diagnoses[0]
    assert d.uid == 1
    assert d.worst_symptom == SYMPTOM_IDLE
    assert d.arbitrage_mwh == 6.0  # 3 MW × 2 h
    assert d.worst_pair_sum == -22.0
    # ceil(22 / 2 × 1.3) = ceil(14.3) = 15
    assert build_overrides(report) == {"line_array": [{"uid": 1, "loss_cost_eps": 15}]}


def test_phantom_circulation_flagged(tmp_path: Path):
    """Simultaneous directional flows (flow_sol + extras flown_sol)
    trip channel A; booked loss above analytic-at-net counts as the
    fictitious energy."""
    results = tmp_path / "results"
    # Net flow 1 MW, flown 5 MW → flow_p 6, flow_n 5: min=5 > tol,
    # sum=11 > 3 × |1|.
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 1.0)]))
    _write(results / "Line" / "flown_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    # analytic at net flow = 0.01 · 1 = 0.01; PWL books both legs.
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 0.61)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, -5.0), (1, 1, 1, 2, -7.0)]),
    )
    report = detect_loss_arbitrage(results, _PLANNING)
    assert len(report.diagnoses) == 1
    d = report.diagnoses[0]
    assert d.worst_symptom == SYMPTOM_PHANTOM
    assert abs(d.arbitrage_mwh - 1.2) < 1e-9  # (0.61 − 0.01) × 2 h
    assert d.worst_pair_sum == -12.0


def test_phantom_legacy_directional_pair(tmp_path: Path):
    """Pre-unified outputs (flowp_sol/flown_sol, no flow_sol) still
    feed channel A; without loss_sol the circulating-flow proxy is
    used for the fictitious energy."""
    results = tmp_path / "results"
    _write(results / "Line" / "flowp_sol.csv", _long([(1, 1, 1, 1, 6.0)]))
    _write(results / "Line" / "flown_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    report = detect_loss_arbitrage(results, _PLANNING)
    assert len(report.diagnoses) == 1
    d = report.diagnoses[0]
    assert d.worst_symptom == SYMPTOM_PHANTOM
    assert d.arbitrage_mwh == 20.0  # 2 × min(6, 5) × 2 h
    assert d.worst_pair_sum is None  # no balance_dual written
    assert build_overrides(report) == {"line_array": []}
    assert any("Bus/balance_dual absent" in n for n in report.notes)


def test_no_phantom_without_directional_split(tmp_path: Path):
    """flow_sol alone cannot flag channel A — a note says why."""
    results = tmp_path / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 1.0)]))
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 0.005)]))
    report = detect_loss_arbitrage(results, _PLANNING)
    assert report.ok
    assert any("flown_sol absent" in n for n in report.notes)


def test_wide_layout_inputs(tmp_path: Path):
    """Wide (uid:N column) outputs are melted and flag identically."""
    results = tmp_path / "results"
    wide = pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [10.0]})
    _write(results / "Line" / "flow_sol.csv", wide)
    loss_wide = pd.DataFrame(
        {"scenario": [1], "stage": [1], "block": [1], "uid:1": [5.0]}
    )
    _write(results / "Line" / "loss_sol.csv", loss_wide)
    dual_wide = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [-50.0],
            "uid:2": [-30.0],
        }
    )
    _write(results / "Bus" / "balance_dual.csv", dual_wide)
    report = detect_loss_arbitrage(results, _PLANNING)
    assert len(report.diagnoses) == 1
    assert report.diagnoses[0].worst_symptom == SYMPTOM_INFLATION
    assert report.diagnoses[0].worst_pair_sum == -80.0


def test_missing_flow_stream_skips(tmp_path: Path):
    results = tmp_path / "results"
    results.mkdir()
    report = detect_loss_arbitrage(results, _PLANNING)
    assert report.ok
    assert any("check skipped" in n for n in report.notes)


def test_positive_pair_sum_not_sized(tmp_path: Path):
    """A flagged line whose worst pair-sum is non-negative gets no
    loss_cost_eps override (no negative-LMP driver)."""
    results = tmp_path / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 10.0)]))
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, 20.0), (1, 1, 1, 2, 20.0)]),
    )
    report = detect_loss_arbitrage(results, _PLANNING)
    assert len(report.diagnoses) == 1
    assert report.diagnoses[0].worst_pair_sum == 40.0
    assert build_overrides(report) == {"line_array": []}


def test_table_and_footer_format(tmp_path: Path):
    results = tmp_path / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 10.0)]))
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, -50.0), (1, 1, 1, 2, -30.0)]),
    )
    report = detect_loss_arbitrage(results, _PLANNING)
    lines = format_table_lines(report)
    assert lines[0].startswith("  uid") or "uid" in lines[0]
    assert any("L1" in ln and SYMPTOM_INFLATION in ln for ln in lines)
    assert lines[-1] == "total fictitious-loss energy: 8.00 MWh"


def test_check_wrapper_findings(tmp_path: Path):
    """check_loss_arbitrage emits WARNING + table + migration note."""
    results = tmp_path / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 10.0)]))
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, -50.0), (1, 1, 1, 2, -30.0)]),
    )
    findings, la_report = check_loss_arbitrage(results, _PLANNING)
    warnings = [f for f in findings if f.severity == "WARNING"]
    assert any("loss-arbitrage" in f.message for f in warnings)
    assert any(MIGRATION_NOTE in f.message for f in warnings)
    assert not la_report.ok


def test_check_wrapper_clean(tmp_path: Path):
    results = _make_clean_results(tmp_path)
    findings, la_report = check_loss_arbitrage(results, _PLANNING)
    assert la_report.ok
    assert all(f.severity == "INFO" for f in findings)
    assert any("no loss-arbitrage symptoms" in f.message for f in findings)


def test_run_all_checks_exposes_report(tmp_path: Path):
    """run_all_checks stores the diagnosis under indicators."""
    results = _make_clean_results(tmp_path)
    report = run_all_checks(results, _PLANNING)
    assert "loss_arbitrage" in report.indicators
    assert report.indicators["loss_arbitrage"].ok


def test_main_emit_overrides(tmp_path: Path, capsys):
    """CLI --emit-overrides writes the JSON snippet to a file."""
    import json

    import pytest

    from gtopt_check_output.main import main

    case = tmp_path / "case"
    case.mkdir()
    (case / "case.json").write_text(json.dumps(_PLANNING))
    results = case / "results"
    _write(results / "Line" / "flow_sol.csv", _long([(1, 1, 1, 1, 10.0)]))
    _write(results / "Line" / "loss_sol.csv", _long([(1, 1, 1, 1, 5.0)]))
    _write(
        results / "Bus" / "balance_dual.csv",
        _long([(1, 1, 1, 1, -50.0), (1, 1, 1, 2, -30.0)]),
    )
    out_file = tmp_path / "overrides.json"
    with pytest.raises(SystemExit):
        main([str(case), "--no-color", "--emit-overrides", str(out_file)])
    capsys.readouterr()
    snippet = json.loads(out_file.read_text())
    assert snippet == {"line_array": [{"uid": 1, "loss_cost_eps": 52}]}
