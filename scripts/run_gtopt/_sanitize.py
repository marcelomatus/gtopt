# SPDX-License-Identifier: BSD-3-Clause
"""Build a sanitized planning JSON with runtime fixes and validations.

The sanitized JSON incorporates adjustments detected at launch time
(compression codec fallback, thread count, option corrections) so that
the gtopt binary receives a single, self-consistent input file.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Option validation and auto-fix
# ---------------------------------------------------------------------------

_VALID_SOLVER_TYPES = {"monolithic", "sddp", "cascade"}
_VALID_INPUT_FORMATS = {"parquet", "csv"}
_VALID_OUTPUT_FORMATS = {"parquet", "csv"}
_VALID_LP_ALGORITHMS = {0, 1, 2, 3}
_VALID_CUT_RECOVERY_MODES = {"none", "keep", "append", "replace"}
_VALID_RECOVERY_MODES = {"none", "cuts", "full"}
_VALID_CUT_SHARING_MODES = {"none", "expected", "accumulate", "max"}
_VALID_ELASTIC_MODES = {"single_cut", "multi_cut", "backpropagate", "cut"}
_VALID_BOUNDARY_MODES = {"noload", "separated", "combined"}


def _get_model_opt(opts: dict, key: str, default: Any = None) -> Any:
    """Get a model option from model_options sub-dict or flat (deprecated).

    Prefers the ``model_options`` sub-dict value when both locations exist.
    """
    mo = opts.get("model_options", {})
    if isinstance(mo, dict) and key in mo:
        return mo[key]
    return opts.get(key, default)


def _set_model_opt(opts: dict, key: str, value: Any) -> None:
    """Set a model option in its canonical location.

    Writes to ``model_options`` if that sub-dict exists, otherwise writes
    to the flat (deprecated) top-level location.  When the key exists in
    both places, both are updated to keep them consistent.
    """
    mo = opts.get("model_options")
    if isinstance(mo, dict):
        mo[key] = value
    if key in opts or not isinstance(mo, dict):
        opts[key] = value


def _validate_options(opts: dict) -> list[str]:
    """Validate planning options and return a list of warning/error messages.

    Each message is prefixed with ``WARN:`` or ``FIX:`` to indicate
    whether it was only warned about or auto-corrected.
    """
    messages: list[str] = []

    # ── Scale factors ──
    scale_obj = _get_model_opt(opts, "scale_objective")
    if scale_obj is not None:
        if scale_obj <= 0:
            messages.append("FIX: scale_objective must be > 0, setting to 1000")
            _set_model_opt(opts, "scale_objective", 1000)
        elif scale_obj > 1e12:
            messages.append(f"WARN: scale_objective={scale_obj} is very large")

    scale_theta = _get_model_opt(opts, "scale_theta")
    if scale_theta is not None and scale_theta <= 0:
        messages.append("FIX: scale_theta must be > 0, setting to 1000")
        _set_model_opt(opts, "scale_theta", 1000)

    # ── Demand/reserve fail cost ──
    dfc = _get_model_opt(opts, "demand_fail_cost")
    if dfc is not None and dfc == 0:
        messages.append("WARN: demand_fail_cost=0 means unserved load has no penalty")

    # ── Format validation ──
    for key, valid in [
        ("input_format", _VALID_INPUT_FORMATS),
        ("output_format", _VALID_OUTPUT_FORMATS),
    ]:
        val = opts.get(key)
        if val and val not in valid:
            messages.append(f"WARN: {key}='{val}' not in {valid}")

    # ── Method (solver type) ──
    method = opts.get("method")
    if method and method not in _VALID_SOLVER_TYPES:
        messages.append(f"WARN: method='{method}' not in {_VALID_SOLVER_TYPES}")

    # ── Discount rate ──
    rate = _get_model_opt(opts, "annual_discount_rate")
    if rate is not None:
        if rate <= -1:
            messages.append("FIX: annual_discount_rate must be > -1, setting to 0")
            _set_model_opt(opts, "annual_discount_rate", 0.0)
        elif rate > 0.5:
            messages.append(
                f"WARN: annual_discount_rate={rate} (>50%) is unusually high"
            )

    # ── Kirchhoff + single-bus consistency ──
    if _get_model_opt(opts, "use_single_bus") and _get_model_opt(opts, "use_kirchhoff"):
        messages.append(
            "FIX: use_kirchhoff=true is incompatible with use_single_bus=true, "
            "disabling use_kirchhoff"
        )
        _set_model_opt(opts, "use_kirchhoff", False)

    # ── LP names level ──
    lp_names = opts.get("use_lp_names")
    if lp_names is not None and not isinstance(lp_names, bool):
        if lp_names not in (0, 1, 2):
            messages.append(f"FIX: use_lp_names={lp_names} invalid, setting to 1")
            opts["use_lp_names"] = 1

    # ── Deprecated top-level solver fields ──
    solver_opts = opts.get("solver_options", {})
    for deprecated in ("lp_algorithm", "lp_threads", "lp_presolve"):
        if deprecated in opts and solver_opts:
            messages.append(f"WARN: deprecated '{deprecated}' overrides solver_options")

    # ── Solver options ──
    if solver_opts:
        algo = solver_opts.get("algorithm")
        if algo is not None and algo not in _VALID_LP_ALGORITHMS:
            messages.append(
                f"FIX: solver_options.algorithm={algo} invalid, setting to 0"
            )
            solver_opts["algorithm"] = 0

        thr = solver_opts.get("threads")
        if thr is not None and thr < 0:
            messages.append("FIX: solver_options.threads must be >= 0, setting to 0")
            solver_opts["threads"] = 0

        for eps_key in ("optimal_eps", "feasible_eps", "barrier_eps"):
            eps = solver_opts.get(eps_key)
            if eps is not None:
                if eps <= 0:
                    messages.append(
                        f"FIX: solver_options.{eps_key}={eps} must be > 0, "
                        "removing (use solver default)"
                    )
                    del solver_opts[eps_key]
                elif eps > 1e-3:
                    messages.append(
                        f"WARN: solver_options.{eps_key}={eps} is very loose"
                    )

    # ── SDDP options ──
    sddp = opts.get("sddp_options", {})
    if sddp:
        _validate_sddp_options(sddp, messages)

    # ── Cascade options ──
    cascade = opts.get("cascade_options", {})
    if cascade:
        _validate_cascade_options(cascade, messages)

    return messages


def _validate_sddp_options(sddp: dict, messages: list[str]) -> None:
    """Validate SDDP sub-options, appending messages."""
    max_iter = sddp.get("max_iterations")
    min_iter = sddp.get("min_iterations")
    if max_iter is not None and max_iter <= 0:
        messages.append("FIX: sddp max_iterations must be > 0, setting to 100")
        sddp["max_iterations"] = 100
    if min_iter is not None and min_iter < 0:
        messages.append("FIX: sddp min_iterations must be >= 0, setting to 0")
        sddp["min_iterations"] = 0
    if max_iter and min_iter and min_iter > max_iter:
        messages.append(
            f"FIX: sddp min_iterations ({min_iter}) > max_iterations ({max_iter}), "
            f"setting min_iterations to {max_iter}"
        )
        sddp["min_iterations"] = max_iter

    tol = sddp.get("convergence_tol")
    if tol is not None and tol <= 0:
        messages.append("FIX: sddp convergence_tol must be > 0, setting to 1e-4")
        sddp["convergence_tol"] = 1e-4

    penalty = sddp.get("elastic_penalty")
    if penalty is not None and penalty <= 0:
        messages.append("FIX: sddp elastic_penalty must be > 0, setting to 1e6")
        sddp["elastic_penalty"] = 1e6

    alpha_min = sddp.get("alpha_min")
    alpha_max = sddp.get("alpha_max")
    if alpha_min is not None and alpha_max is not None and alpha_min > alpha_max:
        messages.append(
            f"FIX: sddp alpha_min ({alpha_min}) > alpha_max ({alpha_max}), swapping"
        )
        sddp["alpha_min"], sddp["alpha_max"] = alpha_max, alpha_min

    for mode_key, valid in [
        ("cut_recovery_mode", _VALID_CUT_RECOVERY_MODES),
        ("recovery_mode", _VALID_RECOVERY_MODES),
        ("cut_sharing_mode", _VALID_CUT_SHARING_MODES),
        ("elastic_mode", _VALID_ELASTIC_MODES),
        ("boundary_cuts_mode", _VALID_BOUNDARY_MODES),
    ]:
        val = sddp.get(mode_key)
        if val and val not in valid:
            messages.append(f"WARN: sddp {mode_key}='{val}' not in {valid}")

    svlm = sddp.get("state_variable_lookup_mode")
    if svlm is not None and svlm not in ("warm_start", "cross_phase"):
        messages.append(
            f"WARN: sddp state_variable_lookup_mode='{svlm}' not in "
            "{{'warm_start', 'cross_phase'}}"
        )

    timeout = sddp.get("aperture_timeout")
    if timeout is not None and timeout < 0:
        messages.append("FIX: sddp aperture_timeout must be >= 0, setting to 0")
        sddp["aperture_timeout"] = 0

    max_cuts = sddp.get("max_cuts_per_phase")
    if max_cuts is not None and max_cuts < 0:
        messages.append("FIX: sddp max_cuts_per_phase must be >= 0, setting to 0")
        sddp["max_cuts_per_phase"] = 0

    prune_interval = sddp.get("cut_prune_interval")
    if prune_interval is not None and prune_interval <= 0:
        messages.append("FIX: sddp cut_prune_interval must be > 0, setting to 10")
        sddp["cut_prune_interval"] = 10

    prune_thr = sddp.get("prune_dual_threshold")
    if prune_thr is not None and prune_thr <= 0:
        messages.append("FIX: sddp prune_dual_threshold must be > 0, setting to 1e-8")
        sddp["prune_dual_threshold"] = 1e-8

    max_stored = sddp.get("max_stored_cuts")
    if max_stored is not None and max_stored < 0:
        messages.append("FIX: sddp max_stored_cuts must be >= 0, setting to 0")
        sddp["max_stored_cuts"] = 0

    sim_mode = sddp.get("simulation_mode")
    if sim_mode is not None and not isinstance(sim_mode, bool):
        messages.append(
            f"FIX: sddp simulation_mode must be a boolean, setting to {bool(sim_mode)}"
        )
        sddp["simulation_mode"] = bool(sim_mode)


def _validate_cascade_options(cascade: dict, messages: list[str]) -> None:
    """Validate cascade sub-options (hierarchical levels array).

    Supports both the new ``levels`` key and the legacy ``level_array``
    key used by the C++ JSON serialisation.
    """
    levels = cascade.get("levels") or cascade.get("level_array")
    if levels is None:
        return
    if not isinstance(levels, list):
        messages.append("WARN: cascade_options.levels must be an array")
        return

    for i, level in enumerate(levels):
        if not isinstance(level, dict):
            messages.append(f"WARN: cascade_options.levels[{i}] must be an object")
            continue

        # ── model_options sub-object (Kirchhoff / single-bus consistency) ──
        model_opts = level.get("model_options", {})
        if isinstance(model_opts, dict):
            if model_opts.get("use_single_bus") and model_opts.get("use_kirchhoff"):
                messages.append(
                    f"FIX: cascade level[{i}] model_options: "
                    "use_kirchhoff incompatible with use_single_bus, "
                    "disabling use_kirchhoff"
                )
                model_opts["use_kirchhoff"] = False

        # ── sddp_options sub-object (CascadeLevelMethod) ──
        sddp = level.get("sddp_options", {})
        if isinstance(sddp, dict):
            max_iter = sddp.get("max_iterations")
            if max_iter is not None and max_iter <= 0:
                messages.append(
                    f"FIX: cascade level[{i}] sddp_options.max_iterations "
                    "must be > 0, setting to 20"
                )
                sddp["max_iterations"] = 20

            min_iter = sddp.get("min_iterations")
            if min_iter is not None and min_iter < 0:
                messages.append(
                    f"FIX: cascade level[{i}] sddp_options.min_iterations "
                    "must be >= 0, setting to 0"
                )
                sddp["min_iterations"] = 0

            conv_tol = sddp.get("convergence_tol")
            if conv_tol is not None and conv_tol < 0:
                messages.append(
                    f"FIX: cascade level[{i}] sddp_options.convergence_tol "
                    "must be >= 0, setting to 0.01"
                )
                sddp["convergence_tol"] = 0.01

        # ── legacy solver sub-object (kept for backward compat) ──
        solver = level.get("solver", {})
        if isinstance(solver, dict):
            max_iter = solver.get("max_iterations")
            if max_iter is not None and max_iter <= 0:
                messages.append(
                    f"FIX: cascade level[{i}] solver.max_iterations must be > 0, "
                    "setting to 20"
                )
                solver["max_iterations"] = 20

            conv_tol = solver.get("convergence_tol")
            if conv_tol is not None and conv_tol < 0:
                messages.append(
                    f"FIX: cascade level[{i}] solver.convergence_tol must be >= 0, "
                    "setting to 0.01"
                )
                solver["convergence_tol"] = 0.01

            num_ap = solver.get("num_apertures")
            if num_ap is not None and num_ap < 0:
                messages.append(
                    f"FIX: cascade level[{i}] solver.num_apertures must be >= 0, "
                    "setting to 0"
                )
                solver["num_apertures"] = 0

        # ── transition sub-object ──
        transition = level.get("transition", {})
        if isinstance(transition, dict):
            target_rtol = transition.get("target_rtol")
            if target_rtol is not None and target_rtol < 0:
                messages.append(
                    f"FIX: cascade level[{i}] transition.target_rtol must be >= 0, "
                    "setting to 0.05"
                )
                transition["target_rtol"] = 0.05

            target_atol = transition.get("target_min_atol")
            if target_atol is not None and target_atol < 0:
                messages.append(
                    f"FIX: cascade level[{i}] transition.target_min_atol must be >= 0, "
                    "setting to 1.0"
                )
                transition["target_min_atol"] = 1.0

            target_penalty = transition.get("target_penalty")
            if target_penalty is not None and target_penalty < 0:
                messages.append(
                    f"FIX: cascade level[{i}] transition.target_penalty must be >= 0, "
                    "setting to 500.0"
                )
                transition["target_penalty"] = 500.0

            opt_dual_thr = transition.get("optimality_dual_threshold")
            if opt_dual_thr is not None and opt_dual_thr < 0:
                messages.append(
                    f"FIX: cascade level[{i}] transition.optimality_dual_threshold "
                    "must be >= 0, setting to 0.0"
                )
                transition["optimality_dual_threshold"] = 0.0


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def sanitize_json(
    json_path: Path,
    *,
    compression: str | None = None,
    threads: int | None = None,
    output_directory: str | None = None,
    input_directory: str | None = None,
    export_path: Path | None = None,
) -> Path | None:
    """Read *json_path*, validate options, apply runtime fixes, write output.

    If no fixes are needed the original path is returned as-is and no
    file is written.

    Args:
        json_path: Path to the original planning JSON.
        compression: Override ``output_compression``.
        threads: Override ``solver_options.threads``.
        output_directory: Override ``output_directory``.
        input_directory: Override ``input_directory``.
        export_path: If given, write the sanitized JSON to this path
            instead of a ``_sanitized.json`` sibling file.

    Returns:
        The path to the sanitized JSON (may be the original if nothing
        changed), or ``None`` on read/parse failure.
    """
    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        log.error("cannot load JSON for sanitization: %s", exc)
        return None

    opts = data.setdefault("options", {})
    changed = False

    # ── Validate and auto-fix options ──
    messages = _validate_options(opts)
    for msg in messages:
        if msg.startswith("FIX:"):
            log.warning("  %s", msg)
            changed = True
        else:
            log.info("  %s", msg)

    # ── Compression codec ──
    if compression:
        current = opts.get("output_compression", "")
        if current and current != compression:
            log.info(
                "sanitize: output_compression '%s' -> '%s'",
                current,
                compression,
            )
            opts["output_compression"] = compression
            changed = True

    # ── Solver threads ──
    if threads is not None:
        solver_opts = opts.setdefault("solver_options", {})
        current_threads = solver_opts.get("threads")
        if current_threads != threads:
            log.info(
                "sanitize: solver_options.threads %s -> %d",
                current_threads,
                threads,
            )
            solver_opts["threads"] = threads
            changed = True

    # ── Output directory ──
    if output_directory:
        current_out = opts.get("output_directory", "")
        if current_out != output_directory:
            log.info(
                "sanitize: output_directory '%s' -> '%s'",
                current_out,
                output_directory,
            )
            opts["output_directory"] = output_directory
            changed = True

    # ── Input directory ──
    if input_directory:
        current_in = opts.get("input_directory", "")
        if current_in != input_directory:
            log.info(
                "sanitize: input_directory '%s' -> '%s'",
                current_in,
                input_directory,
            )
            opts["input_directory"] = input_directory
            changed = True

    # ── Resolve relative input/output directories to absolute ──
    # gtopt resolves paths relative to its CWD, which may differ from the
    # JSON file's location.  Make them absolute so the solver always finds
    # the correct directory regardless of CWD.
    case_root = json_path.parent
    for dir_key in ("input_directory", "output_directory"):
        raw = opts.get(dir_key, "")
        if raw and not Path(raw).is_absolute():
            resolved = str((case_root / raw).resolve())
            if resolved != raw:
                log.info("sanitize: %s '%s' -> '%s'", dir_key, raw, resolved)
                opts[dir_key] = resolved
                changed = True

    if not changed:
        return json_path

    # Write the sanitized JSON
    out_path = export_path or json_path.with_stem(json_path.stem + "_sanitized")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

    log.info("sanitized JSON written to %s", out_path)
    return out_path
