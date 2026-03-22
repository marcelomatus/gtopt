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

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Option validation and auto-fix
# ---------------------------------------------------------------------------

_VALID_SOLVER_TYPES = {"monolithic", "sddp"}
_VALID_INPUT_FORMATS = {"parquet", "csv"}
_VALID_OUTPUT_FORMATS = {"parquet", "csv"}
_VALID_LP_ALGORITHMS = {0, 1, 2, 3}
_VALID_HOT_START_MODES = {"none", "keep", "append", "replace"}
_VALID_CUT_SHARING_MODES = {"none", "expected", "accumulate", "max"}
_VALID_ELASTIC_MODES = {"single_cut", "multi_cut", "backpropagate", "cut"}
_VALID_BOUNDARY_MODES = {"noload", "separated", "combined"}


def _validate_options(opts: dict) -> list[str]:
    """Validate planning options and return a list of warning/error messages.

    Each message is prefixed with ``WARN:`` or ``FIX:`` to indicate
    whether it was only warned about or auto-corrected.
    """
    messages: list[str] = []

    # ── Scale factors ──
    scale_obj = opts.get("scale_objective")
    if scale_obj is not None:
        if scale_obj <= 0:
            messages.append("FIX: scale_objective must be > 0, setting to 1000")
            opts["scale_objective"] = 1000
        elif scale_obj > 1e12:
            messages.append(f"WARN: scale_objective={scale_obj} is very large")

    scale_theta = opts.get("scale_theta")
    if scale_theta is not None and scale_theta <= 0:
        messages.append("FIX: scale_theta must be > 0, setting to 1000")
        opts["scale_theta"] = 1000

    # ── Demand/reserve fail cost ──
    dfc = opts.get("demand_fail_cost")
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

    # ── Solver type ──
    solver_type = opts.get("solver_type")
    if solver_type and solver_type not in _VALID_SOLVER_TYPES:
        messages.append(
            f"WARN: solver_type='{solver_type}' not in {_VALID_SOLVER_TYPES}"
        )

    # ── Discount rate ──
    rate = opts.get("annual_discount_rate")
    if rate is not None:
        if rate <= -1:
            messages.append("FIX: annual_discount_rate must be > -1, setting to 0")
            opts["annual_discount_rate"] = 0.0
        elif rate > 0.5:
            messages.append(
                f"WARN: annual_discount_rate={rate} (>50%) is unusually high"
            )

    # ── Kirchhoff + single-bus consistency ──
    if opts.get("use_single_bus") and opts.get("use_kirchhoff"):
        messages.append(
            "FIX: use_kirchhoff=true is incompatible with use_single_bus=true, "
            "disabling use_kirchhoff"
        )
        opts["use_kirchhoff"] = False

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
        messages.append("FIX: sddp elastic_penalty must be > 0, setting to 1000")
        sddp["elastic_penalty"] = 1000

    alpha_min = sddp.get("alpha_min")
    alpha_max = sddp.get("alpha_max")
    if alpha_min is not None and alpha_max is not None and alpha_min > alpha_max:
        messages.append(
            f"FIX: sddp alpha_min ({alpha_min}) > alpha_max ({alpha_max}), swapping"
        )
        sddp["alpha_min"], sddp["alpha_max"] = alpha_max, alpha_min

    for mode_key, valid in [
        ("hot_start_mode", _VALID_HOT_START_MODES),
        ("cut_sharing_mode", _VALID_CUT_SHARING_MODES),
        ("elastic_mode", _VALID_ELASTIC_MODES),
        ("boundary_cuts_mode", _VALID_BOUNDARY_MODES),
    ]:
        val = sddp.get(mode_key)
        if val and val not in valid:
            messages.append(f"WARN: sddp {mode_key}='{val}' not in {valid}")

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

    if not changed:
        return json_path

    # Write the sanitized JSON
    out_path = export_path or json_path.with_stem(json_path.stem + "_sanitized")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

    log.info("sanitized JSON written to %s", out_path)
    return out_path
