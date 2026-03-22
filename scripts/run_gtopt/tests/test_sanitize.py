# SPDX-License-Identifier: BSD-3-Clause
"""Tests for JSON sanitization."""

import json
from pathlib import Path

from run_gtopt._sanitize import sanitize_json


def _write_plan(path: Path, opts: dict | None = None) -> Path:
    data = {"options": opts or {}, "system": {}, "simulation": {}}
    path.write_text(json.dumps(data))
    return path


def test_no_change_returns_original(tmp_path: Path):
    """When nothing needs fixing, the original path is returned."""
    p = _write_plan(tmp_path / "plan.json")
    result = sanitize_json(p)
    assert result == p


def test_compression_override(tmp_path: Path):
    """Compression mismatch produces a sanitized file."""
    p = _write_plan(tmp_path / "plan.json", {"output_compression": "zstd"})
    result = sanitize_json(p, compression="gzip")
    assert result is not None
    assert result != p
    data = json.loads(result.read_text())
    assert data["options"]["output_compression"] == "gzip"


def test_threads_override(tmp_path: Path):
    """Thread count is written to solver_options."""
    p = _write_plan(tmp_path / "plan.json")
    result = sanitize_json(p, threads=8)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["solver_options"]["threads"] == 8


def test_export_path(tmp_path: Path):
    """--export-json writes to the specified path."""
    p = _write_plan(tmp_path / "plan.json", {"output_compression": "zstd"})
    export = tmp_path / "exported.json"
    result = sanitize_json(p, compression="gzip", export_path=export)
    assert result == export
    assert export.is_file()


def test_fix_bad_scale_objective(tmp_path: Path):
    """scale_objective <= 0 is fixed to 1000."""
    p = _write_plan(tmp_path / "plan.json", {"scale_objective": -5})
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["scale_objective"] == 1000


def test_fix_kirchhoff_single_bus(tmp_path: Path):
    """use_kirchhoff + use_single_bus conflict is resolved."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"use_kirchhoff": True, "use_single_bus": True},
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["use_kirchhoff"] is False


def test_fix_sddp_min_gt_max(tmp_path: Path):
    """SDDP min_iterations > max_iterations is clamped."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"min_iterations": 200, "max_iterations": 50}},
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    sddp = data["options"]["sddp_options"]
    assert sddp["min_iterations"] <= sddp["max_iterations"]


def test_fix_negative_convergence_tol(tmp_path: Path):
    """Negative convergence_tol is fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"convergence_tol": -0.01}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["convergence_tol"] > 0


def test_fix_negative_discount_rate(tmp_path: Path):
    """annual_discount_rate <= -1 is fixed to 0."""
    p = _write_plan(tmp_path / "plan.json", {"annual_discount_rate": -2.0})
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["annual_discount_rate"] == 0.0


def test_fix_bad_solver_algorithm(tmp_path: Path):
    """Invalid solver algorithm is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"solver_options": {"algorithm": 99}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["solver_options"]["algorithm"] == 0


def test_fix_sddp_alpha_swap(tmp_path: Path):
    """alpha_min > alpha_max is swapped."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"alpha_min": 1e12, "alpha_max": 0.0}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    sddp = data["options"]["sddp_options"]
    assert sddp["alpha_min"] <= sddp["alpha_max"]


def test_fix_negative_elastic_penalty(tmp_path: Path):
    """Negative elastic_penalty is fixed to 1000."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"elastic_penalty": -100}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["elastic_penalty"] == 1000


def test_fix_negative_max_cuts_per_phase(tmp_path: Path):
    """Negative max_cuts_per_phase is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"max_cuts_per_phase": -5}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["max_cuts_per_phase"] == 0


def test_fix_zero_cut_prune_interval(tmp_path: Path):
    """Zero cut_prune_interval is fixed to 10."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"cut_prune_interval": 0}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["cut_prune_interval"] == 10


