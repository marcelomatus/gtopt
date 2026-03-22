# SPDX-License-Identifier: BSD-3-Clause
"""Tests for output validation checks."""

from pathlib import Path

import pandas as pd

from gtopt_check_output._checks import (
    check_expected_files,
    check_generation_vs_demand,
    check_load_shedding,
    check_renewable_curtailment,
    compute_congestion_ranking,
    compute_cost_breakdown,
    compute_energy_by_type,
    compute_lmp_statistics,
    run_all_checks,
)

_MINIMAL_PLANNING = {
    "options": {},
    "system": {
        "generator_array": [
            {"uid": 1, "name": "g1", "type": "termica", "bus": "b1", "pmax": 100},
            {"uid": 2, "name": "g2", "type": "hidro", "bus": "b1", "pmax": 50},
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": "b1"},
        ],
        "line_array": [
            {"uid": 1, "name": "L1", "bus_a": "b1", "bus_b": "b2", "tmax_ab": 100},
        ],
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 24.0}],
    },
}


def _write_csv(path: Path, df: pd.DataFrame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(path, index=False)


def _make_results(tmp_path: Path) -> Path:
    """Create a minimal results directory with output files."""
    results = tmp_path / "results"

    # solution.csv
    _write_csv(
        results / "solution.csv",
        pd.DataFrame({"scene": [0], "phase": [0], "status": [0], "obj_value": [500.0]}),
    )
    # Generator/generation_sol.csv
    gen = pd.DataFrame(
        {
            "scenario": [1, 1],
            "stage": [1, 1],
            "block": [1, 1],
            "uid:1": [80.0, 80.0],
            "uid:2": [20.0, 20.0],
        }
    )
    _write_csv(results / "Generator" / "generation_sol.csv", gen)

    # Generator/generation_cost.csv
    cost = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [800.0],
            "uid:2": [50.0],
        }
    )
    _write_csv(results / "Generator" / "generation_cost.csv", cost)

    # Bus/balance_dual.csv
    lmp = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [25.0],
            "uid:2": [30.0],
        }
    )
    _write_csv(results / "Bus" / "balance_dual.csv", lmp)

    # Demand/load_sol.csv
    load = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [100.0],
        }
    )
    _write_csv(results / "Demand" / "load_sol.csv", load)

    # Demand/fail_sol.csv (no shedding)
    fail = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [0.0],
        }
    )
    _write_csv(results / "Demand" / "fail_sol.csv", fail)

    # Line/flowp_sol.csv
    flow = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [50.0],
        }
    )
    _write_csv(results / "Line" / "flowp_sol.csv", flow)

    return results


def test_check_expected_files(tmp_path: Path):
    """Expected files check passes with complete output."""
    results = _make_results(tmp_path)
    findings = check_expected_files(results)
    critical = [f for f in findings if f.severity == "CRITICAL"]
    assert not critical


def test_check_expected_files_missing(tmp_path: Path):
    """Missing solution.csv is critical."""
    results = tmp_path / "empty_results"
    results.mkdir()
    findings = check_expected_files(results)
    critical = [f for f in findings if f.severity == "CRITICAL"]
    assert len(critical) >= 1


def test_check_no_load_shedding(tmp_path: Path):
    """No load shedding when fail_sol is all zeros."""
    results = _make_results(tmp_path)
    findings = check_load_shedding(results, _MINIMAL_PLANNING)
    assert any("no load shedding" in f.message for f in findings)


def test_check_load_shedding_detected(tmp_path: Path):
    """Load shedding is detected when fail_sol > 0."""
    results = _make_results(tmp_path)
    fail = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [10.0],
        }
    )
    _write_csv(results / "Demand" / "fail_sol.csv", fail)
    findings = check_load_shedding(results, _MINIMAL_PLANNING)
    assert any("load shedding detected" in f.message for f in findings)


def test_gen_vs_demand(tmp_path: Path):
    """Generation >= demand produces INFO."""
    results = _make_results(tmp_path)
    findings = check_generation_vs_demand(results, _MINIMAL_PLANNING)
    assert all(f.severity != "CRITICAL" for f in findings)


def test_energy_by_type(tmp_path: Path):
    """Energy breakdown by type includes termica and hidro."""
    results = _make_results(tmp_path)
    _, energy = compute_energy_by_type(results, _MINIMAL_PLANNING)
    assert "termica" in energy
    assert "hidro" in energy


def test_congestion_ranking(tmp_path: Path):
    """Congestion ranking runs without error."""
    results = _make_results(tmp_path)
    findings = compute_congestion_ranking(results, _MINIMAL_PLANNING)
    assert findings  # at least one finding


