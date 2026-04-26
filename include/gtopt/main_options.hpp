/**
 * @file      main_options.hpp
 * @brief     Application command-line option parsing and configuration
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides utility functions for parsing command-line options
 * using a modern C++ command-line parser, applying parsed options to Planning
 * configurations, and building LpMatrixOptions from command-line parameters.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtopt/cli_options.hpp>
#include <gtopt/config_file.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/solver_options.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace po = cli;

/**
 * @brief Extract an optional value from a variables_map
 *
 * @tparam T The type of the value to extract
 * @param vm The variables map containing parsed options
 * @param name The name of the option to look up
 * @return std::optional<T> containing the value if present, std::nullopt
 * otherwise
 */
template<typename T>
[[nodiscard]] std::optional<T> get_opt(const po::variables_map& vm,
                                       const std::string& name)
{
  if (vm.contains(name)) {
    return vm[name].as<T>();
  }
  return std::nullopt;
}

/**
 * @brief Parse an LP algorithm value from a string (name or integer).
 *
 * Accepts either a numeric string ("0"–"3") or a case-sensitive algorithm
 * name ("default", "primal", "dual", "barrier").  The name lookup is
 * driven by @c lp_algo_entries – the same compile-time table used for
 * logging – so the two are always in sync.  With C++26 P2996 static
 * reflection that table would itself be generated automatically from the
 * @c LPAlgo enum, making this function fully reflection-driven.
 *
 * @param s The string to parse.
 * @return The corresponding integer value for the algorithm.
 * @throws cli::parse_error on unrecognised input.
 */
[[nodiscard]] inline LPAlgo parse_lp_algorithm(std::string_view s)
{
  // Name-based lookup via the constexpr table in solver_enums.hpp.
  if (const auto algo = enum_from_name<LPAlgo>(s)) {
    return *algo;
  }
  // Numeric fallback: exactly one digit, "0"–"3"
  if (s.size() == 1 && std::isdigit(static_cast<unsigned char>(s.front())) != 0)
  {
    const int v = s.front() - '0';
    if (v >= 0 && v < static_cast<int>(LPAlgo::last_algo)) {
      return static_cast<LPAlgo>(v);
    }
  }
  throw cli::parse_error(
      std::format("invalid lp-algorithm value: '{}' "
                  "(expected 0-3 or default/primal/dual/barrier)",
                  s));
}

/**
 * @brief Parse a memory size string into megabytes.
 *
 * Accepts a plain number (interpreted as MB) or a number with suffix:
 * "M"/"MB" for megabytes, "G"/"GB" for gigabytes.
 * Examples: "4096", "300M", "5G", "1.5GB".
 *
 * @param s The string to parse.
 * @return Size in megabytes.
 * @throws cli::parse_error on unrecognised input.
 */
[[nodiscard]] inline double parse_memory_size(const std::string& s)
{
  if (s.empty()) {
    return 0.0;
  }
  double value = 0.0;
  size_t pos = 0;
  try {
    value = std::stod(s, &pos);
  } catch (...) {
    throw cli::parse_error(
        std::format("invalid memory size: '{}' (expected number with optional "
                    "M/G suffix)",
                    s));
  }
  auto suffix = s.substr(pos);
  // Strip leading whitespace
  while (!suffix.empty() && suffix.front() == ' ') {
    suffix.erase(suffix.begin());
  }
  if (suffix.empty() || suffix == "M" || suffix == "MB" || suffix == "m"
      || suffix == "mb")
  {
    return value;  // already in MB
  }
  if (suffix == "G" || suffix == "GB" || suffix == "g" || suffix == "gb") {
    return value * 1024.0;
  }
  throw cli::parse_error(std::format(
      "invalid memory size suffix: '{}' (expected M, MB, G, or GB)", suffix));
}

/**
 * @brief Create the command-line options description for the gtopt application
 *
 * @return po::options_description The options description containing all
 * supported options
 */
