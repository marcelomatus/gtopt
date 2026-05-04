/**
 * @file      gtopt_main.hpp
 * @brief     Core application entry point for the gtopt optimizer
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the main optimization entry point that parses
 * planning JSON files, applies command-line options, builds and solves
 * the linear programming model, and writes the results.
 */

#pragma once

#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

/**
 * @brief All command-line options consumed by gtopt_main().
 *
 * Every field is optional so that callers only set what they need and can use
 * designated-initializer syntax:
 *
 * @code{.cpp}
 *   gtopt_main(MainOptions{
 *     .planning_files = files,
 *     .use_single_bus = true,
 *     .print_stats    = true,
 *   });
 * @endcode
 */
struct MainOptions
{
  // ---- required ----
  /** @brief Paths to planning JSON files (``planning.json``).
   *
   * At least one file is required.  When multiple files are given they are
   * merged in order (later files override earlier ones).  Each file may be:
   * - a full path to a ``.json`` file,
   * - a stem without extension (the ``.json`` suffix is appended), or
   * - a directory name (resolved to ``dir/dir.json``).
   */
  std::vector<std::string> planning_files {};

  // ---- I/O directories / formats ----
  /** @brief Override for the input data directory */
  std::optional<std::string> input_directory {};
  /** @brief Input format override ("parquet", "csv", …) */
  std::optional<std::string> input_format {};
  /** @brief Override for the output directory */
  std::optional<std::string> output_directory {};
  /** @brief Output format ("parquet", "csv") */
  std::optional<std::string> output_format {};
  /** @brief Compression codec for parquet output ("gzip", "zstd", …) */
  std::optional<std::string> output_compression {};

  // ---- modelling flags ----
  /** @brief Enable single-bus (copper-plate) mode */
  std::optional<bool> use_single_bus {};
  /** @brief Enable Kirchhoff voltage-law constraints */
  std::optional<bool> use_kirchhoff {};

  // ---- LP options ----
  /** @brief Path stem for writing the LP model file */
  std::optional<std::string> lp_file {};
  /** @brief Epsilon tolerance for LP matrix coefficients */
  std::optional<double> matrix_eps {};
  /** @brief Build all scene/phase LP matrices but skip solving entirely */
  std::optional<bool> lp_only {};
  /** @brief Save debug LP files to the log directory (monolithic: one per
   * scene/phase; SDDP: one per iteration/scene/phase) */
  std::optional<bool> lp_debug {};
  /** @brief Compression codec for LP debug files.
   * `""` = auto (let gtopt_compress_lp decide); `"none"` = no compression;
   * `"gzip"`, `"zstd"`, `"lz4"`, `"bzip2"`, `"xz"` = specific codec. */
  std::optional<std::string> lp_compression {};
  /** @brief LP coefficient ratio threshold for numerical conditioning
   * diagnostics.  When the global max/min |coefficient| ratio exceeds this
   * value, a per-scene/phase breakdown is printed.  (default: 1e7) */
  std::optional<double> lp_coeff_ratio_threshold {};

  // ---- debug / output helpers ----
  /** @brief Path stem for writing the merged planning JSON */
  std::optional<std::string> json_file {};

  // ---- execution control ----
  /** @brief Print pre- and post-solve system statistics */
  std::optional<bool> print_stats {};

  // ---- tracing / diagnostics ----
  /** @brief Path to a file for SPDLOG_TRACE output (enables trace-level
   * logging).  When set, a dedicated file sink is added to spdlog so that
   * all trace-level messages are captured for later review. */
  std::optional<std::string> trace_log {};

  /** @brief Directory to dump backward-pass tgt LPs (one .lp file per
   * `(iter, scene, phase)`) immediately before each `tgt_li.resolve(opts)`.
   *
   * When set, captures the LP exactly as the solver sees it
   * (post-`update_lp_for_phase`, post-`apply_post_load_replay` under
   * compress).  Diff'ing the off and compress dumps for the same
   * `(iter, scene, phase)` localises any non-replayed mutation that
   * survives off but is dropped by compress's reconstruct.  Zero
   * overhead when unset.
   *
   * Translation shim over the unified LP-debug mechanism: setting
   * this flag is equivalent to passing
   *   `--lp-debug --set sddp_options.lp_debug_passes=backward
   *    --log-directory <dir>`
   * (any pre-existing `lp_debug_passes` is preserved when it already
   * mentions `backward` or `all`).  The `GTOPT_DUMP_BACKWARD_LP=<dir>`
   * env var is honoured as a fallback for scripts that pre-date the
   * flag.  Used during the off↔compress symmetry investigation to
   * confirm LP byte-identity (50/50 phases md5-equal at iter 1
   * post-fix).
   */
  std::optional<std::string> lp_dump_backward {};

  // ---- SDDP-specific directories ----
  /** @brief Directory for Benders cut files (default: "cuts") */
  std::optional<std::string> cut_directory {};
  /** @brief Directory for log and trace files (default: "logs") */
  std::optional<std::string> log_directory {};