def test_lmp_statistics(tmp_path: Path):
    """LMP statistics are computed."""
    results = _make_results(tmp_path)
    findings = compute_lmp_statistics(results, _MINIMAL_PLANNING)
    assert any("LMP" in f.message for f in findings)


def test_cost_breakdown(tmp_path: Path):
    """Cost breakdown includes generation."""
    results = _make_results(tmp_path)
    findings = compute_cost_breakdown(results, _MINIMAL_PLANNING)
    assert any("generation" in f.message for f in findings)


def test_congestion_warning(tmp_path: Path):
    """Lines above 90% utilization produce a warning."""
    results = _make_results(tmp_path)
    # Override flow to be near capacity (tmax=100, flow=95)
    flow = pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [95.0]})
    _write_csv(results / "Line" / "flowp_sol.csv", flow)
    findings = compute_congestion_ranking(results, _MINIMAL_PLANNING)
    warnings = [f for f in findings if f.severity == "WARNING"]
    assert len(warnings) >= 1
    assert any("90%" in f.message for f in warnings)


def test_lmp_high_spread_warning(tmp_path: Path):
    """High LMP spread produces a warning."""
    results = _make_results(tmp_path)
    lmp = pd.DataFrame(
        {
            "scenario": [1, 1],
            "stage": [1, 1],
            "block": [1, 2],
            "uid:1": [5.0, 200.0],
            "uid:2": [10.0, 200.0],
        }
    )
    _write_csv(results / "Bus" / "balance_dual.csv", lmp)
    findings = compute_lmp_statistics(results, _MINIMAL_PLANNING)
    warnings = [f for f in findings if f.severity == "WARNING"]
    assert any("spread" in f.message.lower() for f in warnings)


def test_negative_battery_soc_warning(tmp_path: Path):
    """Negative battery SoC produces a warning."""
    from gtopt_check_output._checks import check_battery_soc

    results = _make_results(tmp_path)
    soc = pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [-5.0]})
    _write_csv(results / "Battery" / "energy_sol.csv", soc)
    findings = check_battery_soc(results, _MINIMAL_PLANNING)
    warnings = [f for f in findings if f.severity == "WARNING"]
    assert len(warnings) >= 1


def test_no_curtailment(tmp_path: Path):
    """No curtailment when spillover_sol is missing."""
    results = _make_results(tmp_path)
    findings, data = check_renewable_curtailment(results, _MINIMAL_PLANNING)
    # No GeneratorProfile/spillover_sol → empty findings
    assert not findings
    assert not data


_PLANNING_WITH_PROFILE = {
    "options": {},
    "system": {
        "generator_array": [
            {"uid": 1, "name": "g1", "type": "termica", "bus": "b1", "pmax": 100},
            {"uid": 2, "name": "g_solar", "type": "solar", "bus": "b1", "pmax": 50},
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": "b1"},
        ],
        "line_array": [],
        "generator_profile_array": [
            {"uid": 1, "name": "gp_solar", "generator": "g_solar"},
        ],
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 1.0}],
    },
}


def test_curtailment_detected(tmp_path: Path):
    """Curtailment is detected when spillover_sol > 0."""
    results = _make_results(tmp_path)
    # Generator generation_sol: g_solar produces 30 MW
    gen = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [70.0],
            "uid:2": [30.0],
        }
    )
    _write_csv(results / "Generator" / "generation_sol.csv", gen)
    # GeneratorProfile spillover_sol: 20 MW curtailed
    spill = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [20.0],
        }
    )
    _write_csv(results / "GeneratorProfile" / "spillover_sol.csv", spill)
    findings, data = check_renewable_curtailment(results, _PLANNING_WITH_PROFILE)
    assert any("curtailment detected" in f.message for f in findings)
    assert data.get("gp_solar", 0) > 0
    # Check percentage: 20 / (30 + 20) = 40%
    warning = [f for f in findings if f.severity == "WARNING"][0]
    assert "40.0%" in warning.message


def test_no_curtailment_zero_spillover(tmp_path: Path):
    """No curtailment when spillover_sol is all zeros."""
    results = _make_results(tmp_path)
    gen = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [70.0],
            "uid:2": [50.0],
        }
    )
    _write_csv(results / "Generator" / "generation_sol.csv", gen)
    spill = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [0.0],
        }
    )
    _write_csv(results / "GeneratorProfile" / "spillover_sol.csv", spill)
    findings, curtailment = check_renewable_curtailment(results, _PLANNING_WITH_PROFILE)
    assert any("no renewable curtailment" in f.message for f in findings)
    assert curtailment.get("gp_solar", -1) == 0.0