[[nodiscard]] inline po::options_description make_options_description()
{
  po::options_description desc("Gtopt options");
  desc.add_options()("help,h", "print this help message and exit")  //
      ("solvers", "list available LP solver backends and exit")  //
      ("check-solvers",
       po::value<std::string>().implicit_value(""),
       "run the LinearInterface test suite against all solvers, or a specific "
       "solver if a name is given (e.g. --check-solvers clp), then exit")  //
      ("solver",
       po::value<std::string>(),
       "LP solver backend: cplex, highs, cbc, clp "
       "(default: auto-detect by priority cplex > highs > cbc > clp)")  //
      ("verbose,v", "enable maximum log verbosity (trace level)")  //
      ("quiet,q",
       po::value<bool>().implicit_value(/*v=*/true),
       "suppress all log output to stdout")  //
      ("version,V", "print program version and exit")  //
      ("system-file,s",
       po::value<std::vector<std::string>>(),
       "planning file(s) (planning.json); may be a JSON file, a stem "
       "(without .json), or a directory name")  //
      ("set",
       po::value<std::vector<std::string>>(),
       "set any Planning option via dotted path (repeatable). "
       "Example: --set sddp_options.forward_solver_options.threads=8. "
       "Values are auto-typed (bool/int/double/string)")  //
      // ---- LP options ----
      ("lp-file,l",
       po::value<std::string>(),
       "write the assembled LP model to this file (stem; .lp extension added)")
      //
      ("matrix-eps,e",
       po::value<double>(),
       "epsilon threshold for treating LP matrix coefficients as zero")  //
      ("lp-only,c",
       po::value<bool>().implicit_value(/*v=*/true),
       "build all LP matrices then exit without solving (combine with -l to "
       "save them)")  //
      // ---- debug / output helpers ----
      ("json-file,j",
       po::value<std::string>(),
       "write the merged planning JSON to this file")  //
      ("stats,S",
       po::value<bool>().implicit_value(/*v=*/true),
       "print LP coefficient statistics and system stats before/after solving")
      //
      ("trace-log,T",
       po::value<std::string>(),
       "write SPDLOG_TRACE messages to this file (enables trace-level logging)")
      //
      ("sddp-num-apertures",
       po::value<int>(),
       "SDDP backward-pass aperture count: 0=disabled (default), -1=all, "
       "N=first N scenarios")  //
      ("recover",
       po::value<bool>().implicit_value(/*v=*/true),
       "enable recovery from a previous SDDP run (loads cuts and state "
       "variables according to JSON recovery_mode; default: off)")  //
      ("memory-saving",
       po::value<std::string>().implicit_value("compress"),
       "coordinated memory-saving: sets both the SDDP flat-LP release "
       "policy (sddp_options.low_memory_mode) AND the solver-native "
       "memory hint (solver_options.memory_emphasis, e.g. CPLEX "
       "CPX_PARAM_MEMORYEMPHASIS).  Values: off, "
       "compress (release solver, keep compressed flat LP — best "
       "balance, default when flag is given without value), "
       "rebuild (re-build base LP every solve — lowest RAM, highest "
       "CPU).  Overridden by direct JSON settings for either option.")  //
      // Deprecated alias for `--memory-saving` — hidden from help.
      ("low-memory",
       po::value<std::string>().implicit_value("compress"),
       "")  //
      ("memory-limit",
       po::value<std::string>(),
       "process memory limit for work pool throttling "
       "(e.g. 4096, 300M, 5G)")  //
      ("cpu-factor",
       po::value<double>(),
       "work pool thread over-commit factor (default: 4.0)")  //
      ("write-out",
       po::value<std::string>(),
       "comma-separated list of output fields the solver should emit "
       "(default: all).  Atoms: solution (alias: sol), dual, "
       "reduced_cost (aliases: cost, rcost, rc), all, none.  "
       "Example: --write-out sol,dual  or  --write-out all")  //
      ("build-mode",
       po::value<std::string>(),
       "LP build parallelism: serial, scene-parallel, full-parallel, "
       "direct-parallel (default: scene-parallel)")  //
      ("no-scale",
       po::value<bool>().implicit_value(/*v=*/true),
       "disable every automatic LP scaling / equilibration (sets "
       "model_options.scale_objective=1.0, scale_theta=1.0, and "
       "lp_matrix_options.equilibration_method=none).  Intended for "
       "debug / physical-unit validation of coefficients and RHS.  "
       "JSON settings still take precedence when present.")  //
      // ---- deprecated options (hidden from help, still parsed) ----
      ("sddp-cpu-factor", po::value<double>(), "")  //
      ("input-directory,D", po::value<std::string>(), "")  //
      ("input-format,F", po::value<std::string>(), "")  //
      ("output-directory,d", po::value<std::string>(), "")  //
      ("output-format,f", po::value<std::string>(), "")  //
      ("output-compression,C", po::value<std::string>(), "")  //
      ("use-single-bus,b",
       po::value<bool>().implicit_value(/*v=*/true),
       "")  //
      ("use-kirchhoff,k",
       po::value<bool>().implicit_value(/*v=*/true),
       "")  //
      ("lp-debug",
       po::value<bool>().implicit_value(/*v=*/true),
       "")  //
      ("lp-compression", po::value<std::string>(), "")  //
      ("lp-coeff-ratio", po::value<double>(), "")  //
      ("algorithm,a", po::value<std::string>(), "")  //
      ("threads,t", po::value<int>(), "")  //
      ("cut-directory", po::value<std::string>(), "")  //
      ("log-directory", po::value<std::string>(), "")  //
      ("sddp-max-iterations", po::value<int>(), "")  //
      ("sddp-min-iterations", po::value<int>(), "")  //
      ("sddp-convergence-tol", po::value<double>(), "")  //
      ("sddp-elastic-penalty", po::value<double>(), "")  //
      ("sddp-elastic-mode", po::value<std::string>(), "");
  return desc;
}