  // ---- SDDP algorithm tuning ----
  /** @brief Maximum SDDP forward/backward iterations (default: 100) */
  std::optional<int> sddp_max_iterations {};
  /** @brief Minimum SDDP iterations before convergence (default: 2) */
  std::optional<int> sddp_min_iterations {};
  /** @brief SDDP relative convergence tolerance (default: 1e-4) */
  std::optional<double> sddp_convergence_tol {};
  /** @brief Penalty coefficient for SDDP elastic slack variables (default:
   * 1000) */
  std::optional<double> sddp_elastic_penalty {};
  /** @brief SDDP elastic filter mode: "chinneck" (default), "single_cut",
   *  "multi_cut".  Aliases: "iis" → chinneck, "cut" → single_cut. */
  std::optional<std::string> sddp_elastic_mode {};
  /** @brief Number of SDDP backward-pass apertures (0=disabled, -1=all) */
  std::optional<int> sddp_num_apertures {};
  /** @brief Enable SDDP hot-start from previously saved cuts */
  std::optional<bool> sddp_hot_start {};

  /** @brief Enable recovery from a previous SDDP run.
   *
   * When true, the JSON `recovery_mode` setting takes effect (default "full").
   * When false or unset, `recovery_mode` is forced to "none" regardless of
   * the JSON configuration — i.e. recovery only happens when the user
   * explicitly passes `--recover` on the command line.
   */
  std::optional<bool> recover {};

  /** @brief Global memory-saving mode: `off` / `compress` / `rebuild`.
   *
   * Generalises the older `--low-memory` flag: when set, the CLI applies
   * a coordinated set of memory-saving defaults across the whole run:
   *
   *   - `sddp_options.low_memory_mode` = <value>  (same semantics as
   *     before: off / compress / rebuild of the flat-LP snapshot)
   *   - `solver_options.memory_emphasis` = true    (solver-native hint;
   *     CPLEX's `CPX_PARAM_MEMORYEMPHASIS=1`, ignored by backends that
   *     have no equivalent).
   *
   * Users who want finer control can still set `low_memory_mode` or
   * `memory_emphasis` directly in the planning JSON — the CLI is just
   * the shortcut "turn everything on sensibly".
   *
   * Implicit value (flag with no argument) is `compress`, which is the
   * best balance: releases the solver backend between phases (big RAM
   * win) while keeping the compressed flat LP so the next solve
   * reconstructs in ~50 ms instead of a full re-flatten.  `rebuild`
   * gives the lowest steady-state RAM at higher CPU cost.
   *
   * Bound to `--memory-saving`; `--low-memory` remains as a hidden
   * deprecated alias for one release. */
  std::optional<std::string> memory_saving {};

  // ---- resource limits ----
  /** @brief Process memory limit for work pool throttling.
   * Accepts an absolute value in MB, or a string with suffix:
   * "300M" (megabytes), "5G" (gigabytes).  0 = no limit (default).
   * Mutually exclusive with `memory_quota` — when both are set,
   * `memory_quota` wins (it is the higher-level convenience flag). */
  std::optional<std::string> memory_limit {};

  /** @brief Memory budget as a percentage of total host RAM.
   *
   * Sugar for `memory_limit`: when set, `apply_cli_options` reads
   * `MemTotal` from `/proc/meminfo` and assigns
   * `pool_memory_limit_mb = total_mb × pct / 100`.  Values outside
   * (0, 100) are treated as "unset".
   *
   * Bound to `--memory-quota`. */
  std::optional<double> memory_quota {};

  /** @brief SDDP work pool CPU over-commit factor.
   * Multiplied by hardware_concurrency to set max pool threads.
   * Default 4.0 — extra threads compensate for clone mutex blocking. */
  std::optional<double> sddp_cpu_factor {};

  /** @brief CPU budget as a percentage of physical cores.
   *
   * When set during `apply_cli_options`, calls
   * `set_cpu_quota_pct(pct)`.  This shrinks the value reported by
   * `physical_concurrency()` for the rest of the process to
   * `ceil(detected × pct / 100)` — every work-pool factory then sizes
   * itself against the smaller base, so a `cpu_factor=2.0` pool on a
   * 9-core box with `cpu_quota=30` ends up at `max_threads=6` instead
   * of 18.  Values outside (0, 100) are treated as "unset" (no clamp).
   *
   * Does **not** affect the LP solver's internal thread count —
   * use `--threads` / per-solver settings for that.
   *
   * Bound to `--cpu-quota`. */
  std::optional<double> cpu_quota {};

  /** @brief LP build parallelism mode: "serial", "scene-parallel",
   *  or "full-parallel" (default).  Routed to
   *  `planning.options.build_mode` by `apply_cli_options`.  See
   *  `BuildMode` in `planning_enums.hpp` for the full contract. */
  std::optional<std::string> build_mode {};

