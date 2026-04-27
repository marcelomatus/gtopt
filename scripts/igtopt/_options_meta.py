# SPDX-License-Identifier: BSD-3-Clause
"""Options metadata for the gtopt Excel template.

Pure data — partitions the flat ``options`` worksheet keys into the JSON
sub-objects (``sddp_options``, ``model_options``, ``simulation``,
``monolithic_options``, ``solver_options``).

The frozensets below mirror the C++ ``json_data_contract<...>`` field lists
in :file:`include/gtopt/json/*.hpp`.  Keep them in sync.

``_OPTIONS_FIELDS`` is the ordered list ``(key, description, default)``
that drives the rows of the ``options`` worksheet.

Imported by :mod:`igtopt.igtopt` (via :mod:`igtopt.template_builder`)
to dispatch flat keys into their JSON sub-objects.
"""

from __future__ import annotations

from typing import Any

# Keys that belong inside the ``sddp_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<SddpOptions>`` in
# ``include/gtopt/json/json_options.hpp``.
SDDP_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "cut_sharing_mode",
        "cut_directory",
        "api_enabled",
        "production_factor_update_skip",
        "max_iterations",
        "min_iterations",
        "convergence_tol",
        "elastic_penalty",
        "cut_recovery_mode",
        "recovery_mode",
        "save_per_iteration",
        "cuts_input_file",
        "sentinel_file",
        "elastic_mode",
        "multi_cut_threshold",
        "apertures",
        "num_apertures",
        "aperture_directory",
        "aperture_timeout",
        "save_aperture_lp",
        "boundary_cuts_file",
        "boundary_cuts_mode",
        "boundary_max_iterations",
        "missing_cut_var_mode",
        "named_cuts_file",
        "max_cuts_per_phase",
        "cut_prune_interval",
        "prune_dual_threshold",
        "single_cut_storage",
        "max_stored_cuts",
        "simulation_mode",
        "state_variable_lookup_mode",
        "warm_start",
        "stationary_tol",
        "stationary_window",
        "convergence_mode",
        "convergence_confidence",
        "cut_coeff_eps",
        "scale_alpha",
        "update_lp_skip",
        "forward_solver_options",
        "forward_max_fallbacks",
        "backward_solver_options",
        "backward_max_fallbacks",
        "max_async_spread",
        "low_memory_mode",
        "memory_codec",
    }
)

# Cascade options now use a hierarchical ``levels`` array structure
# that is too complex for the flat Excel template.  Cascade
# configuration should be done directly in JSON.  This frozenset is
# kept empty for backward compatibility with imports.
CASCADE_OPTION_KEYS: frozenset[str] = frozenset()

# Keys that belong inside the ``monolithic_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<MonolithicOptions>``.
# In the flat Excel sheet these are prefixed with ``monolithic_`` to
# distinguish them from the identically-named SDDP fields (e.g.
# ``monolithic_boundary_cuts_file`` → ``boundary_cuts_file``).
MONOLITHIC_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "solve_mode",
        "boundary_cuts_file",
        "boundary_cuts_mode",
        "boundary_max_iterations",
        "solver_options",
    }
)

# Keys that belong inside the ``solver_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<SolverOptions>`` in
# ``include/gtopt/json/json_solver_options.hpp``.
# In the flat Excel sheet these are prefixed with ``solver_`` to
# distinguish them from other options (e.g.
# ``solver_time_limit`` → ``time_limit``).
SOLVER_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "algorithm",
        "threads",
        "presolve",
        "optimal_eps",
        "feasible_eps",
        "barrier_eps",
        "log_level",
        "time_limit",
        "reuse_basis",
        "scaling",
        "log_mode",
        "max_fallbacks",
        "crossover",
    }
)

# Keys that belong inside the ``model_options`` JSON sub-object.
# Must match the fields in ``json_data_contract<ModelOptions>``.
MODEL_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "use_single_bus",
        "use_kirchhoff",
        "use_line_losses",
        "kirchhoff_threshold",
        "loss_segments",
        "scale_objective",
        "scale_theta",
        "demand_fail_cost",
        "reserve_fail_cost",
        "hydro_fail_cost",
        "hydro_use_value",
    }
)

# Keys that belong in the ``simulation`` JSON section rather than ``options``.
# ``annual_discount_rate`` is the canonical simulation-level field; its
# presence under ``options`` is deprecated (the C++ parser emits a warning).
SIMULATION_OPTION_KEYS: frozenset[str] = frozenset(
    {
        "annual_discount_rate",
    }
)

