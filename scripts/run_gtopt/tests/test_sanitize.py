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


def test_fix_negative_elastic_penalty(tmp_path: Path):
    """Negative elastic_penalty is fixed to 1e6."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"elastic_penalty": -100}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["elastic_penalty"] == 1e6


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
    """Invalid method produces a warning."""
    p = _write_plan(tmp_path / "plan.json", {"method": "gurobi"})
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
        {"sddp_options": {"cut_recovery_mode": "invalid"}},
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


def test_fix_cascade_level_negative_max_iterations(tmp_path: Path):
    """Negative solver.max_iterations in a cascade level is fixed to 20."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"solver": {"max_iterations": -1}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["solver"]["max_iterations"]
        == 20
    )


def test_fix_cascade_level_negative_convergence_tol(tmp_path: Path):
    """Negative solver.convergence_tol in a cascade level is fixed to 0.01."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"solver": {"convergence_tol": -0.5}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["solver"]["convergence_tol"]
        == 0.01
    )


def test_fix_cascade_level_negative_target_rtol(tmp_path: Path):
    """Negative transition.target_rtol is fixed to 0.05."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"transition": {"target_rtol": -0.1}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["transition"]["target_rtol"]
        == 0.05
    )


def test_fix_cascade_level_negative_target_min_atol(tmp_path: Path):
    """Negative transition.target_min_atol is fixed to 1.0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"transition": {"target_min_atol": -2.0}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["transition"]["target_min_atol"]
        == 1.0
    )


def test_fix_cascade_level_negative_target_penalty(tmp_path: Path):
    """Negative transition.target_penalty is fixed to 500.0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"transition": {"target_penalty": -100}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["transition"]["target_penalty"]
        == 500.0
    )


def test_fix_cascade_level_negative_num_apertures(tmp_path: Path):
    """Negative solver.num_apertures is fixed to 0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"solver": {"num_apertures": -3}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert (
        data["options"]["cascade_options"]["levels"][0]["solver"]["num_apertures"] == 0
    )


def test_valid_cascade_levels_pass_through(tmp_path: Path):
    """Valid cascade levels are not modified."""
    level = {
        "name": "uninodal_benders",
        "model_options": {"use_single_bus": True, "use_kirchhoff": False},
        "sddp_options": {
            "max_iterations": 20,
            "convergence_tol": 0.01,
        },
        "transition": {
            "inherit_optimality_cuts": False,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500.0,
        },
    }
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [level]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    out_level = data["options"]["cascade_options"]["levels"][0]
    assert out_level["sddp_options"]["max_iterations"] == 20
    assert out_level["transition"]["target_rtol"] == 0.05
    assert out_level["transition"]["target_penalty"] == 500.0


def test_cascade_multiple_levels_validated(tmp_path: Path):
    """Validation applies to each level independently."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "cascade_options": {
                "levels": [
                    {"solver": {"max_iterations": 10}},
                    {"solver": {"max_iterations": -5}},
                ]
            }
        },
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    levels = data["options"]["cascade_options"]["levels"]
    assert levels[0]["solver"]["max_iterations"] == 10
    assert levels[1]["solver"]["max_iterations"] == 20


def test_cascade_solver_type_accepted(tmp_path: Path):
    """method 'cascade' passes validation."""
    p = _write_plan(tmp_path / "plan.json", {"method": "cascade"})
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["method"] == "cascade"


def test_output_directory_override(tmp_path: Path):
    """output_directory is overridden when provided."""
    p = _write_plan(tmp_path / "plan.json", {"output_directory": "old"})
    result = sanitize_json(p, output_directory="/new/output")
    data = json.loads(result.read_text())
    assert data["options"]["output_directory"] == "/new/output"


# ---------------------------------------------------------------------------
# New cascade level structure (sddp_options / model_options)
# ---------------------------------------------------------------------------


def test_fix_cascade_sddp_options_negative_max_iterations(tmp_path: Path):
    """Negative sddp_options.max_iterations in a cascade level is fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"sddp_options": {"max_iterations": -1}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    lvl = data["options"]["cascade_options"]["levels"][0]
    assert lvl["sddp_options"]["max_iterations"] == 20


def test_fix_cascade_sddp_options_negative_min_iterations(tmp_path: Path):
    """Negative sddp_options.min_iterations in a cascade level is fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"sddp_options": {"min_iterations": -3}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    lvl = data["options"]["cascade_options"]["levels"][0]
    assert lvl["sddp_options"]["min_iterations"] == 0


def test_fix_cascade_sddp_options_negative_convergence_tol(tmp_path: Path):
    """Negative sddp_options.convergence_tol in a cascade level is fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [{"sddp_options": {"convergence_tol": -0.5}}]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    lvl = data["options"]["cascade_options"]["levels"][0]
    assert lvl["sddp_options"]["convergence_tol"] == 0.01


def test_fix_cascade_model_options_kirchhoff_single_bus(tmp_path: Path):
    """Kirchhoff + single-bus conflict in cascade model_options is resolved."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "cascade_options": {
                "levels": [
                    {
                        "model_options": {
                            "use_single_bus": True,
                            "use_kirchhoff": True,
                        }
                    }
                ]
            }
        },
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    mo = data["options"]["cascade_options"]["levels"][0]["model_options"]
    assert mo["use_kirchhoff"] is False
    assert mo["use_single_bus"] is True


