/**
 * @file      main_options.hpp
 * @brief     Application command-line option parsing and configuration
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides utility functions for parsing command-line options
 * using a modern C++ command-line parser, applying parsed options to Planning
 * configurations, and building LpBuildOptions from command-line parameters.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtopt/cli_options.hpp>
#include <gtopt/config_file.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/solver_options.hpp>

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
[[nodiscard]] inline int parse_lp_algorithm(const std::string& s)
{
  // Name-based lookup via the constexpr table in solver_options.hpp.
  if (const auto algo = lp_algo_from_name(s)) {
    return static_cast<int>(*algo);
  }
  // Numeric fallback: exactly one digit, "0"–"3"
  if (s.size() == 1 && std::isdigit(static_cast<unsigned char>(s.front())) != 0)
  {
    const int v = s.front() - '0';
    if (v >= 0 && v < static_cast<int>(LPAlgo::last_algo)) {
      return v;
    }
  }
  throw cli::parse_error(
      std::format("invalid lp-algorithm value: '{}' "
                  "(expected 0-3 or default/primal/dual/barrier)",
                  s));
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
      ("solver",
       po::value<std::string>(),
       "LP solver backend: clp (default), cbc, cplex, highs")  //
      ("verbose,v", "enable maximum log verbosity (trace level)")  //
      ("quiet,q",
       po::value<bool>().implicit_value(/*v=*/true),
       "suppress all log output to stdout")  //
      ("version,V", "print program version and exit")  //
      ("system-file,s",
       po::value<std::vector<std::string>>(),
       "planning file(s) (planning.json); may be a JSON file, a stem "
       "(without .json), or a directory name")  //
      ("input-directory,D",
       po::value<std::string>(),
       "root directory for external Parquet/CSV input files (default: input)")
      //
      ("input-format,F",
       po::value<std::string>(),
       "preferred input file format: parquet (default) or csv")  //
      ("output-directory,d",
       po::value<std::string>(),
       "root directory for solution output files (default: output)")  //
      ("output-format,f",
       po::value<std::string>(),
       "output file format: parquet (default) or csv")  //
      ("output-compression,C",
       po::value<std::string>(),
       "Parquet compression codec: gzip (default), zstd, lzo, uncompressed")
      //
      ("use-single-bus,b",
       po::value<bool>().implicit_value(/*v=*/true),
       "copper-plate (single-bus) mode: ignore all transmission constraints")
      //
      ("use-kirchhoff,k",
       po::value<bool>().implicit_value(/*v=*/true),
       "enforce DC Kirchhoff voltage-law constraints (requires reactance data)")
      //
      // ---- LP options ----
      ("lp-file,l",
       po::value<std::string>(),
       "write the assembled LP model to this file (stem; .lp extension added)")
      //
      ("lp-names-level,n",
       po::value<std::string>().implicit_value("only_cols"),
       "LP naming level: 0/minimal, 1/only_cols, 2/cols_and_rows")  //
      ("matrix-eps,e",
       po::value<double>(),
       "epsilon threshold for treating LP matrix coefficients as zero")  //
      ("lp-build,c",
       po::value<bool>().implicit_value(/*v=*/true),
       "build all LP matrices then exit without solving (combine with -l to "
       "save them)")  //
      ("lp-debug",
       po::value<bool>().implicit_value(/*v=*/true),
       "save debug LP files to the log directory (one per scene/phase for "
       "monolithic; one per iteration/scene/phase for SDDP)")  //
      ("lp-compression",
       po::value<std::string>(),
       "compression codec for debug LP files: empty=auto, none=uncompressed, "
       "or gzip/zstd/lz4/bzip2/xz")  //
      ("lp-coeff-ratio",
       po::value<double>(),
       "LP coefficient ratio threshold for conditioning diagnostics: when the "
       "global max/min |coeff| ratio exceeds this value a per-scene/phase "
       "table is printed (default: 1e7)")  //
      // ---- debug / output helpers ----
      ("json-file,j",
       po::value<std::string>(),
       "write the merged planning JSON to this file")  //
      ("fast-parsing,p",
       po::value<bool>().implicit_value(/*v=*/true),
       "use lenient (non-strict) JSON parsing")  //
      ("check-json,J",
       po::value<bool>().implicit_value(/*v=*/true),
       "warn about JSON fields not recognised by the schema")  //
      ("stats,S",
       po::value<bool>().implicit_value(/*v=*/true),
       "print LP coefficient statistics and system stats before/after solving")
      //
      ("algorithm,a",
       po::value<std::string>(),
       "LP solver algorithm: 0/default, 1/primal, 2/dual, 3/barrier "
       "(shorthand for solver_options.algorithm in JSON; default: barrier)")
      //
      ("threads,t",
       po::value<int>(),
       "number of LP solver threads, 0=auto (shorthand for "
       "solver_options.threads in JSON)")  //
      ("trace-log,T",
       po::value<std::string>(),
       "write SPDLOG_TRACE messages to this file (enables trace-level logging)")
      //
      ("cut-directory",
       po::value<std::string>(),
       "directory for SDDP Benders cut files (default: cuts)")  //
      ("log-directory",
       po::value<std::string>(),
       "directory for log and error LP files (default: logs)")  //
      ("sddp-max-iterations",
       po::value<int>(),
       "maximum SDDP forward/backward iterations (default: 100)")  //
      ("sddp-min-iterations",
       po::value<int>(),
       "minimum SDDP iterations before convergence (default: 2)")  //
      ("sddp-convergence-tol",
       po::value<double>(),
       "SDDP relative convergence tolerance (default: 1e-4)")  //
      ("sddp-elastic-penalty",
       po::value<double>(),
       "SDDP elastic slack penalty coefficient (default: 1e6)")  //
      ("sddp-elastic-mode",
       po::value<std::string>(),
       "SDDP elastic filter mode: cut (default) or backpropagate")  //
      ("sddp-num-apertures",
       po::value<int>(),
       "SDDP backward-pass aperture count: 0=disabled (default), -1=all, "
       "N=first N scenarios")  //
      ("recover",
       po::value<bool>().implicit_value(/*v=*/true),
       "enable recovery from a previous SDDP run (loads cuts and state "
       "variables according to JSON recovery_mode; default: off)");
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
 * @param lp_names_level Optional LP naming level
 * (minimal/only_cols/cols_and_rows)
 * @param input_directory Optional input directory path
 * @param input_format Optional input format string
 * @param output_directory Optional output directory path
 * @param output_format Optional output format string
 * @param output_compression Optional compression codec string
 * @param sddp_max_iterations Optional SDDP max iterations
 * @param sddp_convergence_tol Optional SDDP convergence tolerance
 * @param sddp_elastic_penalty Optional elastic penalty coefficient
 * @param sddp_elastic_mode Optional elastic filter mode string
 */
