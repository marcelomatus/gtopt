# SPDX-License-Identifier: BSD-3-Clause
"""Tests for pre-flight checks."""

import json
from pathlib import Path

from run_gtopt._checks import (
    check_input_files,
    check_json_syntax,
    check_output_directory,
)


def _write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


def test_check_json_syntax_ok(tmp_path: Path):
    """Valid JSON passes."""
    p = tmp_path / "plan.json"
    _write_json(p, {"system": {}, "simulation": {}})
    result = check_json_syntax(p)
    assert result.passed
    assert not result.errors


def test_check_json_syntax_invalid(tmp_path: Path):
    """Invalid JSON returns error."""
    p = tmp_path / "bad.json"
    p.write_text("{ invalid }")
    result = check_json_syntax(p)
    assert not result.passed
    assert any("syntax error" in e.lower() for e in result.errors)


def test_check_json_syntax_empty(tmp_path: Path):
    """Empty file is an error."""
    p = tmp_path / "empty.json"
    p.write_text("")
    result = check_json_syntax(p)
    assert not result.passed
    assert any("empty" in e.lower() for e in result.errors)


def test_check_json_syntax_missing(tmp_path: Path):
    """Non-existent file is an error."""
    result = check_json_syntax(tmp_path / "nope.json")
    assert not result.passed
    assert any("does not exist" in e for e in result.errors)


def test_check_json_syntax_not_object(tmp_path: Path):
    """JSON array root is an error."""
    p = tmp_path / "array.json"
    p.write_text("[1, 2, 3]")
    result = check_json_syntax(p)
    assert not result.passed
    assert any("object" in e.lower() for e in result.errors)


def test_check_input_files_ok(tmp_path: Path):
    """No issues when input dir and files exist."""
    case = tmp_path / "case"
    case.mkdir()
    input_dir = case / "input"
    input_dir.mkdir()
    (input_dir / "Generator").mkdir()
    (input_dir / "Generator" / "pmax.parquet").write_text("")

    plan = {
        "options": {"input_directory": "input", "input_format": "parquet"},
        "system": {"generator_array": [{"uid": 1, "name": "g1", "pmax": "pmax"}]},
    }
    json_path = case / "case.json"
    _write_json(json_path, plan)
    result = check_input_files(json_path)
    assert result.passed
    assert not result.warnings


def test_check_input_files_missing_dir(tmp_path: Path):
    """Missing input_directory is reported."""
    plan = {"options": {"input_directory": "no_such_dir"}, "system": {}}
    json_path = tmp_path / "plan.json"
    _write_json(json_path, plan)
    result = check_input_files(json_path)
    assert any("does not exist" in w for w in result.warnings)


def test_check_output_directory_ok(tmp_path: Path):
    """Writable output directory passes."""
    plan = {"options": {"output_directory": str(tmp_path / "results")}}
    json_path = tmp_path / "plan.json"
    _write_json(json_path, plan)
    result = check_output_directory(json_path)
    assert result.passed


def test_check_output_directory_bad_parent(tmp_path: Path):
    """Non-existent parent is an error."""
    plan = {"options": {"output_directory": "/no/such/parent/results"}}
    json_path = tmp_path / "plan.json"
    _write_json(json_path, plan)
    result = check_output_directory(json_path)
    assert not result.passed
    assert any("does not exist" in e for e in result.errors)
