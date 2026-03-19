# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_json."""

import json
from pathlib import Path
from typing import Any

import pytest

from gtopt_check_json._checks import (
    Severity,
    check_affluent_nonneg,
    check_bus_connectivity,
    check_capacity_adequacy,
    check_demand_lmax_nonneg,
    check_element_references,
    check_name_uniqueness,
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
        assert "Island" in findings[0].message

    def test_single_bus_no_findings(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"] = [{"uid": 1, "name": "b1"}]
        case["system"]["line_array"] = []
        assert not check_bus_connectivity(case)


# ── Unreferenced elements ───────────────────────────────────────────────────


class TestUnreferencedElements:
    """Test check_unreferenced_elements."""

    def test_valid_no_findings(self) -> None:
        findings = check_unreferenced_elements(_VALID_CASE)
        assert not findings

    def test_unreferenced_bus(self) -> None:
        case = json.loads(json.dumps(_VALID_CASE))
        case["system"]["bus_array"].append({"uid": 3, "name": "b3_orphan"})
        findings = check_unreferenced_elements(case)
        assert len(findings) >= 1
        assert any("Bus" in f.message and "b3_orphan" in f.message for f in findings)


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
        assert p.name == ".gtopt_check_json.conf"

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
        out = capsys.readouterr().out
        assert "test_valid" in out

    def test_check_valid(self, tmp_path: Path, capsys: Any) -> None:
        p = tmp_path / "test.json"
        p.write_text(json.dumps(_VALID_CASE), encoding="utf-8")
        rc = main(["--no-color", str(p)])
        assert rc == 0
        out = capsys.readouterr().out
        assert "passed" in out.lower() or "0 critical" in out.lower()

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

    def test_scalar_lmax(self) -> None:
        """Scalar lmax should be treated as constant across all blocks."""
        case = json.loads(json.dumps(_VALID_CASE))
        ind = compute_indicators(case)
        # d1 has lmax=50 (scalar), 2 blocks
        assert ind.peak_demand_mw == pytest.approx(50.0)
        assert ind.total_demand_by_block == [50.0, 50.0]

    def test_failure_gen_excluded(self) -> None:
        """Generators with pmax >= 9000 (PLP failure) should be excluded."""
        case = json.loads(json.dumps(_INDICATOR_CASE))
        case["system"]["generator_array"].append(
            {"uid": 99, "name": "failure", "bus": 1, "pmax": 9999, "gcost": 9999}
        )
        ind = compute_indicators(case)
        # Should still be 350, not 350 + 9999
        assert ind.total_gen_capacity_mw == pytest.approx(350.0)
        assert ind.num_generators == 2  # failure gen not counted

    def test_empty_case(self) -> None:
        """Empty planning dict should produce zero indicators."""
        ind = compute_indicators({})
        assert ind.total_gen_capacity_mw == 0.0
        assert ind.peak_demand_mw == 0.0
        assert ind.capacity_adequacy_ratio == float("inf")


class TestFormatIndicators:
    """Test format_indicators function."""

    def test_contains_capacity(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "350.0 MW" in text
        assert "gen capacity" in text.lower()

    def test_contains_peak_demand(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "180.0 MW" in text
        assert "peak demand" in text.lower()

    def test_contains_energy(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "1,440.0 MWh" in text

    def test_contains_adequacy(self) -> None:
        text = format_indicators(_INDICATOR_CASE)
        assert "adequacy" in text.lower()


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