_OPTIONS_FIELDS: list[tuple[str, str, Any]] = [
    # ------------------------------------------------------------------
    # Top-level options (stay at root of "options" JSON object)
    # ------------------------------------------------------------------
    ("input_directory", "Directory for input time-series files", "input"),
    (
        "input_format",
        "Preferred input file format: 'parquet' or 'csv'",
        "parquet",
    ),
    # ------------------------------------------------------------------
    # Model options (nested into "model_options" in JSON output)
    # ------------------------------------------------------------------
    ("demand_fail_cost", "[model] Penalty for unserved load [$/MWh]", 1000),
    ("reserve_fail_cost", "[model] Penalty for unserved spinning reserve [$/MW]", 5000),
    ("use_line_losses", "[model] Enable line loss modelling (true/false)", True),
    (
        "loss_segments",
        "[model] Number of piecewise-linear loss segments (1=linear only)",
        1,
    ),
    (
        "use_kirchhoff",
        "[model] Apply DC Kirchhoff OPF constraints (true/false)",
        True,
    ),
    (
        "use_single_bus",
        "[model] Copper-plate (no network) mode – ignores all line limits (true/false)",
        False,
    ),
    (
        "kirchhoff_threshold",
        "[model] Minimum bus voltage [kV] for Kirchhoff constraints",
        None,
    ),
    (
        "scale_objective",
        "[model] Divide objective coefficients by this value for solver numerics",
        1000,
    ),
    (
        "scale_theta",
        "[model] Angle variable scaling factor (default: 1000)",
        1000,
    ),
    # ------------------------------------------------------------------
    # Simulation fields (moved into "simulation" in JSON output)
    # ------------------------------------------------------------------
    (
        "annual_discount_rate",
        "[simulation] Annual discount rate for CAPEX [p.u.] (e.g. 0.10 = 10 %)",
        0.1,
    ),
    ("output_directory", "Directory for solution output files", "output"),
    ("output_format", "Output file format: 'parquet' or 'csv'", "parquet"),
    (
        "output_compression",
        "Parquet compression codec: 'gzip', 'snappy', 'zstd', or ''",
        "gzip",
    ),
    (
        "use_lp_names",
        "LP naming level: 0=none, 1=names+warn, 2=names+error",
        None,
    ),
    (
        "use_uid_fname",
        "Use uid-based filenames for output (true/false)",
        None,
    ),
    (
        "method",
        "Planning method: 'monolithic' (default), 'sddp', or 'cascade'",
        None,
    ),
    ("log_directory", "Directory for solver log files", "logs"),
    (
        "lp_debug",
        "Save debug LP files to log directory (true/false)",
        None,
    ),
    (
        "lp_compression",
        "Compression codec for debug LP files (e.g. 'gzip')",
        None,
    ),
    ("lp_only", "Build LP without solving (true/false)", None),
    (
        "lp_coeff_ratio_threshold",
        "Warn when LP coefficient ratio exceeds this value",
        None,
    ),
    # ------------------------------------------------------------------
    # Solver options (nested into "solver_options" in JSON output)
    # In the flat Excel sheet these use a "solver_" prefix.
    # The prefix is stripped when writing the JSON sub-object.
    # ------------------------------------------------------------------
    (
        "solver_algorithm",
        "[solver] LP algorithm: 0=default, 1=primal, 2=dual, 3=barrier (default: 3)",
        3,
    ),
    (
        "solver_threads",
        "[solver] Number of parallel LP threads (0 = automatic, default: 0)",
        0,
    ),
    (
        "solver_presolve",
        "[solver] Apply LP presolve optimizations (true/false, default: true)",
        True,
    ),
    (
        "solver_time_limit",
        "[solver] Per-solve time limit in seconds; "
        "passed to LP backend (CLP setMaximumSeconds, HiGHS time_limit). "
        "0 = no limit",
        None,
    ),
    (
        "solver_optimal_eps",
        "[solver] Optimality tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_feasible_eps",
        "[solver] Feasibility tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_barrier_eps",
        "[solver] Barrier convergence tolerance (blank = use solver default)",
        None,
    ),
    (
        "solver_log_level",
        "[solver] Solver output verbosity (0 = none, default: 0)",
        0,
    ),
    (
        "solver_reuse_basis",
        "[solver] Enable basis-reuse for resolves (true/false, default: false)",
        None,
    ),
    # ------------------------------------------------------------------
    # SDDP options (nested into "sddp_options" in JSON output)
    # ------------------------------------------------------------------
    (
        "cut_sharing_mode",
        "[sddp] How Benders cuts are shared: 'none', 'expected', 'accumulate', or 'max'",
        None,
    ),
    (
        "cut_directory",
        "[sddp] Directory for SDDP Benders cut files",
        "cuts",
    ),
    (
        "api_enabled",
        "[sddp] Write SDDP status JSON for monitoring (true/false)",
        None,
    ),
    (
        "production_factor_update_skip",
        "[sddp] SDDP iterations between reservoir efficiency updates",
        None,
    ),
    ("max_iterations", "[sddp] Maximum SDDP outer iterations", None),
    ("min_iterations", "[sddp] Minimum SDDP outer iterations", None),
    (
        "convergence_tol",
        "[sddp] SDDP convergence tolerance (gap between bounds)",
        None,
    ),
    (
        "elastic_penalty",
        "[sddp] Penalty for elastic constraint relaxation",
        None,
    ),
    (
        "cut_recovery_mode",
        "[sddp] Cut persistence mode: 'none' (default), 'keep', 'append', or 'replace'",
        None,
    ),
    (
        "recovery_mode",
        "[sddp] Recovery mode: 'none' (0), 'cuts' (1), or 'full' (2, default)",
        None,
    ),
    (
        "warm_start",
        "[sddp] Enable warm-start for SDDP resolves (true/false, default: true)",
        None,
    ),
    (
        "save_per_iteration",
        "[sddp] Save cuts after every iteration (true/false)",
        None,
    ),
    (
        "cuts_input_file",
        "[sddp] Path to pre-computed Benders cuts file",
        None,
    ),
    (
        "sentinel_file",
        "[sddp] Path to sentinel file that stops SDDP early",
        None,
    ),
    (
        "elastic_mode",
        "[sddp] Elastic filter mode: 'chinneck' (default), 'single_cut', or 'multi_cut'",
        None,
    ),
    (
        "multi_cut_threshold",
        "[sddp] Threshold for multi-cut aggregation",
        None,
    ),
    (
        "num_apertures",
        "[sddp] Number of backward-pass apertures",
        None,
    ),
    (
        "aperture_directory",
        "[sddp] Directory for aperture definition files",
        None,
    ),
    (
        "aperture_timeout",
        "[sddp] Timeout in seconds for each aperture solve",
        None,
    ),
    (
        "save_aperture_lp",
        "[sddp] Save LP files for infeasible apertures (true/false)",
        None,
    ),
    (
        "boundary_cuts_file",
        "[sddp] Path to boundary (future-cost) cuts CSV for last stage",
        None,
    ),
    (
        "boundary_cuts_mode",
        "[sddp] Boundary cuts load mode: 'noload', 'separated', 'combined'",
        "separated",
    ),
    (
        "boundary_max_iterations",
        "[sddp] Max SDDP iterations to load from boundary cuts (0=all)",
        0,
    ),
    (
        "named_cuts_file",
        "[sddp] Path to named cuts file for warm-starting SDDP",
        None,
    ),
    (
        "max_cuts_per_phase",
        "[sddp] Max retained cuts per (scene,phase) LP (0=unlimited, no pruning)",
        0,
    ),
    (
        "cut_prune_interval",
        "[sddp] Iterations between cut pruning passes (requires max_cuts_per_phase>0)",
        10,
    ),
    (
        "prune_dual_threshold",
        "[sddp] Dual threshold for inactive cut detection during pruning",
        1e-8,
    ),
    (
        "single_cut_storage",
        "[sddp] Store cuts per-scene only, build combined on demand (saves memory)",
        False,
    ),
    (
        "max_stored_cuts",
        "[sddp] Max stored cuts per scene (0=unlimited; oldest dropped first)",
        0,
    ),
    (
        "simulation_mode",
        "[sddp] Forward-only evaluation of loaded cuts, no training (true/false)",
        None,
    ),
    # NOTE: Cascade options now use a hierarchical ``levels`` array
    # structure (with lp_options, solver, and transition sub-objects per
    # level).  This is too complex for the flat Excel template; cascade
    # configuration should be done directly in JSON.
    # ------------------------------------------------------------------
    # Monolithic options (nested into "monolithic_options" in JSON output)
    # In the flat Excel sheet these use a "monolithic_" prefix to avoid
    # name collisions with the SDDP options above.  The prefix is
    # stripped when writing the JSON sub-object.
    # ------------------------------------------------------------------
    (
        "monolithic_solve_mode",
        "[monolithic] Solve mode: 'monolithic' or 'relaxed'",
        None,
    ),
    (
        "monolithic_boundary_cuts_file",
        "[monolithic] Path to boundary cuts CSV for monolithic solver",
        None,
    ),
    (
        "monolithic_boundary_cuts_mode",
        "[monolithic] Boundary cuts mode: 'noload', 'separated', 'combined'",
        None,
    ),
    (
        "monolithic_boundary_max_iterations",
        "[monolithic] Max iterations to load from boundary cuts (0=all)",
        None,
    ),
]