/**
 * @brief Apply command-line options to a Planning object
 *
 * Updates the planning options based on parsed command-line values.
 *
 * @param planning The Planning object to update
 * @param use_single_bus Optional single-bus mode flag
 * @param use_kirchhoff Optional Kirchhoff mode flag
 * @param input_directory Optional input directory path
 * @param input_format Optional input format string
 * @param output_directory Optional output directory path
 * @param output_format Optional output format string
 * @param output_compression Optional compression codec string
 * @param cut_directory Optional directory for SDDP cut files
 * @param log_directory Optional directory for log/debug LP files
 * @param sddp_max_iterations Optional SDDP max iterations
 * @param sddp_convergence_tol Optional SDDP convergence tolerance
 * @param sddp_elastic_penalty Optional elastic penalty coefficient
 * @param sddp_elastic_mode Optional elastic filter mode string
 * @param sddp_num_apertures Optional number of SDDP apertures
 * @param lp_debug Optional flag to enable LP debug output
 * @param lp_compression Optional LP output compression codec string
 * @param lp_coeff_ratio_threshold Optional threshold for LP coefficient ratio
 */
inline void apply_cli_options(
    Planning& planning,  // NOLINT(misc-const-correctness)
    const std::optional<bool>& use_single_bus,
    const std::optional<bool>& use_kirchhoff,
    const std::optional<std::string>& input_directory,
    const std::optional<std::string>& input_format,
    const std::optional<std::string>& output_directory,
    const std::optional<std::string>& output_format,
    const std::optional<std::string>& output_compression,
    const std::optional<std::string>& cut_directory = {},
    const std::optional<std::string>& log_directory = {},
    const std::optional<int>& sddp_max_iterations = {},
    const std::optional<double>& sddp_convergence_tol = {},
    const std::optional<double>& sddp_elastic_penalty = {},
    const std::optional<std::string>& sddp_elastic_mode = {},
    const std::optional<int>& sddp_num_apertures = {},
    const std::optional<bool>& lp_debug = {},
    const std::optional<std::string>& lp_compression = {},
    const std::optional<double>& lp_coeff_ratio_threshold = {})
{
  if (use_single_bus) {
    planning.options.use_single_bus = use_single_bus;
  }

  if (use_kirchhoff) {
    planning.options.use_kirchhoff = use_kirchhoff;
  }

  if (output_directory) {
    planning.options.output_directory = output_directory.value();
  }

  if (input_directory) {
    planning.options.input_directory = input_directory.value();
  }

  if (output_format) {
    planning.options.output_format =
        require_enum<DataFormat>("output-format", output_format.value());
  }

  if (output_compression) {
    planning.options.output_compression = require_enum<CompressionCodec>(
        "output-compression", output_compression.value());
  }

  if (input_format) {
    planning.options.input_format =
        require_enum<DataFormat>("input-format", input_format.value());
  }

  if (cut_directory) {
    planning.options.sddp_options.cut_directory = cut_directory.value();
  }

  if (log_directory) {
    planning.options.log_directory = log_directory.value();
  }

  if (sddp_max_iterations) {
    planning.options.sddp_options.max_iterations = sddp_max_iterations;
  }

  if (sddp_convergence_tol) {
    planning.options.sddp_options.convergence_tol = sddp_convergence_tol;
  }

  if (sddp_elastic_penalty) {
    planning.options.sddp_options.elastic_penalty = sddp_elastic_penalty;
  }

  if (sddp_elastic_mode) {
    planning.options.sddp_options.elastic_mode =
        require_enum<ElasticFilterMode>("sddp-elastic-mode",
                                        sddp_elastic_mode.value());
  }

  if (sddp_num_apertures) {
    // Legacy CLI: convert num_apertures int to apertures array.
    // 0 → empty (no apertures), >0 or <0 handled at solve time.
    if (*sddp_num_apertures == 0) {
      planning.options.sddp_options.apertures = Array<Uid> {};
    }
    // Non-zero: leave apertures as nullopt (use per-phase apertures)
  }

  if (lp_debug) {
    planning.options.lp_debug = lp_debug;
  }

  if (lp_compression) {
    planning.options.lp_compression = require_enum<CompressionCodec>(
        "lp-compression", lp_compression.value());
  }

  if (lp_coeff_ratio_threshold) {
    planning.options.lp_matrix_options.lp_coeff_ratio_threshold =
        lp_coeff_ratio_threshold;
  }
}