def test_fix_cascade_transition_negative_optimality_dual_threshold(tmp_path: Path):
    """Negative optimality_dual_threshold in transition is fixed to 0.0."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "cascade_options": {
                "levels": [{"transition": {"optimality_dual_threshold": -0.01}}]
            }
        },
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    tr = data["options"]["cascade_options"]["levels"][0]["transition"]
    assert tr["optimality_dual_threshold"] == 0.0


def test_cascade_level_array_key_accepted(tmp_path: Path):
    """The legacy level_array key is also accepted."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "cascade_options": {
                "level_array": [{"sddp_options": {"max_iterations": -1}}]
            }
        },
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    lvl = data["options"]["cascade_options"]["level_array"][0]
    assert lvl["sddp_options"]["max_iterations"] == 20


def test_cascade_new_structure_full_level(tmp_path: Path):
    """A full new-structure level with model_options, sddp_options, transition."""
    level = {
        "name": "fast_benders",
        "model_options": {"use_single_bus": True, "use_kirchhoff": False},
        "sddp_options": {
            "max_iterations": 20,
            "convergence_tol": 0.01,
        },
        "transition": {
            "inherit_optimality_cuts": True,
            "inherit_targets": False,
            "target_penalty": 500.0,
            "optimality_dual_threshold": 0.0,
        },
    }
    p = _write_plan(
        tmp_path / "plan.json",
        {"cascade_options": {"levels": [level]}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    out = data["options"]["cascade_options"]["levels"][0]
    assert out["sddp_options"]["max_iterations"] == 20
    assert out["model_options"]["use_kirchhoff"] is False
    assert out["transition"]["target_penalty"] == 500.0
    assert out["transition"]["optimality_dual_threshold"] == 0.0


# ---------------------------------------------------------------------------
# simulation_mode in sddp_options
# ---------------------------------------------------------------------------


def test_sddp_simulation_mode_bool_accepted(tmp_path: Path):
    """Boolean simulation_mode passes through without modification."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"simulation_mode": True}},
    )
    result = sanitize_json(p)
    # No FIX applied, original returned
    assert result == p


def test_sddp_simulation_mode_false_accepted(tmp_path: Path):
    """simulation_mode=false passes through without modification."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"simulation_mode": False}},
    )
    result = sanitize_json(p)
    assert result == p


def test_fix_sddp_simulation_mode_non_bool(tmp_path: Path):
    """Non-boolean simulation_mode is coerced to bool."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"sddp_options": {"simulation_mode": 1}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["sddp_options"]["simulation_mode"] is True


# ---------------------------------------------------------------------------
# model_options sub-dict (new location for model fields)
# ---------------------------------------------------------------------------


def test_model_options_scale_objective_fix(tmp_path: Path):
    """scale_objective in model_options is validated and fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"model_options": {"scale_objective": -5}},
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["model_options"]["scale_objective"] == 1000


def test_model_options_preferred_over_flat(tmp_path: Path):
    """model_options value takes precedence over flat (deprecated) value."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "scale_objective": 500,
            "model_options": {"scale_objective": -1},
        },
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    # Both locations updated to the fixed value
    assert data["options"]["model_options"]["scale_objective"] == 1000
    assert data["options"]["scale_objective"] == 1000


def test_model_options_kirchhoff_single_bus_conflict(tmp_path: Path):
    """Kirchhoff + single-bus conflict in model_options is resolved."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"model_options": {"use_kirchhoff": True, "use_single_bus": True}},
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["model_options"]["use_kirchhoff"] is False
    assert data["options"]["model_options"]["use_single_bus"] is True


def test_model_options_discount_rate_fix(tmp_path: Path):
    """annual_discount_rate in model_options is validated and fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"model_options": {"annual_discount_rate": -2.0}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["model_options"]["annual_discount_rate"] == 0.0


def test_model_options_scale_theta_fix(tmp_path: Path):
    """scale_theta in model_options is validated and fixed."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"model_options": {"scale_theta": 0}},
    )
    result = sanitize_json(p)
    data = json.loads(result.read_text())
    assert data["options"]["model_options"]["scale_theta"] == 1000


def test_model_options_demand_fail_cost_warn(tmp_path: Path):
    """demand_fail_cost=0 in model_options produces a warning but no fix."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"model_options": {"demand_fail_cost": 0}},
    )
    result = sanitize_json(p)
    # Warn-only: original path returned (no FIX applied)
    assert result == p


def test_model_options_valid_pass_through(tmp_path: Path):
    """Valid model_options values are not modified."""
    p = _write_plan(
        tmp_path / "plan.json",
        {
            "model_options": {
                "scale_objective": 1000,
                "use_kirchhoff": True,
                "use_single_bus": False,
                "annual_discount_rate": 0.1,
            }
        },
    )
    result = sanitize_json(p)
    assert result == p


def test_flat_fields_still_work(tmp_path: Path):
    """Flat (deprecated) fields still validated when no model_options."""
    p = _write_plan(
        tmp_path / "plan.json",
        {"scale_objective": -5, "scale_theta": -1},
    )
    result = sanitize_json(p)
    assert result is not None
    data = json.loads(result.read_text())
    assert data["options"]["scale_objective"] == 1000
    assert data["options"]["scale_theta"] == 1000