def test_fix_negative_prune_dual_threshold(tmp_path: Path):
    """Negative prune_dual_threshold is fixed to 1e-8."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"prune_dual_threshold": -0.01}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["prune_dual_threshold"] == 1e-8


def test_input_directory_override(tmp_path: Path):
    """input_directory is overridden when provided."""
    p = _write_plan(tmp_path / "plan.json", {"input_directory": "old"})
    result = sanitize_json(p, input_directory="/new/input")
    data = json.loads(result.read_text())
    assert data["options"]["input_directory"] == "/new/input"


def test_corrupt_json_returns_none(tmp_path: Path):
    """Unreadable JSON returns None."""
    p = tmp_path / "bad.json"
    p.write_text("{ not json }")
    result = sanitize_json(p)
    assert result is None


def test_scale_theta_fix(tmp_path: Path):
    """scale_theta <= 0 is fixed to 1000."""
    p = _write_plan(tmp_path / "plan.json", {"scale_theta": 0})
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["scale_theta"] == 1000


def test_warn_demand_fail_cost_zero(tmp_path: Path):
    """demand_fail_cost=0 produces a warning but no fix."""
    p = _write_plan(tmp_path / "plan.json", {"demand_fail_cost": 0})
    result = sanitize_json(p)
    # Warn-only: original path returned (no FIX applied)
    assert result == p


def test_warn_large_scale_objective(tmp_path: Path):
    """Very large scale_objective produces a warning but no fix."""
    p = _write_plan(tmp_path / "plan.json", {"scale_objective": 1e15})
    result = sanitize_json(p)
    assert result == p


def test_warn_high_discount_rate(tmp_path: Path):
    """Discount rate > 50% produces a warning but no fix."""
    p = _write_plan(tmp_path / "plan.json", {"annual_discount_rate": 0.9})
    result = sanitize_json(p)
    assert result == p


def test_warn_invalid_format(tmp_path: Path):
    """Invalid input_format produces a warning."""
    p = _write_plan(tmp_path / "plan.json", {"input_format": "xml"})
    result = sanitize_json(p)
    assert result == p


def test_warn_invalid_solver_type(tmp_path: Path):
    """Invalid solver_type produces a warning."""
    p = _write_plan(tmp_path / "plan.json", {"solver_type": "gurobi"})
    result = sanitize_json(p)
    assert result == p


def test_fix_lp_names_invalid(tmp_path: Path):
    """Invalid use_lp_names is fixed to 1."""
    p = _write_plan(tmp_path / "plan.json", {"use_lp_names": 5})
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["use_lp_names"] == 1


def test_fix_negative_solver_threads(tmp_path: Path):
    """Negative solver threads are fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"solver_options": {"threads": -1}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["solver_options"]["threads"] == 0


def test_fix_negative_eps(tmp_path: Path):
    """Negative solver epsilon is removed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"solver_options": {"optimal_eps": -0.01}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert "optimal_eps" not in data["options"]["solver_options"]


def test_warn_loose_eps(tmp_path: Path):
    """Very loose epsilon produces a warning but no fix."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"solver_options": {"feasible_eps": 0.1}},
    )
    result = sanitize_json(p)
    assert result == p


def test_warn_deprecated_solver_field(tmp_path: Path):
    """Deprecated top-level solver field with solver_options produces warning."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"lp_threads": 4, "solver_options": {"threads": 2}},
    )
    result = sanitize_json(p)
    assert result == p


def test_fix_sddp_max_iterations_zero(tmp_path: Path):
    """SDDP max_iterations <= 0 is fixed to 100."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"max_iterations": 0}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["max_iterations"] == 100


def test_fix_sddp_min_iterations_negative(tmp_path: Path):
    """SDDP negative min_iterations is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"min_iterations": -5}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["min_iterations"] == 0


def test_warn_sddp_invalid_mode(tmp_path: Path):
    """Invalid SDDP mode values produce warnings."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"hot_start_mode": "invalid"}},
    )
    result = sanitize_json(p)
    assert result == p


def test_fix_sddp_aperture_timeout_negative(tmp_path: Path):
    """Negative aperture_timeout is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"aperture_timeout": -10}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["aperture_timeout"] == 0


def test_fix_negative_max_stored_cuts(tmp_path: Path):
    """Negative max_stored_cuts is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"max_stored_cuts": -3}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["max_stored_cuts"] == 0


def test_output_directory_override(tmp_path: Path):
    """output_directory is overridden when provided."""
    p = _write_plan(tmp_path / "plan.json", {"output_directory": "old"})
    result = sanitize_json(p, output_directory="/new/output")
    data = json.loads(result.read_text())
    assert data["options"]["output_directory"] == "/new/output"