/// @brief Emit a deprecation warning for a CLI option replaceable by --set.
/// @param opt       The optional value to check (warning only if it has a
/// value)
/// @param cli_flag  The deprecated CLI flag name
/// @param set_path  The --set key path that replaces it
template<typename T>
void warn_deprecated_cli(const std::optional<T>& opt,
                         std::string_view cli_flag,
                         std::string_view set_path)
{
  if (opt.has_value()) {
    spdlog::warn(
        "--{} is deprecated, use: --set {}={}", cli_flag, set_path, *opt);
  }
}

/// @brief Specialisation for bool (prints true/false instead of 1/0).
inline void warn_deprecated_cli(const std::optional<bool>& opt,
                                std::string_view cli_flag,
                                std::string_view set_path)
{
  if (opt.has_value()) {
    spdlog::warn("--{} is deprecated, use: --set {}={}",
                 cli_flag,
                 set_path,
                 *opt ? "true" : "false");
  }
}

/// @brief Overload for string options — no conversion needed.
inline void warn_deprecated_cli(const std::optional<std::string>& opt,
                                std::string_view cli_flag,
                                std::string_view set_path)
{
  if (opt.has_value()) {
    spdlog::warn(
        "--{} is deprecated, use: --set {}={}", cli_flag, set_path, *opt);
  }
}

/**
 * @brief Apply command-line options from a MainOptions struct to a Planning
 * object
 *
 * Convenience overload that takes a @c MainOptions struct directly, delegating
 * to the individual-parameter overload.
 *
 * @param planning The Planning object to update
 * @param opts     The MainOptions containing the option overrides
 */