  /** @brief Comma-separated list of output fields to emit
   * (`solution`, `dual`, `reduced_cost`, or aliases `sol`, `cost`,
   * `rcost`, `rc`; also `all` and `none`).  Default (unset) emits
   * every field.  Routed to `planning.options.write_out` by
   * `apply_cli_options`. */
  std::optional<std::string> write_out {};

  // ---- solver selection ----
  /** @brief LP solver backend name ("clp", "cbc", "cplex", "highs").
   * When empty, auto-detects from available plugins. */
  std::optional<std::string> solver {};

  // ---- solver algorithm (shortcuts for solver_options fields) ----
  /** @brief LP solution algorithm override.
   * Mapped to solver_options.algorithm by apply_cli_options. */
  std::optional<LPAlgo> algorithm {};
  /** @brief Number of solver threads override (0=automatic).
   *  Mapped to solver_options.threads by apply_cli_options. */
  std::optional<int> threads {};

  // ---- per-solver configuration ----
  /** @brief Per-solver default SolverOptions loaded from `.gtopt.conf`.
   *
   * Keys are solver names ("cplex", "highs", "clp").  Populated from
   * `[solver.cplex]`, `[solver.highs]`, etc. sections in the config file.
   * The matching entry is overlaid on top of the backend's
   * `optimal_options()` before applying user-explicit solver_options.
   */
  std::map<std::string, SolverOptions> solver_configs {};

  /** @brief Disable every automatic scaling / equilibration mechanism.
   *
   * When true, the CLI forces:
   *   - `model_options.scale_objective = 1.0`   (no obj divisor)
   *   - `model_options.scale_theta     = 1.0`   (no Kirchhoff angle scale)
   *   - `lp_matrix_options.equilibration_method = none`
   *
   * Intended for debug / physical-unit validation runs where you want
   * the LP coefficients in their raw data-layer units, at the cost of
   * potentially much higher solver kappa.  Bound to the CLI flag
   * `--no-scale`.  A user-explicit JSON setting of any of the three
   * fields takes precedence (the override only fills in what the JSON
   * leaves unset). */
  std::optional<bool> no_scale {};

  // ---- generic option overrides ----
  /** @brief Repeatable ``--set key=value`` overrides.
   *
   * Each entry is a ``dotted.path=value`` string that maps to a field in
   * the Planning options JSON structure.  Values are auto-typed:
   * ``true``/``false`` → bool, integers → int, decimals → double,
   * otherwise → string.  Applied as a JSON overlay merged into Planning
   * after file parsing but before specific CLI flags.
   *
   * Example: ``--set sddp_options.forward_solver_options.threads=8``
   */
  std::vector<std::string> set_options {};

  /** @brief True when input/output/log/cut directories were auto-resolved
   * from a directory argument (not explicitly set by the user).
   * Suppresses deprecation warnings in apply_cli_options. */
  bool dirs_auto_resolved {false};
};

/**
 * @brief Run the gtopt power-system optimizer.
 *
 * Reads the planning files listed in @p raw_opts.planning_files, merges
 * them into a single Planning object, applies CLI overrides, builds and
 * solves the LP model, writes the solution output, and saves a copy of
 * the merged planning as ``planning.json`` in the output directory.
 *
 * @param raw_opts  All runtime options; only set the fields you need.
 * @return 0 on success, 1 on infeasibility, or an error string on failure.
 */
[[nodiscard]] std::expected<int, std::string> gtopt_main(
    const MainOptions& raw_opts);

/**
 * @brief Add a `<log_dir>/gtopt.log` file sink to spdlog's default logger.
 *
 * Resolves the log directory using @c opts.log_directory if set, else
 * `<output_directory>/logs`.  Creates the directory if needed and
 * attaches a `basic_file_sink_mt` (truncating) at the current default
 * level so all subsequent `spdlog::info/warn/error` go to the file.
 *
 * When @p suppress_stdout is true, the existing console sinks are
 * removed so log output appears only in the file — this is what the
 * standalone binary uses when stdout is not a TTY (i.e. when piped to
 * `run_gtopt`, redirected, or run from CI), keeping the calling
 * process's stdout clean and avoiding duplicate-output overhead.
 */
void setup_file_logging(const MainOptions& opts, bool suppress_stdout);

/**
 * @brief Classify an error string into an exit code.
 *
 * Examines @p error for keywords indicating input-related errors vs
 * internal/solver errors.
 *
 * @return 2 for input errors (missing file, parse error, invalid JSON),
 *         3 for internal/solver errors.
 */
[[nodiscard]] inline int classify_error_exit_code(
    std::string_view error) noexcept
{
  if (error.contains("not found") || error.contains("not exist")
      || error.contains("Cannot open") || error.contains("parse")
      || error.contains("Invalid") || error.contains("JSON"))
  {
    return 2;  // input error
  }
  return 3;  // internal/solver error
}

}  // namespace gtopt
