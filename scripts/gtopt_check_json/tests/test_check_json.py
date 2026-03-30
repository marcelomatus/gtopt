# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_json."""

import json
from pathlib import Path
from typing import Any

import pytest

from gtopt_check_json._checks import (
    Severity,
    analyse_bus_islands,
    check_affluent_nonneg,
    check_battery_efficiency,
    check_boundary_cuts,
    check_bus_connectivity,
    check_capacity_adequacy,
    check_cascade_levels,
    check_cascade_solver_type,
    check_demand_lmax_nonneg,
    check_element_references,
    check_name_uniqueness,
    check_sddp_options,
    check_simulation_mode,
    check_uid_uniqueness,
    check_unreferenced_elements,
    run_all_checks,
)
from gtopt_check_json._config import (
    default_config_path,
    is_check_enabled,
    load_config,
    save_config,
)
from gtopt_check_json._info import (
    compute_indicators,
    format_indicators,
    format_info,
)
from gtopt_check_json.gtopt_check_json import check_json, main


# ── Test fixtures ────────────────────────────────────────────────────────────

_VALID_CASE: dict = {
    "options": {
        "use_kirchhoff": True,
        "use_single_bus": False,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
        "input_directory": "input",
    },
    "simulation": {
        "block_array": [
            {"uid": 1, "duration": 1},
            {"uid": 2, "duration": 1},
        ],
        "stage_array": [
            {"uid": 1, "first_block": 0, "count_block": 2},
        ],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "test_valid",
        "bus_array": [
            {"uid": 1, "name": "b1", "reference_theta": 0},
            {"uid": 2, "name": "b2"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": 1,
                "pmax": 100,
                "gcost": 20,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": 2, "lmax": 50},
        ],
        "line_array": [
            {
                "uid": 1,
                "name": "l1_2",
                "bus_a": 1,
                "bus_b": 2,
                "reactance": 0.05,
            },
        ],
    },
}


# ── Info tests ───────────────────────────────────────────────────────────────


class TestFormatInfo:
    """Test system statistics output."""

    def test_includes_system_name(self) -> None:
        info = format_info(_VALID_CASE)
        assert "test_valid" in info

    def test_includes_element_counts(self) -> None:
        info = format_info(_VALID_CASE)
        assert "Buses" in info
        assert "Generators" in info
        assert "Demands" in info
        assert "Lines" in info

    def test_includes_options(self) -> None:
        info = format_info(_VALID_CASE)
        assert "use_kirchhoff" in info
        assert "true" in info.lower()

    def test_includes_simulation_counts(self) -> None:
        info = format_info(_VALID_CASE)
        assert "Blocks" in info
        assert "Stages" in info
        assert "Scenarios" in info


# ── UID uniqueness ───────────────────────────────────────────────────────────


class TestUidUniqueness:
    """Test check_uid_uniqueness."""

    def test_valid_no_findings(self) -> None:
        assert not check_uid_uniqueness(_VALID_CASE)

    def test_duplicate_generator_uid(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["generator_array"].append(
            {"uid": 1, "name": "g1_dup", "bus": 1, "pmax": 50}
        )
        findings = check_uid_uniqueness(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.CRITICAL
        assert "Generator" in findings[0].message

    def test_same_uid_different_classes_ok(self) -> None:
        """A UID can appear in different element classes."""
        case = json.loads(json.dumps(_VALID_CASE))
        # Generator uid=1 and line uid=1 is ok
        assert not check_uid_uniqueness(case)


# ── Name uniqueness ──────────────────────────────────────────────────────────


class TestNameUniqueness:
    """Test check_name_uniqueness."""

    def test_valid_no_findings(self) -> None:
        assert not check_name_uniqueness(_VALID_CASE)

    def test_duplicate_bus_name(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b1"})
        findings = check_name_uniqueness(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.CRITICAL
        assert "Bus" in findings[0].message


# ── Demand lmax non-negative ─────────────────────────────────────────────────


class TestDemandLmaxNonneg:
    """Test check_demand_lmax_nonneg."""

    def test_valid_no_findings(self) -> None:
        assert not check_demand_lmax_nonneg(_VALID_CASE)

    def test_negative_lmax_warning(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["demand_array"][0]["lmax"] = -10
        findings = check_demand_lmax_nonneg(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING

    def test_negative_in_list(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["demand_array"][0]["lmax"] = [50, -5, 30]
        findings = check_demand_lmax_nonneg(case)
        assert len(findings) == 1
        assert "-5" in findings[0].message


# ── Affluent non-negative ────────────────────────────────────────────────────


class TestAffluentNonneg:
    """Test check_affluent_nonneg."""

    def test_no_flows_no_findings(self) -> None:
        assert not check_affluent_nonneg(_VALID_CASE)

    def test_negative_affluent(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["flow_array"] = [
            {"uid": 1, "name": "f1", "junction": 1, "affluent": -5}
        ]
        findings = check_affluent_nonneg(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING


# ── Element references ───────────────────────────────────────────────────────


class TestElementReferences:
    """Test check_element_references."""

    def test_valid_no_findings(self) -> None:
        assert not check_element_references(_VALID_CASE)

    def test_generator_bad_bus(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["generator_array"][0]["bus"] = 999
        findings = check_element_references(case)
        assert len(findings) >= 1
        assert any(
            f.severity == Severity.CRITICAL and "Generator" in f.message
            for f in findings
        )

    def test_line_bad_bus_a(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["line_array"][0]["bus_a"] = 999
        findings = check_element_references(case)
        assert any("Line" in f.message and "bus_a" in f.message for f in findings)

    def test_converter_bad_battery(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["converter_array"] = [
            {
                "uid": 1,
                "name": "c1",
                "battery": 999,
                "generator": 1,
                "demand": 1,
            }
        ]
        findings = check_element_references(case)
        assert any(
            "Converter" in f.message and "battery" in f.message for f in findings
        )


# ── Bus connectivity ─────────────────────────────────────────────────────────


class TestBusConnectivity:
    """Test check_bus_connectivity."""

    def test_connected_no_findings(self) -> None:
        assert not check_bus_connectivity(_VALID_CASE)

    def test_island_detection(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_isolated"})
        findings = check_bus_connectivity(case)
        assert len(findings) >= 1
        assert findings[0].severity == Severity.WARNING
        assert "island" in findings[0].message

    def test_single_bus_no_findings(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"] = [{"uid": 1, "name": "b1"}]
        case["system"]["line_array"] = []
        assert not check_bus_connectivity(case)


# ── Bus island analysis ────────────────────────────────────────────────────


class TestAnalyseBusIslands:
    """Test analyse_bus_islands."""

    def test_no_islands(self) -> None:
        assert not analyse_bus_islands(_VALID_CASE)

    def test_empty_island(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_orphan"})
        islands = analyse_bus_islands(case)
        assert len(islands) == 1
        assert "b3_orphan" in islands[0].buses
        assert not islands[0].has_elements
        assert islands[0].generator_names == []
        assert islands[0].demand_names == []
        assert islands[0].line_names == []

    def test_island_with_generator(self) -> None:
        """Bus with a generator but no lines reports the generator name."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_ref"})
        case["system"]["generator_array"].append(
            {"uid": 2, "name": "g2", "bus": "b3_ref", "pmax": 50, "gcost": 10}
        )
        islands = analyse_bus_islands(case)
        assert len(islands) == 1
        assert islands[0].has_elements
        assert islands[0].generator_names == ["g2"]
        assert islands[0].demand_names == []

    def test_island_with_demand(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_dem"})
        case["system"]["demand_array"].append(
            {"uid": 2, "name": "d2", "bus": "b3_dem", "lmax": 30}
        )
        islands = analyse_bus_islands(case)
        assert len(islands) == 1
        assert islands[0].has_elements
        assert islands[0].demand_names == ["d2"]

    def test_island_with_internal_line(self) -> None:
        """Multi-bus island reports internal line names."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].extend(
            [
                {"uid": 3, "name": "b3"},
                {"uid": 4, "name": "b4"},
            ]
        )
        case["system"]["line_array"].append(
            {"uid": 2, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.1}
        )
        islands = analyse_bus_islands(case)
        assert len(islands) == 1
        assert set(islands[0].buses) == {"b3", "b4"}
        assert islands[0].line_names == ["l3_4"]

    def test_multiple_islands(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].extend(
            [
                {"uid": 3, "name": "b3_iso"},
                {"uid": 4, "name": "b4_iso"},
            ]
        )
        islands = analyse_bus_islands(case)
        assert len(islands) == 2


# ── Unreferenced elements ───────────────────────────────────────────────────


class TestUnreferencedElements:
    """Test check_unreferenced_elements."""

    def test_valid_no_findings(self) -> None:
        findings = check_unreferenced_elements(_VALID_CASE)
        assert not findings

    def test_unreferenced_bus_not_reported(self) -> None:
        """Unreferenced buses are shown in the connectivity table, not here."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_orphan"})
        findings = check_unreferenced_elements(case)
        assert not any("b3_orphan" in f.message for f in findings)


# ── Battery efficiency ──────────────────────────────────────────────────────


class TestBatteryEfficiency:
    """Test check_battery_efficiency."""

    def test_valid_no_findings(self) -> None:
        """No batteries → no findings."""
        assert not check_battery_efficiency(_VALID_CASE)

    def test_valid_efficiencies(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": 0.95,
                "output_efficiency": 0.9,
            },
        ]
        assert not check_battery_efficiency(case)

    def test_input_efficiency_above_one(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": 1.05,
                "output_efficiency": 0.9,
            },
        ]
        findings = check_battery_efficiency(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "input_efficiency" in findings[0].message
        assert "1.05" in findings[0].message

    def test_output_efficiency_above_one(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": 0.95,
                "output_efficiency": 1.1,
            },
        ]
        findings = check_battery_efficiency(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "output_efficiency" in findings[0].message

    def test_negative_efficiency(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": -0.5,
                "output_efficiency": 0.9,
            },
        ]
        findings = check_battery_efficiency(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "negative" in findings[0].message.lower()

    def test_both_above_one(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": 1.2,
                "output_efficiency": 1.3,
            },
        ]
        findings = check_battery_efficiency(case)
        assert len(findings) == 2

    def test_efficiency_list_with_bad_values(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {
                "uid": 1,
                "name": "bat1",
                "input_efficiency": [0.9, 1.1, 0.95],
                "output_efficiency": 0.9,
            },
        ]
        findings = check_battery_efficiency(case)
        assert len(findings) == 1
        assert "1.1" in findings[0].message

    def test_missing_efficiency_no_findings(self) -> None:
        """Batteries without efficiency fields should not produce findings."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["battery_array"] = [
            {"uid": 1, "name": "bat1"},
        ]
        assert not check_battery_efficiency(case)


# ── run_all_checks ──────────────────────────────────────────────────────────


class TestRunAllChecks:
    """Test the check orchestrator."""

    def test_valid_case_no_findings(self) -> None:
        findings = run_all_checks(_VALID_CASE)
        assert not findings

    def test_multiple_issues(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        # Duplicate UID
        case["system"]["generator_array"].append({"uid": 1, "name": "g1_dup", "bus": 1})
        # Negative lmax
        case["system"]["demand_array"][0]["lmax"] = -10
        findings = run_all_checks(case)
        assert len(findings) >= 2

    def test_disabled_check_skipped(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["demand_array"][0]["lmax"] = -10
        findings = run_all_checks(
            case,
            enabled_checks={"uid_uniqueness"},
        )
        assert all(f.check_id != "demand_lmax_nonneg" for f in findings)


# ── Config ───────────────────────────────────────────────────────────────────


class TestConfig:
    """Test configuration I/O."""

    def test_default_config_path(self) -> None:
        p = default_config_path()
        assert p.name == ".gtopt.conf"

    def test_load_missing_returns_defaults(self) -> None:
        cfg = load_config(Path("/nonexistent/.conf"))
        assert "check_uid_uniqueness" in cfg
        assert cfg["check_uid_uniqueness"] == "true"

    def test_save_and_load(self, tmp_path: Path) -> None:
        cfg_path = tmp_path / "test.conf"
        cfg = {"check_uid_uniqueness": "false", "color": "never"}
        save_config(cfg_path, cfg)
        loaded = load_config(cfg_path)
        assert loaded["check_uid_uniqueness"] == "false"

    def test_is_check_enabled(self) -> None:
        cfg = {"check_uid_uniqueness": "true"}
        assert is_check_enabled(cfg, "uid_uniqueness")
        cfg["check_uid_uniqueness"] = "false"
        assert not is_check_enabled(cfg, "uid_uniqueness")


# ── CLI / main ───────────────────────────────────────────────────────────────


class TestCLI:
    """Test the CLI entry point."""

    def test_info_flag(self, tmp_path: Path, capsys: Any) -> None:
        p = tmp_path / "test.json"
        p.write_text(json.dumps(_VALID_CASE), encoding="utf-8")
        rc = main(["--info", "--no-color", str(p)])
        assert rc == 0
        captured = capsys.readouterr()
        # Rich writes to stderr; check both streams
        combined = captured.out + captured.err
        assert "test_valid" in combined

    def test_check_valid(self, tmp_path: Path, capsys: Any) -> None:
        p = tmp_path / "test.json"
        p.write_text(json.dumps(_VALID_CASE), encoding="utf-8")
        rc = main(["--no-color", str(p)])
        assert rc == 0
        captured = capsys.readouterr()
        combined = captured.out + captured.err
        assert "passed" in combined.lower() or "0 critical" in combined.lower()

    def test_check_critical(self, tmp_path: Path, capsys: Any) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["generator_array"].append({"uid": 1, "name": "g1_dup", "bus": 1})
        p = tmp_path / "test.json"
        p.write_text(json.dumps(case), encoding="utf-8")
        rc = main(["--no-color", str(p)])
        assert rc == 1

    def test_no_files_error(self) -> None:
        rc = main(["--no-color"])
        assert rc == 2

    def test_missing_file_exits(self) -> None:
        with pytest.raises(SystemExit):
            check_json(["/nonexistent/file.json"])

    def test_file_without_extension(self, tmp_path: Path, capsys: Any) -> None:
        p = tmp_path / "case.json"
        p.write_text(json.dumps(_VALID_CASE), encoding="utf-8")
        rc = main(["--info", "--no-color", str(tmp_path / "case")])
        assert rc == 0


# ── Indicators ───────────────────────────────────────────────────────────────


# A multi-block case with two generators and two demands for indicator tests.
_INDICATOR_CASE: dict = {
    "options": {
        "use_kirchhoff": True,
        "use_single_bus": False,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
    },
    "simulation": {
        "block_array": [
            {"uid": 1, "duration": 2},
            {"uid": 2, "duration": 3},
            {"uid": 3, "duration": 5},
        ],
        "stage_array": [
            {"uid": 1, "first_block": 0, "count_block": 3},
        ],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "indicator_test",
        "bus_array": [
            {"uid": 1, "name": "b1", "reference_theta": 0},
            {"uid": 2, "name": "b2"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": 1,
                "pmax": 200,
                "capacity": 200,
                "gcost": 20,
            },
            {
                "uid": 2,
                "name": "g2",
                "bus": 2,
                "pmax": 150,
                "capacity": 150,
                "gcost": 35,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": 1, "lmax": [100, 120, 80]},
            {"uid": 2, "name": "d2", "bus": 2, "lmax": [50, 60, 40]},
        ],
        "line_array": [
            {"uid": 1, "name": "l1_2", "bus_a": 1, "bus_b": 2, "reactance": 0.05},
        ],
        "flow_array": [
            {"uid": 1, "name": "f1", "junction": 1, "discharge": [10.0, 20.0, 30.0]},
            {"uid": 2, "name": "f2", "junction": 2, "discharge": 5.0},
        ],
        "junction_array": [
            {"uid": 1, "name": "j1"},
            {"uid": 2, "name": "j2"},
        ],
    },
}


class TestComputeIndicators:
    """Test compute_indicators function."""

    def test_total_gen_capacity(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # g1(200) + g2(150) = 350
        assert ind.total_gen_capacity_mw == pytest.approx(350.0)

    def test_peak_demand(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # Block 1: d1(120) + d2(60) = 180
        assert ind.peak_demand_mw == pytest.approx(180.0)

    def test_min_demand(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # Block 2: d1(80) + d2(40) = 120
        assert ind.min_demand_mw == pytest.approx(120.0)

    def test_first_block_demand(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # Block 0: d1(100) + d2(50) = 150
        assert ind.first_block_demand_mw == pytest.approx(150.0)

    def test_last_block_demand(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # Block 2: d1(80) + d2(40) = 120
        assert ind.last_block_demand_mw == pytest.approx(120.0)

    def test_peak_demand_block(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        assert ind.peak_demand_block == 1  # 0-indexed

    def test_total_demand_by_block(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        assert len(ind.total_demand_by_block) == 3
        assert ind.total_demand_by_block[0] == pytest.approx(150.0)
        assert ind.total_demand_by_block[1] == pytest.approx(180.0)
        assert ind.total_demand_by_block[2] == pytest.approx(120.0)

    def test_total_energy(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # 150*2 + 180*3 + 120*5 = 300 + 540 + 600 = 1440
        assert ind.total_energy_mwh == pytest.approx(1440.0)

    def test_capacity_adequacy_ratio(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        # 350 / 180 ≈ 1.944
        assert ind.capacity_adequacy_ratio == pytest.approx(350.0 / 180.0)

    def test_num_counts(self) -> None:
        ind = compute_indicators(_INDICATOR_CASE)
        assert ind.num_generators == 2
        assert ind.num_demands == 2
        assert ind.num_blocks == 3
        assert ind.num_flows == 2

    def test_scalar_lmax(self) -> None:
        """Scalar lmax should be treated as constant across all blocks."""
        case = json.loads(json.dumps(_VALID_CASE))
        ind = compute_indicators(case)
        # d1 has lmax=50 (scalar), 2 blocks
        assert ind.peak_demand_mw == pytest.approx(50.0)
        assert ind.total_demand_by_block == [50.0, 50.0]

    def test_failure_gen_excluded_by_type(self) -> None:
        """Generators with type='falla' should be excluded regardless of pmax."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        case["system"]["generator_array"].append(
            {
                "uid": 99,
                "name": "failure",
                "bus": 1,
                "pmax": 9999,
                "gcost": 9999,
                "type": "falla",
            }
        )
        ind = compute_indicators(case)
        # Should still be 350, not 350 + 9999
        assert ind.total_gen_capacity_mw == pytest.approx(350.0)
        assert ind.num_generators == 2  # failure gen not counted

    def test_high_pmax_included_without_falla_type(self) -> None:
        """A generator with high pmax but no type='falla' should be included."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        case["system"]["generator_array"].append(
            {"uid": 99, "name": "big", "bus": 1, "pmax": 9999, "gcost": 10}
        )
        ind = compute_indicators(case)
        # 350 + 9999 = 10349 — included because type is not falla
        assert ind.total_gen_capacity_mw == pytest.approx(10349.0)
        assert ind.num_generators == 3

    def test_empty_case(self) -> None:
        """Empty planning dict should produce zero indicators."""
        ind = compute_indicators({})
        assert ind.total_gen_capacity_mw == 0.0
        assert ind.peak_demand_mw == 0.0
        assert ind.capacity_adequacy_ratio == float("inf")

    def test_first_block_affluent(self) -> None:
        """First block affluent should sum discharge at block 0."""
        ind = compute_indicators(_INDICATOR_CASE)
        # f1 discharge=[10,20,30] → first=10; f2 discharge=5.0 → first=5
        assert ind.first_block_affluent_avg == pytest.approx(15.0)

    def test_last_block_affluent(self) -> None:
        """Last block affluent should sum discharge at last block."""
        ind = compute_indicators(_INDICATOR_CASE)
        # f1 discharge=[10,20,30] → last=30; f2 discharge=5.0 → last=5
        assert ind.last_block_affluent_avg == pytest.approx(35.0)

    def test_no_flows_zero_affluent(self) -> None:
        """When there are no flows, affluent should be zero."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        case["system"]["flow_array"] = []
        ind = compute_indicators(case)
        assert ind.first_block_affluent_avg == 0.0
        assert ind.last_block_affluent_avg == 0.0
        assert ind.num_flows == 0

    def test_file_ref_demand_resolved_with_base_dir(self, tmp_path: Path) -> None:
        """Demand lmax as a file reference is resolved when base_dir given."""
        import pandas as pd  # pylint: disable=import-outside-toplevel

        # Write a Demand/lmax.parquet with 3 blocks and 2 demands
        dem_dir = tmp_path / "Demand"
        dem_dir.mkdir()
        df = pd.DataFrame(
            {
                "block": [1, 2, 3],
                "uid:1": [100.0, 120.0, 80.0],
                "uid:2": [50.0, 60.0, 40.0],
            }
        )
        df.to_parquet(dem_dir / "lmax.parquet", index=False)

        case: dict[str, Any] = {
            "options": {"input_directory": "."},
            "simulation": {
                "block_array": [
                    {"uid": 1, "duration": 2},
                    {"uid": 2, "duration": 3},
                    {"uid": 3, "duration": 5},
                ],
            },
            "system": {
                "generator_array": [
                    {"uid": 1, "name": "g1", "bus": 1, "capacity": 200},
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 1, "lmax": "lmax"},
                    {"uid": 2, "name": "d2", "bus": 2, "lmax": "lmax"},
                ],
                "flow_array": [],
            },
        }

        # Without base_dir → file ref is skipped → demand = 0
        ind_no = compute_indicators(case)
        assert ind_no.first_block_demand_mw == pytest.approx(0.0)

        # With base_dir → file ref is resolved
        ind = compute_indicators(case, base_dir=str(tmp_path))
        # Block 0: 100 + 50 = 150
        assert ind.first_block_demand_mw == pytest.approx(150.0)
        # Block 1: 120 + 60 = 180
        assert ind.peak_demand_mw == pytest.approx(180.0)
        # Block 2: 80 + 40 = 120
        assert ind.last_block_demand_mw == pytest.approx(120.0)
        # Energy: 150*2 + 180*3 + 120*5 = 300+540+600 = 1440
        assert ind.total_energy_mwh == pytest.approx(1440.0)
        # Adequacy: 200 / 180
        assert ind.capacity_adequacy_ratio == pytest.approx(200.0 / 180.0)

    def test_file_ref_flow_resolved_with_base_dir(self, tmp_path: Path) -> None:
        """Flow discharge as a file reference is resolved when base_dir given."""
        import pandas as pd  # pylint: disable=import-outside-toplevel

        # Write a Flow/discharge.parquet with scenario column
        flow_dir = tmp_path / "Flow"
        flow_dir.mkdir()
        df = pd.DataFrame(
            {
                "scenario": [1, 1, 1, 2, 2, 2],
                "stage": [1, 1, 1, 1, 1, 1],
                "block": [1, 2, 3, 1, 2, 3],
                "uid:1": [10.0, 20.0, 30.0, 20.0, 30.0, 40.0],
                "uid:2": [5.0, 6.0, 7.0, 15.0, 16.0, 17.0],
            }
        )
        df.to_parquet(flow_dir / "discharge.parquet", index=False)

        case: dict[str, Any] = {
            "options": {"input_directory": "."},
            "simulation": {
                "block_array": [
                    {"uid": 1, "duration": 1},
                    {"uid": 2, "duration": 1},
                    {"uid": 3, "duration": 1},
                ],
            },
            "system": {
                "generator_array": [],
                "demand_array": [],
                "flow_array": [
                    {"uid": 1, "name": "f1", "junction": 1, "discharge": "discharge"},
                    {"uid": 2, "name": "f2", "junction": 2, "discharge": "discharge"},
                ],
            },
        }

        # Without base_dir → 0
        ind_no = compute_indicators(case)
        assert ind_no.first_block_affluent_avg == pytest.approx(0.0)

        # With base_dir → resolved and averaged across scenarios
        ind = compute_indicators(case, base_dir=str(tmp_path))
        # Block 1 avg: uid:1=(10+20)/2=15, uid:2=(5+15)/2=10 → total=25
        assert ind.first_block_affluent_avg == pytest.approx(25.0)
        # Block 3 avg: uid:1=(30+40)/2=35, uid:2=(7+17)/2=12 → total=47
        assert ind.last_block_affluent_avg == pytest.approx(47.0)


class TestFormatIndicators:
    """Test format_indicators function."""

    def test_contains_capacity(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "350.00 MW" in text
        assert "gen capacity" in text.lower()

    def test_contains_first_block_demand(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "first block demand" in text.lower()
        assert "150.00 MW" in text

    def test_contains_last_block_demand(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "last block demand" in text.lower()

    def test_contains_energy(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "1'440.00 MWh" in text

    def test_contains_adequacy(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "adequacy" in text.lower()

    def test_contains_affluent(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "affluent" in text.lower()


class TestAvgDemandFcost:
    """Test _avg_demand_fcost helper in _info.py."""

    def test_scalar_values(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        demands = [
            {"uid": 1, "name": "d1", "fcost": 400.0},
            {"uid": 2, "name": "d2", "fcost": 600.0},
        ]
        assert _avg_demand_fcost(demands, None, ".") == pytest.approx(500.0)

    def test_list_values_uses_first(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        demands = [
            {"uid": 1, "name": "d1", "fcost": [300.0, 400.0]},
            {"uid": 2, "name": "d2", "fcost": [500.0, 600.0]},
        ]
        assert _avg_demand_fcost(demands, None, ".") == pytest.approx(400.0)

    def test_empty_demands(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        assert _avg_demand_fcost([], None, ".") == pytest.approx(0.0)

    def test_no_fcost_fields(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        demands = [{"uid": 1, "name": "d1"}, {"uid": 2, "name": "d2"}]
        assert _avg_demand_fcost(demands, None, ".") == pytest.approx(0.0)

    def test_mixed_scalar_and_missing(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        demands = [
            {"uid": 1, "name": "d1", "fcost": 400.0},
            {"uid": 2, "name": "d2"},  # no fcost
            {"uid": 3, "name": "d3", "fcost": 600.0},
        ]
        # Average of 400 and 600 only
        assert _avg_demand_fcost(demands, None, ".") == pytest.approx(500.0)

    def test_file_reference_without_base_dir(self) -> None:
        from gtopt_check_json._info import _avg_demand_fcost

        demands = [
            {"uid": 1, "name": "d1", "fcost": "fcost"},  # file ref
            {"uid": 2, "name": "d2", "fcost": 400.0},
        ]
        # File ref not resolved (no base_dir) → only scalar counted
        assert _avg_demand_fcost(demands, None, ".") == pytest.approx(400.0)

    def test_file_reference_with_parquet(self, tmp_path) -> None:
        import pandas as pd
        from gtopt_check_json._info import _avg_demand_fcost

        # Create Demand/fcost.parquet
        dem_dir = tmp_path / "Demand"
        dem_dir.mkdir()
        df = pd.DataFrame({"stage": [1, 2], "uid:1": [300.0, 500.0]})
        df.to_parquet(dem_dir / "fcost.parquet", index=False)

        demands = [
            {"uid": 1, "name": "d1", "fcost": "fcost"},
        ]
        # Resolves first value from parquet (300.0)
        result = _avg_demand_fcost(demands, str(tmp_path), ".")
        assert result == pytest.approx(300.0)


class TestComputeIndicatorsAvgFcost:
    """Test avg_fcost in compute_indicators."""

    def test_avg_fcost_from_scalar(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["demand_array"] = [
            {"uid": 1, "name": "d1", "bus": 1, "lmax": 50, "fcost": 400.0},
            {"uid": 2, "name": "d2", "bus": 2, "lmax": 30, "fcost": 600.0},
        ]
        ind = compute_indicators(case)
        assert ind.avg_fcost == pytest.approx(500.0)

    def test_avg_fcost_zero_when_no_fcost(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        ind = compute_indicators(case)
        assert ind.avg_fcost == pytest.approx(0.0)


class TestCheckCapacityAdequacy:
    """Test check_capacity_adequacy check."""

    def test_adequate_no_findings(self) -> None:
        # 350 MW gen vs 180 MW demand → ratio ~1.94 → no findings
        findings = check_capacity_adequacy(_INDICATOR_CASE)
        assert not findings

    def test_deficit_critical(self) -> None:
        """Capacity deficit should produce a CRITICAL finding."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        # Set generators to tiny capacity
        case["system"]["generator_array"] = [
            {"uid": 1, "name": "g1", "bus": 1, "pmax": 10, "capacity": 10, "gcost": 20},
        ]
        findings = check_capacity_adequacy(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.CRITICAL
        assert "deficit" in findings[0].message.lower()

    def test_thin_margin_warning(self) -> None:
        """Thin reserve margin (< 15%) should produce a WARNING."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        # Peak demand is 180; capacity needs to be < 1.15*180=207 but >= 180
        case["system"]["generator_array"] = [
            {
                "uid": 1,
                "name": "g1",
                "bus": 1,
                "pmax": 190,
                "capacity": 190,
                "gcost": 20,
            },
        ]
        findings = check_capacity_adequacy(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "margin" in findings[0].message.lower()

    def test_no_demand_no_findings(self) -> None:
        """No demand should produce no findings."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        case["system"]["demand_array"] = []
        findings = check_capacity_adequacy(case)
        assert not findings


# ── Cascade level validation ──────────────────────────────────────────────


class TestCascadeLevels:
    """Test check_cascade_levels."""

    def test_no_cascade_no_findings(self) -> None:
        assert not check_cascade_levels(_VALID_CASE)

    def test_valid_cascade(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse", "sddp_options": {"max_iterations": 10}},
                {
                    "name": "fine",
                    "transition": {"inherit_optimality_cuts": True},
                    "sddp_options": {
                        "max_iterations": 20,
                        "convergence_tol": 0.001,
                    },
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert not findings

    def test_missing_name_warning(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"sddp_options": {"max_iterations": 10}},
            ],
        }
        findings = check_cascade_levels(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "missing 'name'" in findings[0].message

    def test_level0_transition_warning(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {
                    "name": "first",
                    "transition": {"inherit_optimality_cuts": True},
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            "index 0" in f.message and "nothing to inherit" in f.message
            for f in findings
        )

    def test_target_penalty_nonpositive(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {
                    "name": "fine",
                    "transition": {
                        "inherit_targets": True,
                        "target_penalty": 0,
                    },
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            "target_penalty" in f.message and f.severity == Severity.WARNING
            for f in findings
        )

    def test_target_rtol_out_of_range(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {
                    "name": "fine",
                    "transition": {"target_rtol": 1.5},
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            "target_rtol" in f.message and f.severity == Severity.CRITICAL
            for f in findings
        )

    def test_target_rtol_zero(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {"name": "fine", "transition": {"target_rtol": 0}},
            ],
        }
        findings = check_cascade_levels(case)
        assert any("target_rtol" in f.message for f in findings)

    def test_target_rtol_valid_one(self) -> None:
        """target_rtol=1.0 is valid (edge of range)."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {"name": "fine", "transition": {"target_rtol": 1.0}},
            ],
        }
        findings = check_cascade_levels(case)
        assert not any("target_rtol" in f.message for f in findings)

    def test_both_inherit_cuts_and_targets_note(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {
                    "name": "fine",
                    "transition": {
                        "inherit_optimality_cuts": True,
                        "inherit_targets": True,
                    },
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            f.severity == Severity.NOTE
            and "inherit_optimality_cuts" in f.message
            and "inherit_targets" in f.message
            for f in findings
        )

    def test_negative_max_iterations_critical(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {
                    "name": "bad",
                    "sddp_options": {"max_iterations": -1},
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            "max_iterations" in f.message and f.severity == Severity.CRITICAL
            for f in findings
        )

    def test_convergence_tol_out_of_range(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [
                {
                    "name": "bad",
                    "sddp_options": {"convergence_tol": 1.5},
                },
            ],
        }
        findings = check_cascade_levels(case)
        assert any(
            "convergence_tol" in f.message and f.severity == Severity.CRITICAL
            for f in findings
        )


# ── Simulation mode checks ────────────────────────────────────────────────


class TestSimulationMode:
    """Test check_simulation_mode."""

    def test_no_simulation_mode_no_findings(self) -> None:
        assert not check_simulation_mode(_VALID_CASE)

    def test_simulation_mode_with_max_iterations(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "simulation_mode": True,
            "max_iterations": 10,
        }
        findings = check_simulation_mode(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "max_iterations" in findings[0].message

    def test_simulation_mode_with_save_per_iteration(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "simulation_mode": True,
            "save_per_iteration": True,
        }
        findings = check_simulation_mode(case)
        assert any("save_per_iteration" in f.message for f in findings)

    def test_simulation_mode_false_no_findings(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "simulation_mode": False,
            "max_iterations": 10,
        }
        assert not check_simulation_mode(case)

    def test_simulation_mode_max_iter_zero_ok(self) -> None:
        """simulation_mode=true with max_iterations=0 is consistent."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "simulation_mode": True,
            "max_iterations": 0,
        }
        findings = check_simulation_mode(case)
        assert not any("max_iterations" in f.message for f in findings)


# ── General SDDP checks ──────────────────────────────────────────────────


class TestSddpOptions:
    """Test check_sddp_options."""

    def test_no_sddp_options_no_findings(self) -> None:
        assert not check_sddp_options(_VALID_CASE)

    def test_min_gt_max_iterations(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "min_iterations": 50,
            "max_iterations": 10,
        }
        findings = check_sddp_options(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "min_iterations" in findings[0].message

    def test_max_iter_zero_no_hot_start(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "max_iterations": 0,
        }
        findings = check_sddp_options(case)
        assert any(
            "untrained" in f.message and f.severity == Severity.WARNING
            for f in findings
        )

    def test_max_iter_zero_with_hot_start_ok(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "max_iterations": 0,
            "hot_start": True,
        }
        findings = check_sddp_options(case)
        assert not any("untrained" in f.message for f in findings)

    def test_max_iter_zero_with_cuts_input_ok(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "max_iterations": 0,
            "cuts_input_file": "cuts/saved.csv",
        }
        findings = check_sddp_options(case)
        assert not any("untrained" in f.message for f in findings)

    def test_max_iter_zero_with_cut_recovery_mode_ok(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "max_iterations": 0,
            "cut_recovery_mode": "keep",
        }
        findings = check_sddp_options(case)
        assert not any("untrained" in f.message for f in findings)

    def test_convergence_tol_zero_critical(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "convergence_tol": 0,
        }
        findings = check_sddp_options(case)
        assert any(
            "convergence_tol" in f.message and f.severity == Severity.CRITICAL
            for f in findings
        )

    def test_convergence_tol_one_critical(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "convergence_tol": 1.0,
        }
        findings = check_sddp_options(case)
        assert any(
            "convergence_tol" in f.message and f.severity == Severity.CRITICAL
            for f in findings
        )

    def test_convergence_tol_negative_critical(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "convergence_tol": -0.01,
        }
        findings = check_sddp_options(case)
        assert any("convergence_tol" in f.message for f in findings)

    def test_convergence_tol_valid(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "convergence_tol": 0.001,
        }
        findings = check_sddp_options(case)
        assert not any("convergence_tol" in f.message for f in findings)

    def test_valid_sddp_options_no_findings(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "max_iterations": 100,
            "min_iterations": 2,
            "convergence_tol": 0.0001,
        }
        assert not check_sddp_options(case)


# ── Cascade + method consistency ──────────────────────────────────────────


class TestCascadeSolverType:
    """Test check_cascade_solver_type."""

    def test_no_cascade_no_findings(self) -> None:
        assert not check_cascade_solver_type(_VALID_CASE)

    def test_levels_with_wrong_method(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["method"] = "sddp"
        case["options"]["cascade_options"] = {
            "level_array": [
                {"name": "coarse"},
                {"name": "fine"},
            ],
        }
        findings = check_cascade_solver_type(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "sddp" in findings[0].message

    def test_cascade_method_no_levels_note(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["method"] = "cascade"
        case["options"]["cascade_options"] = {"level_array": []}
        findings = check_cascade_solver_type(case)
        assert len(findings) == 1
        assert findings[0].severity == Severity.NOTE
        assert "default level" in findings[0].message

    def test_cascade_method_with_levels_ok(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["method"] = "cascade"
        case["options"]["cascade_options"] = {
            "level_array": [{"name": "coarse"}, {"name": "fine"}],
        }
        assert not check_cascade_solver_type(case)

    def test_no_method_no_cascade_no_findings(self) -> None:
        """No method and no cascade → no findings."""
        assert not check_cascade_solver_type(_VALID_CASE)

    def test_levels_without_method_no_findings(self) -> None:
        """Levels present but method empty string → no finding."""
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["cascade_options"] = {
            "level_array": [{"name": "coarse"}],
        }
        # method not set at all → empty string → no warning
        findings = check_cascade_solver_type(case)
        assert not findings


class TestBoundaryCuts:
    """Test check_boundary_cuts."""

    def test_no_sddp_options_no_findings(self) -> None:
        assert not check_boundary_cuts(_VALID_CASE)

    def test_no_boundary_file_no_findings(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {}
        assert not check_boundary_cuts(case)

    def test_missing_file_warning(self, tmp_path: Path) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "nonexistent.csv",
        }
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "not found" in findings[0].message

    def test_all_known_state_vars_ok(self, tmp_path: Path) -> None:
        cuts_csv = tmp_path / "boundary.csv"
        cuts_csv.write_text("name,iteration,scene,rhs,RES_A,RES_B\n")

        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "boundary.csv",
        }
        case["system"]["junction_array"] = [
            {"uid": 1, "name": "RES_A"},
            {"uid": 2, "name": "RES_B"},
        ]
        case["system"]["reservoir_array"] = [
            {"uid": 1, "name": "RES_A", "junction": 1},
            {"uid": 2, "name": "RES_B", "junction": 2},
        ]
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert not findings

    def test_unknown_state_var_warning(self, tmp_path: Path) -> None:
        cuts_csv = tmp_path / "boundary.csv"
        cuts_csv.write_text("name,iteration,scene,rhs,RES_A,UNKNOWN\n")

        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "boundary.csv",
        }
        case["system"]["junction_array"] = [
            {"uid": 1, "name": "RES_A"},
        ]
        case["system"]["reservoir_array"] = [
            {"uid": 1, "name": "RES_A", "junction": 1},
        ]
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert len(findings) == 1
        assert findings[0].severity == Severity.WARNING
        assert "UNKNOWN" in findings[0].message

    def test_use_state_variable_false_excluded(self, tmp_path: Path) -> None:
        """Reservoir with use_state_variable=false is not a known state var."""
        cuts_csv = tmp_path / "boundary.csv"
        cuts_csv.write_text("name,iteration,scene,rhs,RES_A,RES_B\n")

        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "boundary.csv",
        }
        case["system"]["junction_array"] = [
            {"uid": 1, "name": "RES_A"},
            {"uid": 2, "name": "RES_B"},
        ]
        case["system"]["reservoir_array"] = [
            {"uid": 1, "name": "RES_A", "junction": 1},
            {
                "uid": 2,
                "name": "RES_B",
                "junction": 2,
                "use_state_variable": False,
            },
        ]
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert len(findings) == 1
        assert "RES_B" in findings[0].message

    def test_battery_state_var(self, tmp_path: Path) -> None:
        """Batteries are always state variables."""
        cuts_csv = tmp_path / "boundary.csv"
        cuts_csv.write_text("name,iteration,scene,rhs,BAT1\n")

        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "boundary.csv",
        }
        case["system"]["battery_array"] = [
            {"uid": 1, "name": "BAT1", "bus": 1},
        ]
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert not findings

    def test_no_state_vars_in_model(self, tmp_path: Path) -> None:
        """CSV has state var columns but model has none."""
        cuts_csv = tmp_path / "boundary.csv"
        cuts_csv.write_text("name,iteration,scene,rhs,GHOST\n")

        case = json.loads(json.dumps(_VALID_CASE))
        case["options"]["sddp_options"] = {
            "boundary_cuts_file": "boundary.csv",
        }
        findings = check_boundary_cuts(case, base_dir=str(tmp_path))
        assert len(findings) == 1
        assert "no state variables" in findings[0].message