inline void apply_cli_options(Planning& planning, const MainOptions& opts)
{
  // Emit deprecation warnings for options replaceable by --set.
  // Skip directory warnings when auto-resolved from a directory argument
  // (these are internal, not user-provided CLI flags).
  warn_deprecated_cli(opts.use_single_bus, "use-single-bus", "use_single_bus");
  warn_deprecated_cli(opts.use_kirchhoff, "use-kirchhoff", "use_kirchhoff");
  if (!opts.dirs_auto_resolved) {
    warn_deprecated_cli(
        opts.input_directory, "input-directory", "input_directory");
    warn_deprecated_cli(
        opts.output_directory, "output-directory", "output_directory");
    warn_deprecated_cli(
        opts.cut_directory, "cut-directory", "sddp_options.cut_directory");
    warn_deprecated_cli(opts.log_directory, "log-directory", "log_directory");
  }
  warn_deprecated_cli(opts.input_format, "input-format", "input_format");
  warn_deprecated_cli(opts.output_format, "output-format", "output_format");
  warn_deprecated_cli(
      opts.output_compression, "output-compression", "output_compression");
  warn_deprecated_cli(opts.sddp_max_iterations,
                      "sddp-max-iterations",
                      "sddp_options.max_iterations");
  warn_deprecated_cli(opts.sddp_min_iterations,
                      "sddp-min-iterations",
                      "sddp_options.min_iterations");
  warn_deprecated_cli(opts.sddp_convergence_tol,
                      "sddp-convergence-tol",
                      "sddp_options.convergence_tol");
  warn_deprecated_cli(opts.sddp_elastic_penalty,
                      "sddp-elastic-penalty",
                      "sddp_options.elastic_penalty");
  warn_deprecated_cli(
      opts.sddp_elastic_mode, "sddp-elastic-mode", "sddp_options.elastic_mode");
  if (opts.algorithm.has_value()) {
    spdlog::warn(
        "--algorithm is deprecated, use: --set solver_options"
        ".algorithm={}",
        enum_name(*opts.algorithm));
  }
  warn_deprecated_cli(opts.threads, "threads", "solver_options.threads");
  warn_deprecated_cli(opts.lp_debug, "lp-debug", "lp_debug");
  warn_deprecated_cli(opts.lp_compression, "lp-compression", "lp_compression");
  warn_deprecated_cli(opts.lp_coeff_ratio_threshold,
                      "lp-coeff-ratio",
                      "lp_matrix_options.lp_coeff_ratio_threshold");

  apply_cli_options(planning,
                    opts.use_single_bus,
                    opts.use_kirchhoff,
                    opts.input_directory,
                    opts.input_format,
                    opts.output_directory,
                    opts.output_format,
                    opts.output_compression,
                    opts.cut_directory,
                    opts.log_directory,
                    opts.sddp_max_iterations,
                    opts.sddp_convergence_tol,
                    opts.sddp_elastic_penalty,
                    opts.sddp_elastic_mode,
                    opts.sddp_num_apertures,
                    opts.lp_debug,
                    opts.lp_compression,
                    opts.lp_coeff_ratio_threshold);

  // Additional SDDP options not in the positional overload
  if (opts.sddp_min_iterations) {
    planning.options.sddp_options.min_iterations = opts.sddp_min_iterations;
  }
  if (opts.sddp_hot_start) {
    planning.options.sddp_options.cut_recovery_mode =
        *opts.sddp_hot_start ? HotStartMode::replace : HotStartMode::none;
  }

  // --recover gates whether recovery happens at all.
  // When not passed (or explicitly false), force recovery_mode to "none"
  // so JSON config alone cannot trigger recovery.
  if (!opts.recover.value_or(false)) {
    planning.options.sddp_options.recovery_mode = RecoveryMode::none;
  }

  if (opts.no_scale.value_or(false)) {
    // `--no-scale` disables every auto-scaling / equilibration
    // mechanism for debug / physical-unit validation.  CLI flag wins
    // over JSON: an explicit `scale_objective: 1000` in the planning
    // file would otherwise silently survive `--no-scale` (observed on
    // juan/gtopt_iplp where SDDP cuts compounded by 1000× per backward
    // phase due to scale_objective=1000 being applied to physical-
    // space cut rows).
    planning.options.model_options.scale_objective = 1.0;
    planning.options.model_options.scale_theta = 1.0;
    planning.options.lp_matrix_options.equilibration_method =
        LpEquilibrationMethod::none;
    // Kill switch for the per-element auto-scale heuristics in
    // `PlanningLP::auto_scale_theta / _reservoirs / _lng_terminals`
    // — without this the reservoir energy/flow variable_scales would
    // still be computed and applied at LP construction.
    planning.options.model_options.auto_scale = false;
  }

  if (opts.memory_saving) {
    // Map the string to the LowMemoryMode enum and apply to the SDDP
    // side of the equation.
    planning.options.sddp_options.low_memory_mode =
        require_enum<LowMemoryMode>("memory-saving", *opts.memory_saving);
    // Coordinated effect #2: hint the solver backends to compact
    // internal data structures (CPLEX CPX_PARAM_MEMORYEMPHASIS=1; other
    // backends silently ignore when they have no equivalent).  Only
    // write the default when the user hasn't already set memory_emphasis
    // explicitly in the planning JSON — JSON takes precedence.
    if (*opts.memory_saving != "off"
        && !planning.options.solver_options.memory_emphasis.has_value())
    {
      planning.options.solver_options.memory_emphasis = true;
    }
  }

  if (opts.memory_limit) {
    planning.options.sddp_options.pool_memory_limit_mb =
        parse_memory_size(*opts.memory_limit);
  }

  if (opts.sddp_cpu_factor) {
    planning.options.sddp_options.pool_cpu_factor = opts.sddp_cpu_factor;
  }

  if (opts.build_mode) {
    planning.options.build_mode =
        require_enum<BuildMode>("build-mode", *opts.build_mode);
  }

  if (opts.write_out) {
    planning.options.write_out = parse_output_flags(*opts.write_out);
  }

  // CLI solver shortcuts → solver_options
  if (opts.algorithm) {
    planning.options.solver_options.algorithm = *opts.algorithm;
  }
  if (opts.threads) {
    planning.options.solver_options.threads = *opts.threads;
  }
}

/**
 * @brief Build LpMatrixOptions from internal parameters
 *
 * @param enable_names         Whether to enable column/row name generation
 * @param matrix_eps           Optional epsilon tolerance for matrix
 *                             coefficients
 * @param compute_stats        Whether to compute LP statistics (default
 *                             false)
 * @param lp_solver            Optional solver name to use
 * @param equilibration_method Optional equilibration method
 * @return LpMatrixOptions configured according to the parameters
 */
[[nodiscard]] inline LpMatrixOptions make_lp_matrix_options(
    bool enable_names,
    const std::optional<double>& matrix_eps,
    bool compute_stats = false,
    const std::optional<std::string>& lp_solver = {},
    std::optional<LpEquilibrationMethod> equilibration_method = {})
{
  LpMatrixOptions lp_matrix_opts;
  lp_matrix_opts.eps = matrix_eps.value_or(0);
  lp_matrix_opts.col_with_names = enable_names;
  lp_matrix_opts.row_with_names = enable_names;
  lp_matrix_opts.col_with_name_map = enable_names;
  lp_matrix_opts.row_with_name_map = enable_names;
  lp_matrix_opts.compute_stats = compute_stats;
  lp_matrix_opts.solver_name = lp_solver.value_or("");
  lp_matrix_opts.equilibration_method = equilibration_method;

  return lp_matrix_opts;
}