inline void apply_cli_options(
    Planning& planning,  // NOLINT(misc-const-correctness)
    const std::optional<bool>& use_single_bus,
    const std::optional<bool>& use_kirchhoff,
    const std::optional<LpNamesLevel>& lp_names_level,
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

  if (lp_names_level) {
    planning.options.lp_build_options.names_level = lp_names_level;
  }

  if (output_directory) {
    planning.options.output_directory = output_directory.value();
  }

  if (input_directory) {
    planning.options.input_directory = input_directory.value();
  }

  if (output_format) {
    planning.options.output_format =
        data_format_from_name(output_format.value());
  }

  if (output_compression) {
    planning.options.output_compression =
        compression_codec_from_name(output_compression.value());
  }

  if (input_format) {
    planning.options.input_format = data_format_from_name(input_format.value());
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
        elastic_filter_mode_from_name(sddp_elastic_mode.value());
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
    planning.options.lp_compression =
        compression_codec_from_name(lp_compression.value());
  }

  if (lp_coeff_ratio_threshold) {
    planning.options.lp_build_options.lp_coeff_ratio_threshold =
        lp_coeff_ratio_threshold;
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
  apply_cli_options(planning,
                    opts.use_single_bus,
                    opts.use_kirchhoff,
                    opts.lp_names_level,
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

  // CLI solver shortcuts → solver_options
  if (opts.algorithm) {
    planning.options.solver_options.algorithm =
        static_cast<LPAlgo>(*opts.algorithm);
  }
  if (opts.threads) {
    planning.options.solver_options.threads = *opts.threads;
  }
}

/**
 * @brief Parse an LP names level from a string (name or integer).
 *
 * Accepts "0"–"2" or "minimal"/"only_cols"/"cols_and_rows".
 *
 * @param s The string to parse.
 * @return The corresponding LpNamesLevel value.
 * @throws cli::parse_error on unrecognised input.
 */
[[nodiscard]] inline LpNamesLevel parse_lp_names_level(const std::string& s)
{
  if (const auto lvl = lp_names_level_from_name(s)) {
    return *lvl;
  }
  if (s.size() == 1 && std::isdigit(static_cast<unsigned char>(s.front())) != 0)
  {
    const int v = s.front() - '0';
    if (v >= 0 && v <= static_cast<int>(LpNamesLevel::cols_and_rows)) {
      return static_cast<LpNamesLevel>(v);
    }
  }
  throw cli::parse_error(
      std::format("invalid lp-names-level value: '{}' "
                  "(expected 0-2 or minimal/only_cols/cols_and_rows)",
                  s));
}

/**
 * @brief Build LpBuildOptions from command-line parameters
 *
 * @param lp_names_level Optional LP naming level
 * @param matrix_eps Optional epsilon tolerance for matrix coefficients
 * @return LpBuildOptions configured according to the parameters
 */
[[nodiscard]] inline LpBuildOptions make_lp_build_options(
    const std::optional<LpNamesLevel>& lp_names_level,
    const std::optional<double>& matrix_eps,
    bool compute_stats = false,
    const std::optional<std::string>& lp_solver = {})
{
  const auto eps = matrix_eps.value_or(0);
  const auto lvl = lp_names_level.value_or(LpNamesLevel::minimal);

  LpBuildOptions lp_build_opts;
  lp_build_opts.eps = eps;
  lp_build_opts.col_with_names = lvl >= LpNamesLevel::minimal;
  lp_build_opts.row_with_names = lvl >= LpNamesLevel::only_cols;
  lp_build_opts.col_with_name_map = lvl >= LpNamesLevel::minimal;
  lp_build_opts.row_with_name_map = lvl >= LpNamesLevel::only_cols;
  lp_build_opts.compute_stats = compute_stats;
  lp_build_opts.lp_names_level = lvl;
  lp_build_opts.solver_name = lp_solver.value_or("");

  return lp_build_opts;
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
      .lp_names_level = [&]() -> std::optional<LpNamesLevel>
      {
        if (const auto raw = get_opt<std::string>(vm, "lp-names-level")) {
          return parse_lp_names_level(*raw);
        }
        return std::nullopt;
      }(),
      .matrix_eps = get_opt<double>(vm, "matrix-eps"),
      .lp_build = get_opt<bool>(vm, "lp-build"),
      .lp_debug = get_opt<bool>(vm, "lp-debug"),
      .lp_compression = get_opt<std::string>(vm, "lp-compression"),
      .lp_coeff_ratio_threshold = get_opt<double>(vm, "lp-coeff-ratio"),
      .json_file = get_opt<std::string>(vm, "json-file"),
      .fast_parsing = get_opt<bool>(vm, "fast-parsing"),
      .check_json = get_opt<bool>(vm, "check-json"),
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
      .solver = get_opt<std::string>(vm, "solver"),
      .algorithm = [&]() -> std::optional<int>
      {
        if (const auto raw = get_opt<std::string>(vm, "algorithm")) {
          return parse_lp_algorithm(*raw);
        }
        return std::nullopt;
      }(),
      .threads = get_opt<int>(vm, "threads"),
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
      } catch (...) {
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
      } catch (...) {
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
  if (const auto raw = get_str("lp-names-level")) {
    try {
      opts.lp_names_level = parse_lp_names_level(*raw);
    } catch (...) {
    }
  }
  opts.matrix_eps = get_dbl("matrix-eps");
  opts.lp_build = get_bool("lp-build");
  opts.lp_debug = get_bool("lp-debug");
  opts.lp_compression = get_str("lp-compression");
  opts.lp_coeff_ratio_threshold = get_dbl("lp-coeff-ratio");

  // Debug / output
  opts.json_file = get_str("json-file");
  opts.fast_parsing = get_bool("fast-parsing");
  opts.check_json = get_bool("check-json");
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

  // Solver
  opts.solver = get_str("solver");
  if (const auto raw = get_str("algorithm")) {
    try {
      opts.algorithm = parse_lp_algorithm(*raw);
    } catch (...) {
    }
  }
  opts.threads = get_int("threads");

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
  merge(opts.lp_names_level, defaults.lp_names_level);
  merge(opts.matrix_eps, defaults.matrix_eps);
  merge(opts.lp_build, defaults.lp_build);
  merge(opts.lp_debug, defaults.lp_debug);
  merge(opts.lp_compression, defaults.lp_compression);
  merge(opts.lp_coeff_ratio_threshold, defaults.lp_coeff_ratio_threshold);
  merge(opts.json_file, defaults.json_file);
  merge(opts.fast_parsing, defaults.fast_parsing);
  merge(opts.check_json, defaults.check_json);
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
  merge(opts.solver, defaults.solver);
  merge(opts.algorithm, defaults.algorithm);
  merge(opts.threads, defaults.threads);
}

}  // namespace gtopt