def test_curtailment_multi_block_energy(tmp_path: Path):
    """Curtailment energy accounts for block durations correctly."""
    planning = {
        "options": {},
        "system": {
            "generator_array": [
                {"uid": 1, "name": "g_th", "type": "termica", "bus": "b1", "pmax": 200},
                {"uid": 2, "name": "g_wind", "type": "wind", "bus": "b1", "pmax": 100},
            ],
            "demand_array": [{"uid": 1, "name": "d1", "bus": "b1"}],
            "line_array": [],
            "generator_profile_array": [
                {"uid": 1, "name": "gp_wind", "generator": "g_wind"},
            ],
        },
        "simulation": {
            "block_array": [
                {"uid": 1, "duration": 4.0},
                {"uid": 2, "duration": 8.0},
                {"uid": 3, "duration": 12.0},
            ],
        },
    }
    results = _make_results(tmp_path)
    # Spillover: 10 MW in block 1 (4h), 5 MW in block 2 (8h), 0 in block 3
    spill = pd.DataFrame(
        {
            "scenario": [1, 1, 1],
            "stage": [1, 1, 1],
            "block": [1, 2, 3],
            "uid:1": [10.0, 5.0, 0.0],
        }
    )
    _write_csv(results / "GeneratorProfile" / "spillover_sol.csv", spill)
    # Generation: 50 MW all blocks
    gen = pd.DataFrame(
        {
            "scenario": [1, 1, 1],
            "stage": [1, 1, 1],
            "block": [1, 2, 3],
            "uid:1": [100.0, 100.0, 100.0],
            "uid:2": [50.0, 50.0, 50.0],
        }
    )
    _write_csv(results / "Generator" / "generation_sol.csv", gen)
    findings, data = check_renewable_curtailment(results, planning)
    # Curtailment: 10*4 + 5*8 + 0*12 = 80 MWh
    assert data["gp_wind"] == 80.0
    # Actual gen: 50*4 + 50*8 + 50*12 = 1200 MWh
    # Potential: 1200 + 80 = 1280 MWh → 6.25%
    warning = [f for f in findings if f.severity == "WARNING"][0]
    assert "80.0 MWh" in warning.message
    assert "6.2%" in warning.message


def test_curtailment_multiple_profiles(tmp_path: Path):
    """Multiple profiles are reported sorted by curtailment energy."""
    planning = {
        "options": {},
        "system": {
            "generator_array": [
                {
                    "uid": 1,
                    "name": "g_solar",
                    "type": "solar",
                    "bus": "b1",
                    "pmax": 100,
                },
                {"uid": 2, "name": "g_wind", "type": "wind", "bus": "b1", "pmax": 200},
            ],
            "demand_array": [{"uid": 1, "name": "d1", "bus": "b1"}],
            "line_array": [],
            "generator_profile_array": [
                {"uid": 1, "name": "gp_solar", "generator": "g_solar"},
                {"uid": 2, "name": "gp_wind", "generator": "g_wind"},
            ],
        },
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1.0}],
        },
    }
    results = _make_results(tmp_path)
    # Solar spills 5 MW, wind spills 30 MW
    spill = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [5.0],
            "uid:2": [30.0],
        }
    )
    _write_csv(results / "GeneratorProfile" / "spillover_sol.csv", spill)
    gen = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [45.0],
            "uid:2": [70.0],
        }
    )
    _write_csv(results / "Generator" / "generation_sol.csv", gen)
    findings, data = check_renewable_curtailment(results, planning)
    assert data["gp_wind"] == 30.0
    assert data["gp_solar"] == 5.0
    # Per-profile breakdown: wind first (30 > 5)
    info_msgs = [f.message for f in findings if "gp_wind" in f.message]
    assert len(info_msgs) == 1
    assert "30.0" in info_msgs[0]


def test_run_all_checks(tmp_path: Path):
    """Full check run completes and returns a report."""
    results = _make_results(tmp_path)
    report = run_all_checks(results, _MINIMAL_PLANNING)
    assert report.ok  # no critical findings for valid output
    assert len(report.findings) > 5  # multiple checks ran


def test_run_all_checks_with_curtailment(tmp_path: Path):
    """Full check run includes curtailment indicators."""
    results = _make_results(tmp_path)
    spill = pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [10.0],
        }
    )
    _write_csv(results / "GeneratorProfile" / "spillover_sol.csv", spill)
    report = run_all_checks(results, _PLANNING_WITH_PROFILE)
    assert "renewable_curtailment" in report.indicators
