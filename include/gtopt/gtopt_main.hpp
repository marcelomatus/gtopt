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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

/**
 * @brief All command-line options consumed by gtopt_main().
 *
 * Every field is optional so that callers only set what they need and can use
 * designated-initializer syntax:
 * @code
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
  /** @brief LP naming level: minimal, only_cols, cols_and_rows */
  std::optional<LpNamesLevel> lp_names_level {};
  /** @brief Epsilon tolerance for LP matrix coefficients */
  std::optional<double> matrix_eps {};
  /** @brief Build all scene/phase LP matrices but skip solving entirely */
  std::optional<bool> lp_build {};
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
  /** @brief Use fast (non-strict) JSON parsing */
  std::optional<bool> fast_parsing {};
  /** @brief Warn about JSON fields not recognised by the schema */
  std::optional<bool> check_json {};
  /** @brief Print pre- and post-solve system statistics */
  std::optional<bool> print_stats {};

  // ---- tracing / diagnostics ----
  /** @brief Path to a file for SPDLOG_TRACE output (enables trace-level
   * logging).  When set, a dedicated file sink is added to spdlog so that
   * all trace-level messages are captured for later review. */
  std::optional<std::string> trace_log {};

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
  /** @brief SDDP elastic filter mode: "cut" (default) or "backpropagate" */
  std::optional<std::string> sddp_elastic_mode {};
  /** @brief SDDP cut coefficient source: "reduced_cost" (default) or
   * "row_dual" (PLP-style explicit coupling constraint rows) */
  std::optional<std::string> sddp_cut_coeff_mode {};
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

  // ---- solver selection ----
  /** @brief LP solver backend name ("clp", "cbc", "cplex", "highs").
   * When empty, auto-detects from available plugins. */
  std::optional<std::string> solver {};

  // ---- solver algorithm (shortcuts for solver_options fields) ----
  /** @brief LP solution algorithm override (0=default, 1=primal, 2=dual,
   * 3=barrier).  Mapped to solver_options.algorithm by apply_cli_options. */
  std::optional<int> algorithm {};
  /** @brief Number of solver threads override (0=automatic).
   *  Mapped to solver_options.threads by apply_cli_options. */
  std::optional<int> threads {};

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
