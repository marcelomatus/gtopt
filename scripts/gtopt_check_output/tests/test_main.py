# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_output CLI entry point."""

import json
from pathlib import Path

import pandas as pd
import pytest

from gtopt_check_output.main import _resolve_paths, main, make_parser


def _write_csv(path: Path, df: pd.DataFrame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(path, index=False)


def _make_case(tmp_path: Path) -> Path:
    """Create a minimal case directory with results and JSON."""
    case = tmp_path / "my_case"
    case.mkdir()

    # Planning JSON
    plan = {
        "options": {},
        "system": {
            "generator_array": [
                {"uid": 1, "name": "g1", "type": "thermal", "pmax": 100.0}
            ],
            "demand_array": [{"uid": 1, "name": "d1"}],
        },
        "simulation": {"block_array": [{"uid": 1, "duration": 24.0}]},
    }
    (case / "my_case.json").write_text(json.dumps(plan))

    # Results
    results = case / "results"
    _write_csv(
        results / "solution.csv",
        pd.DataFrame({"status": [0], "objective": [500.0]}),
    )
    _write_csv(
        results / "Generator" / "generation_sol.csv",
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [80.0]}),
    )
    _write_csv(
        results / "Bus" / "balance_dual.csv",
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [25.0]}),
    )
    _write_csv(
        results / "Demand" / "load_sol.csv",
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [80.0]}),
    )
    _write_csv(
        results / "Demand" / "fail_sol.csv",
        pd.DataFrame({"scenario": [1], "stage": [1], "block": [1], "uid:1": [0.0]}),
    )

    return case


def test_parser_defaults():
    """Parser creates valid defaults."""
    p = make_parser()
    args = p.parse_args([])
    assert args.case_dir is None
    assert args.quiet is False
    assert args.no_color is False


def test_resolve_paths_case_dir(tmp_path: Path):
    """_resolve_paths finds results/ and JSON in case dir."""
    case = _make_case(tmp_path)
    p = make_parser()
    args = p.parse_args([str(case)])
    results_dir, json_path = _resolve_paths(args)
    assert results_dir == case / "results"
    assert json_path.is_file()


def test_resolve_paths_planning_json_in_results(tmp_path: Path):
    """Prefers planning.json from results directory."""
    case = _make_case(tmp_path)
    # Write planning.json to results/
    plan = {"options": {}, "system": {}, "simulation": {}}
    (case / "results" / "planning.json").write_text(json.dumps(plan))

    p = make_parser()
    args = p.parse_args([str(case)])
    _, json_path = _resolve_paths(args)
    assert json_path.name == "planning.json"
    assert json_path.parent.name == "results"


def test_main_runs_on_case(tmp_path: Path):
    """main() completes without error on valid case."""
    case = _make_case(tmp_path)
    # Should exit 0 (no critical findings)
    with pytest.raises(SystemExit) as exc_info:
        main([str(case), "--no-color"])
    assert exc_info.value.code == 0


def test_main_quiet_mode(tmp_path: Path, capsys):
    """--quiet suppresses INFO findings."""
    case = _make_case(tmp_path)
    with pytest.raises(SystemExit):
        main([str(case), "--quiet", "--no-color"])
    captured = capsys.readouterr()
    # Should not show section headers
    assert "--- expected_files ---" not in captured.out


def test_main_missing_results(tmp_path: Path):
    """Exits with code 2 when results dir is missing."""
    case = tmp_path / "empty"
    case.mkdir()
    with pytest.raises(SystemExit) as exc_info:
        main([str(case)])
    assert exc_info.value.code == 2


def test_main_missing_json(tmp_path: Path):
    """Exits with code 2 when JSON is missing."""
    case = tmp_path / "nojson"
    results = case / "results"
    results.mkdir(parents=True)
    _write_csv(results / "solution.csv", pd.DataFrame({"status": [0]}))
    with pytest.raises(SystemExit) as exc_info:
        main([str(case)])
    assert exc_info.value.code == 2