/**
 * @brief Build a MainOptions struct from a parsed CLI variables_map.
 *
 * Extracts every gtopt_main option from @p vm into a @c MainOptions value.
 * @p system_files is taken from the positional arguments already pulled out
 * by the caller (they are not stored in @p vm by default).
 *
 * @param vm           Parsed CLI variables map (from po::store/po::notify)
 * @param system_files Positional system-file arguments
 * @return Fully populated MainOptions
 */
[[nodiscard]] inline MainOptions parse_main_options(
    const po::variables_map& vm, std::vector<std::string> system_files)
{
  return MainOptions {
      .planning_files = std::move(system_files),
      .input_directory = get_opt<std::string>(vm, "input-directory"),
      .input_format = get_opt<std::string>(vm, "input-format"),
      .output_directory = get_opt<std::string>(vm, "output-directory"),
      .output_format = get_opt<std::string>(vm, "output-format"),
      .output_compression = get_opt<std::string>(vm, "output-compression"),
      .use_single_bus = get_opt<bool>(vm, "use-single-bus"),
      .use_kirchhoff = get_opt<bool>(vm, "use-kirchhoff"),
      .lp_file = get_opt<std::string>(vm, "lp-file"),
      .matrix_eps = get_opt<double>(vm, "matrix-eps"),
      .lp_only = get_opt<bool>(vm, "lp-only"),
      .lp_debug = get_opt<bool>(vm, "lp-debug"),
      .lp_compression = get_opt<std::string>(vm, "lp-compression"),
      .lp_coeff_ratio_threshold = get_opt<double>(vm, "lp-coeff-ratio"),
      .json_file = get_opt<std::string>(vm, "json-file"),
      .print_stats = get_opt<bool>(vm, "stats"),
      .trace_log = get_opt<std::string>(vm, "trace-log"),
      .cut_directory = get_opt<std::string>(vm, "cut-directory"),
      .log_directory = get_opt<std::string>(vm, "log-directory"),
      .sddp_max_iterations = get_opt<int>(vm, "sddp-max-iterations"),
      .sddp_min_iterations = get_opt<int>(vm, "sddp-min-iterations"),
      .sddp_convergence_tol = get_opt<double>(vm, "sddp-convergence-tol"),
      .sddp_elastic_penalty = get_opt<double>(vm, "sddp-elastic-penalty"),
      .sddp_elastic_mode = get_opt<std::string>(vm, "sddp-elastic-mode"),
      .sddp_num_apertures = get_opt<int>(vm, "sddp-num-apertures"),
      .recover = get_opt<bool>(vm, "recover"),
      // Prefer the new `--memory-saving` name; fall back to the
      // deprecated `--low-memory` alias for backward compatibility.
      .memory_saving =
          get_opt<std::string>(vm, "memory-saving")
              .or_else([&] { return get_opt<std::string>(vm, "low-memory"); }),
      .memory_limit = get_opt<std::string>(vm, "memory-limit"),
      .sddp_cpu_factor =
          get_opt<double>(vm, "cpu-factor")
              .or_else([&] { return get_opt<double>(vm, "sddp-cpu-factor"); }),
      .build_mode = get_opt<std::string>(vm, "build-mode"),
      .write_out = get_opt<std::string>(vm, "write-out"),
      .solver = get_opt<std::string>(vm, "solver"),
      .algorithm = [&]() -> std::optional<LPAlgo>
      {
        if (const auto raw = get_opt<std::string>(vm, "algorithm")) {
          return parse_lp_algorithm(*raw);
        }
        return std::nullopt;
      }(),
      .threads = get_opt<int>(vm, "threads"),
      .no_scale = get_opt<bool>(vm, "no-scale"),
      .set_options = vm.contains("set")
          ? vm["set"].as<std::vector<std::string>>()
          : std::vector<std::string> {},
  };
}

// ── Config file support ────────────────────────────────────────────────────

/**
 * @brief Load a MainOptions from the `[gtopt]` section of `.gtopt.conf`.
 *
 * Reads the config file found by @c find_config_file() and extracts
 * values from the `[gtopt]` section.  Keys use kebab-case matching
 * the CLI flag names (e.g. `output-format`, `sddp-max-iterations`).
 *
 * @return MainOptions with fields populated from the config file.
 *         Fields not present in the config file remain as nullopt.
 */
