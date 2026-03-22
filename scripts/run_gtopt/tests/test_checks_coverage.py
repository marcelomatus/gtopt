# SPDX-License-Identifier: BSD-3-Clause
"""Additional tests for run_gtopt._checks to improve coverage."""

import json
from pathlib import Path

from run_gtopt._checks import (
    available_checks,
    check_input_files,
    check_json_syntax,
    check_output_directory,
    run_preflight_checks,
)


def _write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


def test_json_syntax_warns_missing_sections(tmp_path: Path):
    """Missing system/simulation sections produce warnings, not errors."""
    p = tmp_path / "plan.json"
    _write_json(p, {"options": {}})
    result = check_json_syntax(p)
    assert result.passed  # passed but with warnings
    assert any("system" in w for w in result.warnings)


def test_input_files_missing_file_reference(tmp_path: Path):
    """Missing file-referenced data produces a warning."""
    case = tmp_path / "case"
    case.mkdir()
    input_dir = case / "input"
    input_dir.mkdir()
    (input_dir / "Generator").mkdir()

    plan = {
        "options": {"input_directory": "input", "input_format": "parquet"},
        "system": {"generator_array": [{"uid": 1, "name": "g1", "pmax": "pmax"}]},
    }
    json_path = case / "case.json"
    _write_json(json_path, plan)
    result = check_input_files(json_path)
    assert any("missing" in w.lower() for w in result.warnings)


def test_input_files_skips_cross_reference_fields(tmp_path: Path):
    """Cross-reference fields are not treated as file references."""
    case = tmp_path / "case"
    case.mkdir()
    (case / "input").mkdir()

    plan = {
        "options": {"input_directory": "input"},
        "system": {
            "turbine_array": [
                {
                    "uid": 1,
                    "name": "t1",
                    "generator": "g1",
                    "flow": "f1",
                    "junction": "j1",
                }
            ]
        },
    }
    json_path = case / "case.json"
    _write_json(json_path, plan)
    result = check_input_files(json_path)
    assert not any("generator" in w.lower() for w in result.warnings)


def test_input_files_cross_class_reference(tmp_path: Path):
    """Cross-class file reference (OtherClass@field) is handled."""
    case = tmp_path / "case"
    case.mkdir()
    input_dir = case / "input"
    input_dir.mkdir()
    (input_dir / "Flow").mkdir()
    (input_dir / "Flow" / "discharge.parquet").write_text("")

    plan = {
        "options": {"input_directory": "input"},
        "system": {
            "generator_profile_array": [
                {
                    "uid": 1,
                    "name": "gp1",
                    "generator": "g1",
                    "profile": "Flow@discharge",
                }
            ]
        },
    }
    json_path = case / "case.json"
    _write_json(json_path, plan)
    result = check_input_files(json_path)
    assert not any("missing" in w.lower() for w in result.warnings)


def test_output_directory_existing_writable(tmp_path: Path):
    """Existing writable output dir passes."""
    out = tmp_path / "results"
    out.mkdir()
    plan = {"options": {"output_directory": str(out)}}
    json_path = tmp_path / "plan.json"
    _write_json(json_path, plan)
    result = check_output_directory(json_path)
    assert result.passed


def test_preflight_checks_bad_json(tmp_path: Path):
    """Preflight fails immediately on bad JSON syntax."""
    p = tmp_path / "bad.json"
    p.write_text("{ invalid }")
    ok = run_preflight_checks(p)
    assert not ok


def test_preflight_checks_good_json(tmp_path: Path):
    """Preflight passes on valid minimal JSON."""
    case = tmp_path / "case"
    case.mkdir()
    plan = {
        "options": {"input_directory": ".", "output_directory": str(case / "results")},
        "system": {},
        "simulation": {},
    }
    json_path = case / "plan.json"
    _write_json(json_path, plan)
    ok = run_preflight_checks(json_path)
    assert ok


def test_preflight_strict_mode(tmp_path: Path):
    """Strict mode aborts on warnings."""
    case = tmp_path / "case"
    case.mkdir()
    (case / "input").mkdir()
    (case / "input" / "Generator").mkdir()

    plan = {
        "options": {"input_directory": "input"},
        "system": {"generator_array": [{"uid": 1, "name": "g1", "pmax": "pmax"}]},
        "simulation": {},
    }
    json_path = case / "plan.json"
    _write_json(json_path, plan)
    ok = run_preflight_checks(json_path, strict=True)
    assert not ok


def test_preflight_with_enabled_filter(tmp_path: Path):
    """Only enabled checks run."""
    p = tmp_path / "plan.json"
    _write_json(p, {"system": {}, "simulation": {}, "options": {}})
    ok = run_preflight_checks(p, enabled={"json_syntax"})
    assert ok


def test_available_checks():
    """available_checks returns pre and post check names."""
    checks = available_checks()
    assert "pre" in checks
    assert "post" in checks
    assert "json_syntax" in checks["pre"]
    assert "check_lp" in checks["post"]