[[nodiscard]] inline MainOptions load_gtopt_config()
{
  MainOptions opts;

  const auto config_path = find_config_file();
  if (config_path.empty()) {
    return opts;
  }

  const auto ini = parse_ini_file(config_path);
  const auto it = ini.find("gtopt");
  if (it == ini.end()) {
    return opts;
  }

  const auto& section = it->second;

  // Helper: get string value if present
  auto get_str = [&](const std::string& key) -> std::optional<std::string>
  {
    if (const auto kv = section.find(key); kv != section.end()) {
      if (!kv->second.empty()) {
        return kv->second;
      }
    }
    return std::nullopt;
  };

  // Helper: get bool value if present
  auto get_bool = [&](const std::string& key) -> std::optional<bool>
  {
    if (const auto kv = section.find(key); kv != section.end()) {
      const auto& v = kv->second;
      if (v == "true" || v == "1" || v == "yes") {
        return true;
      }
      if (v == "false" || v == "0" || v == "no") {
        return false;
      }
    }
    return std::nullopt;
  };

  // Helper: get int value if present
  auto get_int = [&](const std::string& key) -> std::optional<int>
  {
    if (const auto kv = section.find(key); kv != section.end()) {
      try {
        return std::stoi(kv->second);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
    return std::nullopt;
  };

  // Helper: get double value if present
  auto get_dbl = [&](const std::string& key) -> std::optional<double>
  {
    if (const auto kv = section.find(key); kv != section.end()) {
      try {
        return std::stod(kv->second);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
    return std::nullopt;
  };

  // I/O directories / formats
  opts.input_directory = get_str("input-directory");
  opts.input_format = get_str("input-format");
  opts.output_directory = get_str("output-directory");
  opts.output_format = get_str("output-format");
  opts.output_compression = get_str("output-compression");

  // Modelling flags
  opts.use_single_bus = get_bool("use-single-bus");
  opts.use_kirchhoff = get_bool("use-kirchhoff");

  // LP options
  opts.lp_file = get_str("lp-file");
  opts.matrix_eps = get_dbl("matrix-eps");
  opts.lp_only = get_bool("lp-only");
  opts.lp_debug = get_bool("lp-debug");
  opts.lp_compression = get_str("lp-compression");
  opts.lp_coeff_ratio_threshold = get_dbl("lp-coeff-ratio");

  // Debug / output
  opts.json_file = get_str("json-file");
  opts.print_stats = get_bool("stats");
  opts.trace_log = get_str("trace-log");

  // SDDP directories
  opts.cut_directory = get_str("cut-directory");
  opts.log_directory = get_str("log-directory");

  // SDDP tuning
  opts.sddp_max_iterations = get_int("sddp-max-iterations");
  opts.sddp_min_iterations = get_int("sddp-min-iterations");
  opts.sddp_convergence_tol = get_dbl("sddp-convergence-tol");
  opts.sddp_elastic_penalty = get_dbl("sddp-elastic-penalty");
  opts.sddp_elastic_mode = get_str("sddp-elastic-mode");
  opts.sddp_num_apertures = get_int("sddp-num-apertures");
  // Prefer the new key; fall back to the deprecated `low-memory` alias.
  opts.memory_saving = get_str("memory-saving");
  if (!opts.memory_saving) {
    opts.memory_saving = get_str("low-memory");
  }
  opts.memory_limit = get_str("memory-limit");
  opts.sddp_cpu_factor = get_dbl("cpu-factor");
  if (!opts.sddp_cpu_factor) {
    opts.sddp_cpu_factor = get_dbl("sddp-cpu-factor");
  }
  opts.build_mode = get_str("build-mode");
  opts.write_out = get_str("write-out");

  // Solver
  opts.solver = get_str("solver");
  if (const auto raw = get_str("algorithm")) {
    try {
      opts.algorithm = parse_lp_algorithm(*raw);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  }
  opts.threads = get_int("threads");

  // Per-solver configuration: [solver.cplex], [solver.highs], [solver.clp]
  for (const auto& [sec_name, sec_map] : ini) {
    if (!sec_name.starts_with("solver.")) {
      continue;
    }
    const auto solver_name = sec_name.substr(7);  // strip "solver."
    if (solver_name.empty()) {
      continue;
    }

    // Helper lambdas bound to this section
    auto sv_str = [&](const std::string& key) -> std::optional<std::string>
    {
      if (const auto kv = sec_map.find(key); kv != sec_map.end()) {
        if (!kv->second.empty()) {
          return kv->second;
        }
      }
      return std::nullopt;
    };
    auto sv_int = [&](const std::string& key) -> std::optional<int>
    {
      if (const auto kv = sec_map.find(key); kv != sec_map.end()) {
        try {
          return std::stoi(kv->second);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      }
      return std::nullopt;
    };
    auto sv_dbl = [&](const std::string& key) -> std::optional<double>
    {
      if (const auto kv = sec_map.find(key); kv != sec_map.end()) {
        try {
          return std::stod(kv->second);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      }
      return std::nullopt;
    };
    auto sv_bool = [&](const std::string& key) -> std::optional<bool>
    {
      if (const auto kv = sec_map.find(key); kv != sec_map.end()) {
        const auto& v = kv->second;
        if (v == "true" || v == "1" || v == "yes") {
          return true;
        }
        if (v == "false" || v == "0" || v == "no") {
          return false;
        }
      }
      return std::nullopt;
    };

    SolverOptions sopts;
    if (const auto raw = sv_str("algorithm")) {
      try {
        sopts.algorithm = parse_lp_algorithm(*raw);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
    if (const auto v = sv_int("threads")) {
      sopts.threads = *v;
    }
    if (const auto v = sv_bool("presolve")) {
      sopts.presolve = *v;
    }
    sopts.optimal_eps = sv_dbl("optimal-eps");
    sopts.feasible_eps = sv_dbl("feasible-eps");
    sopts.barrier_eps = sv_dbl("barrier-eps");
    if (const auto v = sv_int("log-level")) {
      sopts.log_level = *v;
    }
    sopts.time_limit = sv_dbl("time-limit");
    if (const auto raw = sv_str("scaling")) {
      sopts.scaling = require_enum<SolverScaling>("scaling", *raw);
    }
    if (const auto v = sv_int("max-fallbacks")) {
      sopts.max_fallbacks = *v;
    }

    opts.solver_configs[solver_name] = sopts;
  }

  return opts;
}

/**
 * @brief Merge config-file defaults into a MainOptions struct.
 *
 * For each field in @p opts that is not set (nullopt / empty), copies
 * the value from @p defaults.  CLI-set fields are never overwritten.
 *
 * @param opts     The primary options (typically from CLI parsing).
 * @param defaults The fallback options (typically from config file).
 */
inline void merge_config_defaults(MainOptions& opts,
                                  const MainOptions& defaults)
{
  auto merge =
      []<typename T>(std::optional<T>& dst, const std::optional<T>& src)
  {
    if (!dst.has_value() && src.has_value()) {
      dst = src;
    }
  };

  merge(opts.input_directory, defaults.input_directory);
  merge(opts.input_format, defaults.input_format);
  merge(opts.output_directory, defaults.output_directory);
  merge(opts.output_format, defaults.output_format);
  merge(opts.output_compression, defaults.output_compression);
  merge(opts.use_single_bus, defaults.use_single_bus);
  merge(opts.use_kirchhoff, defaults.use_kirchhoff);
  merge(opts.lp_file, defaults.lp_file);
  merge(opts.matrix_eps, defaults.matrix_eps);
  merge(opts.lp_only, defaults.lp_only);
  merge(opts.lp_debug, defaults.lp_debug);
  merge(opts.lp_compression, defaults.lp_compression);
  merge(opts.lp_coeff_ratio_threshold, defaults.lp_coeff_ratio_threshold);
  merge(opts.json_file, defaults.json_file);
  merge(opts.print_stats, defaults.print_stats);
  merge(opts.trace_log, defaults.trace_log);
  merge(opts.cut_directory, defaults.cut_directory);
  merge(opts.log_directory, defaults.log_directory);
  merge(opts.sddp_max_iterations, defaults.sddp_max_iterations);
  merge(opts.sddp_min_iterations, defaults.sddp_min_iterations);
  merge(opts.sddp_convergence_tol, defaults.sddp_convergence_tol);
  merge(opts.sddp_elastic_penalty, defaults.sddp_elastic_penalty);
  merge(opts.sddp_elastic_mode, defaults.sddp_elastic_mode);
  merge(opts.sddp_num_apertures, defaults.sddp_num_apertures);
  merge(opts.memory_saving, defaults.memory_saving);
  merge(opts.no_scale, defaults.no_scale);
  merge(opts.memory_limit, defaults.memory_limit);
  merge(opts.sddp_cpu_factor, defaults.sddp_cpu_factor);
  merge(opts.build_mode, defaults.build_mode);
  merge(opts.write_out, defaults.write_out);
  merge(opts.solver, defaults.solver);
  merge(opts.algorithm, defaults.algorithm);
  merge(opts.threads, defaults.threads);

  // Per-solver configs: merge defaults into opts (don't overwrite existing)
  for (const auto& [name, sopts] : defaults.solver_configs) {
    if (!opts.solver_configs.contains(name)) {
      opts.solver_configs[name] = sopts;
    }
  }
}

}  // namespace gtopt
