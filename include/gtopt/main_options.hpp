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

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/cli_options.hpp>
#include <gtopt/config_file.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/hardware_info.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/memory_monitor.hpp>
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
    if (v >= 0 && v < std::to_underlying(LPAlgo::last_algo)) {
      return static_cast<LPAlgo>(v);
    }
  }
  throw cli::parse_error(
      std::format("invalid lp-algorithm value: '{}' "
                  "(expected 0-3 or default/primal/dual/barrier)",
                  s));
}

/**
 * @brief Parse an aperture-chunk-size value from a string.
 *
 * Accepts either an integer literal (parsed via @c std::stoi) or the
 * case-insensitive string @c "auto" — mapped to the JSON sentinel 0,
 * which @c SDDPMethod::initialize_solver resolves to a concrete K via
 * @c compute_auto_aperture_chunk_size (power-of-2 rounded).
 *
 * Used by both the boost::program_options CLI parse (where the flag
 * is declared as a @c std::string) and the INI-file parser (where the
 * value is read as a string from the section map).  Single source of
 * truth for the "auto" alias keeps the two parse paths in sync.
 *
 * @param s The string to parse.
 * @return The resolved integer, or @c std::nullopt on parse failure
 *         (e.g. an empty string or non-numeric non-"auto" input).
 */
[[nodiscard]] inline std::optional<int> parse_aperture_chunk_size(
    std::string_view s) noexcept
{
  if (s.empty()) {
    return std::nullopt;
  }
  std::string lower;
  lower.reserve(s.size());
  for (const char c : s) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  if (lower == "auto") {
    return 0;
  }
  // Strict integer parse: require the WHOLE string be consumed so
  // "7auto" / "1.5" / "4 " do not silently truncate to 7 / 1 / 4.
  try {
    const std::string str(s);
    std::size_t consumed = 0;
    const int v = std::stoi(str, &consumed);
    if (consumed != str.size()) {
      return std::nullopt;
    }
    return v;
  } catch (...) {
    return std::nullopt;
  }
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
      ("list-dialects",
       po::value<std::string>().implicit_value(""),
       "list the entries in share/gtopt/naming_dialects.json (and the "
       "matching units from share/gtopt/unit_dialects.json) then exit.  "
       "Without an argument, dumps every (canonical, dialect, alias, "
       "unit) row.  With an argument, filters by that dialect name "
       "(e.g. --list-dialects plexos).")  //
      ("json-schema",
       po::value<std::string>().implicit_value(""),
       "dump the JSON Schema of the gtopt input options model to stdout "
       "and exit.  Without an argument, dumps the default "
       "(PlanningOptions) schema.  With an argument, selects a known "
       "top-level type (e.g. --json-schema options).  The full "
       "Planning/System schema is not emitted (its field-schedule "
       "variants are not renderable by the JSON-schema generator).")  //
      ("solver",
       po::value<std::string>(),
       "LP/MIP solver backend: cplex, gurobi, highs, mindopt, cbc, clp "
       "(default: auto-detect by priority "
       "cplex > highs > mindopt > cbc > clp).  "
       "Use --solvers to list backends compiled into the current build.")
      //
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
       po::value<std::string>().implicit_value(""),
       "write SPDLOG_TRACE messages to this file (enables trace-level "
       "logging).  Pass `-T PATH` for an explicit path, or just `-T` to "
       "auto-name `<log_dir>/trace_<N>.log` matching the gtopt_<N>.log "
       "from the same run.  Without `-T` no trace file is written.")
      //
      ("async-logger",
       po::value<bool>().implicit_value(/*v=*/true),
       "enable (default) or disable the spdlog async logger wrapper.  "
       "Pass `--async-logger=false` (or `--no-async-logger` shorthand) "
       "to keep the synchronous default logger.  Async mode isolates "
       "solver threads from sink I/O via a bounded queue + overrun_oldest "
       "policy + 2 worker threads; sync mode takes a sink mutex on every "
       "log call from every thread.  Disable only as a diagnostic fallback "
       "when async-related symptoms (silent log drops under -T, drain "
       "stalls during signal handling) are suspected.  Implicitly forced "
       "to false when `--trace-log`/`-T` is set, to guarantee every trace "
       "line lands on disk (the bounded async queue's overrun_oldest "
       "policy would silently drop trace bursts).")  //
      ("no-async-logger", "shorthand for `--async-logger=false`.")
      //
      ("lp-dump-backward",
       po::value<std::string>(),
       "shim onto the unified LP-debug mechanism: directory to dump "
       "backward-pass tgt LPs (one .lp file per (iter, scene, phase)) "
       "immediately before each tgt_li.resolve(opts).  Equivalent to "
       "passing --lp-debug --set sddp_options.lp_debug_passes=backward "
       "--log-directory <dir> (any pre-existing lp_debug_passes that "
       "already mentions backward or all is preserved).  Captures the "
       "LP exactly as the solver sees it (post-update_lp_for_phase, "
       "post-apply_post_load_replay under compress).  Used to localise "
       "non-replayed mutations by diffing off vs compress dumps for "
       "the same (iter, scene, phase).  Also honoured as the env var "
       "GTOPT_DUMP_BACKWARD_LP.")
      //
      ("fcut-log",
       po::value<bool>().implicit_value(/*v=*/true),
       "write a PLP-style feasibility-cut debug log (gtopt_fcut.log in "
       "log_directory — the plpfact.log analogue): one record per SDDP "
       "forward-pass infeasibility event with the emitted cut coefficients, "
       "RHS, INSTALLED/HOLGURAS/FAIL outcome, and ROLLBACK events; "
       "shorthand for --set sddp_options.fcut_log=<bool> (default when "
       "flag is given without value: true)")
      //
      ("sddp-num-apertures",
       po::value<int>(),
       "SDDP backward-pass aperture count: 0=disabled (default), -1=all, "
       "N=first N scenarios")  //
      ("aperture-chunk-size",
       po::value<std::string>(),
       "SDDP chunked aperture pass: apertures solved serially per task, "
       "sharing one LP clone with warm-start reuse.  Accepts either an "
       "integer or the literal 'auto'. "
       "auto/0/unset=auto (formula `A_max × S / (2 × cores)` rounded "
       "down to nearest power of 2), "
       "1=legacy 1-task-per-aperture, "
       ">1=exactly K apertures per task, "
       "-1=fully serial (one task per scene, all apertures inside)")  //
      ("recover",
       po::value<bool>().implicit_value(/*v=*/true),
       "enable recovery from a previous SDDP run (loads cuts and state "
       "variables according to JSON recovery_mode; default: off)")  //
      ("memory-saving",
       po::value<std::string>(),
       "coordinated memory-saving (arg = off | compress, default compress "
       "for SDDP/cascade): sets both the SDDP flat-LP release policy "
       "(sddp_options.low_memory_mode) AND the solver-native memory "
       "hint (solver_options.memory_emphasis, e.g. CPLEX "
       "CPX_PARAM_MEMORYEMPHASIS).  `compress` releases the solver and "
       "keeps a compressed flat LP — the best balance.  The legacy "
       "`rebuild` value is accepted as a back-compat alias for "
       "`compress`.  Overridden by direct JSON settings for either "
       "option.")  //
      // Deprecated alias for `--memory-saving` — hidden from help.
      ("low-memory",
       po::value<std::string>(),
       "")  //
      ("memory-limit",
       po::value<std::string>(),
       "process memory limit for work pool throttling "
       "(e.g. 4096, 300M, 5G)")  //
      ("memory-quota",
       po::value<double>(),
       "memory budget as a percentage of total host RAM "
       "(e.g. 30 → 30% of MemTotal, applied as --memory-limit). "
       "Overrides --memory-limit when both are given.")  //
      ("cpu-factor",
       po::value<double>(),
       "work pool thread over-commit factor (default: 4.0)")  //
      ("cpu-quota",
       po::value<double>(),
       "CPU budget as a percentage of physical cores (e.g. 30 → use "
       "30% of physical cores).  Shrinks physical_concurrency() once "
       "at startup so every work pool's max_threads scales down "
       "automatically.  Does not affect LP solver thread count.")  //
      ("write-out",
       po::value<std::string>(),
       "comma-separated list of output fields the solver should emit "
       "(default: all — primal solutions, row duals, and reduced costs).  "
       "Atoms: solution (alias: sol), dual, reduced_cost (aliases: cost, "
       "rcost, rc), all, none.  Example: --write-out sol,dual to skip "
       "reduced costs, or --write-out sol to skip duals as well.")  //
      ("build-mode",
       po::value<std::string>(),
       "LP build parallelism: serial, scene-parallel, full-parallel, "
       "direct-parallel (default: scene-parallel)")  //
      ("no-scale",
       po::value<bool>().implicit_value(/*v=*/true),
       "disable every automatic LP scaling / equilibration (forces "
       "model_options.scale_objective=1.0, scale_theta=1.0, "
       "auto_scale=false, and "
       "lp_matrix_options.equilibration_method=none).  Intended for "
       "debug / physical-unit validation of coefficients and RHS.  "
       "Overrides JSON values for the affected fields.")  //
      ("lp-reduction",
       po::value<bool>().implicit_value(/*v=*/true),
       "eliminate provably-zero LP columns/rows at the SOURCE element "
       "(generator pmax==pmin==0, reservoir zero extraction, "
       "reserve/inertia zero-ceiling provision, inertia zero requirement). "
       "Shorthand for `--set model_options.lp_reduction=true`.  DEFAULT "
       "OFF: the un-reduced LP is the honest model and CPLEX/Gurobi "
       "presolve reduces it equivalently.  Enable for weak-presolve "
       "backends (CLP / CBC / HiGHS) that benefit from the smaller LP up "
       "front.  Note: enabling it drops commitment u/v/w for pmax=0 units "
       "(no ancillary participation), which can shift the optimum "
       "slightly.")  //
      ("no-lp-reduction",
       po::value<bool>().implicit_value(/*v=*/true),
       "force-disable the SOURCE zero-column/row elimination (the default "
       "already-OFF behaviour) — useful to override a JSON file that sets "
       "`model_options.lp_reduction=true`.  Shorthand for "
       "`--set model_options.lp_reduction=false`.")  //
      // `--no-presolve` and `--no-crossover` removed (2026-05-21):
      // too solver-specific for the top-level CLI surface.  The
      // generic `--set solver_options.presolve=false` and
      // `--set solver_options.crossover=false` still work for the
      // narrow set of users who actually need them.
      ("no-mip",
       po::value<bool>().implicit_value(/*v=*/true),
       "LP-relax every phase (all binary / integer variables become "
       "continuous).  Shorthand for "
       "`--set model_options.continuous_phases=all`.  Applied at LP "
       "assembly time via SimulationLP, so the relaxation is uniform "
       "across the monolithic, SDDP, and cascade methods.  Useful for "
       "quick LP-only smoke tests on cases that would otherwise solve a "
       "MIP (commitment, segment-based costs, etc.); the relaxation "
       "gives a lower bound on the true MIP optimum but loses on/off "
       "semantics.")
      //
      ("naming-dialect",
       po::value<std::string>(),
       "enforce a specific naming dialect for input + output.  On "
       "input: emits a once-per-alias warning when a JSON key matches "
       "an alias whose `dialect` tag in "
       "share/gtopt/naming_dialects.json differs from <name>.  On "
       "output: rewrites canonical keys to the dialect's aliases when "
       "emitting `planning.json` (parquet column rename is a follow-up).  "
       "Recognised values: gtopt, plp, sddp, plexos, pypsa, pandapower "
       "(see naming_dialects.json for the authoritative list).  "
       "Shorthand for --set model_options.naming_dialect=<name>.")
      //
      ("mip-gap",
       po::value<double>(),
       "Target relative MIP optimality gap (e.g. 0.01 = 1 %); ignored on "
       "continuous LPs.  Shorthand for "
       "--set solver_options.mip_gap=<value>.  Backend mapping: "
       "CPLEX CPX_PARAM_EPGAP, HiGHS mip_rel_gap, Gurobi MIPGap.  Pair "
       "with --time-limit to bound MIP wall-clock when the gap target "
       "is loose.")
      //
      ("time-limit",
       po::value<double>(),
       "Per-solve time limit in seconds (0 = no limit).  Shorthand for "
       "--set solver_options.time_limit=<value>.  Applied to every LP / "
       "MIP solve the backend issues (forward + backward passes in "
       "SDDP, every aperture clone in cascade); the solver aborts the "
       "current solve when wall-clock exceeds the limit.  Backend "
       "mapping: CPLEX TILIM, HiGHS time_limit, Gurobi TimeLimit, "
       "MindOpt MAX_TIME, CLP setMaximumSeconds.  Callers should check "
       "`is_optimal()` after solve to detect timeouts.")
      //
      ("mip-start",
       po::value<bool>().implicit_value(/*v=*/true),
       "enable the initial-MIP-solution (warm-start) pipeline: relax -> round "
       "-> domain_rules -> [scip_repair] -> inject.  Solves the LP relaxation, "
       "rounds the integer columns, repairs the commitment with power-system "
       "domain rules, and injects the result as a backend MIP start so the "
       "solver bypasses its costly node-0 heuristic incumbent.  Shorthand for "
       "--set monolithic_options.mip_start.enabled=true.  Stage controls via "
       "--set monolithic_options.mip_start.<stage>.<field>=<value>: "
       "relax.solver_options.*, relax.check, "
       "relax.on_infeasible=stop|warn|feasopt, relax.report_saturated, "
       "round.threshold, domain_rules.min_up_down, "
       "domain_rules.commitment_logic, domain_rules.peak_injection.enabled, "
       "domain_rules.peak_injection.peak_window.{start,end}, "
       "domain_rules.peak_injection.solar_window.{start,end}, "
       "scip_repair.enabled, inject.effort, from_file=<path> to replay a "
       "dumped start, dump_file=<path> to persist this solve's integers for a "
       "later cross-solver replay.")
      //
      // ---- deprecated options (hidden from `--help`, still parsed) ----
      //
      // Each flag below emits a deprecation warning via `warn_deprecated_cli`
      // and points the user at the canonical `--set <path>=<value>` form.
      // Migration map (old flag → new --set path):
      //   --sddp-cpu-factor       -> --set sddp_options.pool_cpu_factor=...
      //   --input-directory  / -D -> --set input_directory=...
      //   --input-format     / -F -> --set input_format=...
      //   --output-directory / -d -> --set output_directory=...
      //   --output-format    / -f -> --set output_format=...
      //   --output-compression/-C -> --set output_compression=...
      //   --use-single-bus   / -b -> --set model_options.use_single_bus=...
      //   --use-kirchhoff    / -k -> --set model_options.use_kirchhoff=...
      //   --lp-debug              -> --set lp_debug=...
      //   --lp-compression        -> --set lp_compression=...
      //   --lp-coeff-ratio        -> --set
      //   lp_matrix_options.lp_coeff_ratio_threshold=...
      //   --algorithm        / -a -> --set solver_options.algorithm=...
      //   --threads          / -t -> --set solver_options.threads=...
      //   --cut-directory         -> --set sddp_options.cut_directory=...
      //   --log-directory         -> --set log_directory=...
      //   --sddp-max-iterations   -> --set sddp_options.max_iterations=...
      //   --sddp-min-iterations   -> --set sddp_options.min_iterations=...
      //   --sddp-convergence-tol  -> --set sddp_options.convergence_tol=...
      //   --sddp-elastic-penalty  -> --set sddp_options.elastic_penalty=...
      //   --sddp-elastic-mode     -> --set sddp_options.elastic_mode=...
      ("sddp-cpu-factor",
       po::value<double>(),
       "deprecated alias for --cpu-factor (SDDP work-pool over-commit "
       "factor); kept for backward compatibility")  //
      ("input-directory,D",
       po::value<std::string>(),
       "directory holding input time-series files (default: 'input'); "
       "shorthand for --set input_directory=<dir>")  //
      ("input-format,F",
       po::value<std::string>(),
       "preferred input file format: parquet | csv (default: parquet); "
       "shorthand for --set input_format=<fmt>")  //
      ("output-directory,d",
       po::value<std::string>(),
       "directory for solution output files (default: 'output'); "
       "shorthand for --set output_directory=<dir>")  //
      ("output-format,f",
       po::value<std::string>(),
       "output file format: parquet | csv (default: parquet); "
       "shorthand for --set output_format=<fmt>")  //
      ("output-compression,C",
       po::value<std::string>(),
       "Parquet output compression codec: lz4 | snappy | zstd | gzip | none "
       "(default: zstd); shorthand for --set output_compression=<codec>")  //
      ("use-single-bus,b",
       po::value<bool>().implicit_value(/*v=*/true),
       "copper-plate (single-bus) mode: ignore network topology and line "
       "limits; shorthand for --set model_options.use_single_bus=<bool> "
       "(default when flag is given without value: true)")  //
      ("use-kirchhoff,k",
       po::value<bool>().implicit_value(/*v=*/true),
       "apply DC-OPF Kirchhoff voltage-angle constraints (default: true); "
       "shorthand for --set model_options.use_kirchhoff=<bool>")  //
      ("lp-debug",
       po::value<bool>().implicit_value(/*v=*/true),
       "save LP debug files to log_directory: a single monolithic.lp for "
       "the monolithic solver, or one per-(scene, phase) LP file for every "
       "SDDP pass selected by sddp_options.lp_debug_passes; shorthand for "
       "--set lp_debug=<bool> (default when flag is given without "
       "value: true)")  //
      ("lp-error",
       po::value<bool>().implicit_value(/*v=*/true),
       "write an error LP file to log_directory when a cell is infeasible, and "
       "keep the col/row name metadata needed to write it (independent of "
       "lp-debug); shorthand for --set lp_error=<bool> (default when flag is "
       "given without value: true)")  //
      ("lp-compression",
       po::value<std::string>(),
       "compression codec for LP debug files: gzip | zstd | none "
       "(default: zstd); shorthand for --set lp_compression=<codec>")  //
      ("lp-coeff-ratio",
       po::value<double>(),
       "warn when the LP max/min |coefficient| ratio exceeds this threshold "
       "(default: 1e7); shorthand for --set "
       "lp_matrix_options.lp_coeff_ratio_threshold=<ratio>")  //
      ("algorithm,a",
       po::value<std::string>(),
       "LP algorithm: default | primal | dual | barrier (or 0..3); "
       "shorthand for --set solver_options.algorithm=<algo>")  //
      ("threads,t",
       po::value<int>(),
       "solver thread count (default: solver-specific); shorthand for "
       "--set solver_options.threads=<n>")  //
      ("cut-directory",
       po::value<std::string>(),
       "directory under output_directory for SDDP Benders cut files "
       "(default: 'cuts'); shorthand for --set "
       "sddp_options.cut_directory=<dir>")  //
      ("log-directory",
       po::value<std::string>(),
       "directory for solver log + LP-debug files (default: 'logs'); "
       "shorthand for --set log_directory=<dir>")  //
      ("sddp-max-iterations",
       po::value<int>(),
       "maximum SDDP forward/backward iterations (default: 100); "
       "shorthand for --set sddp_options.max_iterations=<n>")  //
      ("sddp-min-iterations",
       po::value<int>(),
       "minimum SDDP iterations before convergence is considered "
       "(default: 1); shorthand for --set "
       "sddp_options.min_iterations=<n>")  //
      ("sddp-convergence-tol",
       po::value<double>(),
       "relative gap tolerance for SDDP convergence (default: 0.01); "
       "shorthand for --set sddp_options.convergence_tol=<eps>")  //
      ("sddp-elastic-penalty",
       po::value<double>(),
       "penalty coefficient for elastic slack variables in the SDDP "
       "feasibility filter; shorthand for --set "
       "sddp_options.elastic_penalty=<val>")  //
      ("sddp-elastic-mode",
       po::value<std::string>(),
       "elastic-filter mode: chinneck | iis | single_cut | cut | multi_cut "
       "| state_repair (alias plp) | farkas_recursive (default: chinneck). "
       "chinneck/iis: IIS-filtered per-bound cuts; single_cut/cut: one "
       "aggregated Benders fcut from the elastic clone's fixing-row duals; "
       "multi_cut: one signed bound cut per activated slack; state_repair "
       "(alias plp): PLP-exact per-reservoir minimal-repair cuts "
       "(AgrElastici parity); farkas_recursive: Fullner-Rebennack "
       "SIAM-Rev-2023 recursive aggregated cut that also relaxes installed "
       "fcut rows (+z) and folds their intercepts.  state_repair/"
       "farkas_recursive tune via --set sddp_options.fact_eps=<eps> and "
       "--set sddp_options.fact_max_cycles=<n>; shorthand for --set "
       "sddp_options.elastic_mode=<mode>")  //
      ("constraint-mode",
       po::value<std::string>(),
       "UserConstraint resolver strictness: "
       "strict (default; abort on any unresolved element ref), "
       "normal (drop a constraint when ALL terms are unresolved; emit a "
       "warning), "
       "debug (drop unresolved terms silently and continue — for "
       "iterative parser work where dangling LHS refs are expected; pair "
       "with `plexos2gtopt --lax-uc-refs` for the matching converter-side "
       "leniency).  Shorthand for --set "
       "planning_options.constraint_mode=<mode>");
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
    const std::optional<bool>& lp_error = {},
    const std::optional<std::string>& lp_compression = {},
    const std::optional<double>& lp_coeff_ratio_threshold = {})
{
  // Post-§11 (2026-05-17): write into `model_options` since the
  // legacy top-level mirrors on `PlanningOptions` were removed.
  if (use_single_bus) {
    planning.options.model_options.use_single_bus = use_single_bus;
  }

  if (use_kirchhoff) {
    planning.options.model_options.use_kirchhoff = use_kirchhoff;
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
    // Direct mapping to SddpOptions::num_apertures — the C++ side
    // truncates each phase's Phase.apertures to the first N entries.
    // Combined with the wettest-first sort applied by plp2gtopt this
    // selects the N wettest apertures per phase.  Per-level overrides
    // are available via --set
    // cascade_options.level_array.N.sddp_options.num_apertures=K.
    planning.options.sddp_options.num_apertures = *sddp_num_apertures;
  }

  if (lp_debug) {
    planning.options.lp_debug = lp_debug;
    planning.options.lp_error = lp_error;
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
  // `--constraint-mode` is a permanent CLI shorthand for
  // `--set planning_options.constraint_mode=...` — no deprecation
  // warning, but the apply step below routes the override.
  if (opts.constraint_mode) {
    planning.options.constraint_mode = require_enum<ConstraintMode>(
        "constraint-mode", opts.constraint_mode.value());
  }
  if (opts.algorithm.has_value()) {
    spdlog::warn(
        "--algorithm is deprecated, use: --set solver_options"
        ".algorithm={}",
        enum_name(*opts.algorithm));
  }
  warn_deprecated_cli(opts.threads, "threads", "solver_options.threads");
  warn_deprecated_cli(opts.lp_debug, "lp-debug", "lp_debug");
  warn_deprecated_cli(opts.lp_error, "lp-error", "lp_error");
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
                    opts.lp_error,
                    opts.lp_compression,
                    opts.lp_coeff_ratio_threshold);

  // Additional SDDP options not in the positional overload
  if (opts.sddp_min_iterations) {
    planning.options.sddp_options.min_iterations = opts.sddp_min_iterations;
  }
  if (opts.sddp_aperture_chunk_size) {
    planning.options.sddp_options.aperture_chunk_size =
        opts.sddp_aperture_chunk_size;
  }
  // Translation shim: `--lp-dump-backward <dir>` (and the legacy
  // `GTOPT_DUMP_BACKWARD_LP=<dir>` env var) map onto the unified
  // `lp_debug` + `lp_debug_passes` mechanism.  Sets `lp_debug=true`,
  // routes the dump under `<dir>` (overrides `log_directory`), and
  // ensures the `backward` token is selected (or `all` is preserved
  // if the user already configured a richer pass list).
  std::optional<std::string> dump_dir = opts.lp_dump_backward;
  if (!dump_dir) {
    if (const auto* env = std::getenv("GTOPT_DUMP_BACKWARD_LP");
        env != nullptr && *env != '\0')
    {
      dump_dir = std::string(env);
    }
  }
  if (dump_dir) {
    planning.options.lp_debug = true;
    planning.options.log_directory = *dump_dir;
    auto& passes = planning.options.sddp_options.lp_debug_passes;
    auto already_includes_backward = [&](std::string_view s) noexcept
    {
      // Tokens are case-insensitive, comma-separated.  Match
      // `backward` or `all`.
      std::string_view rest = s;
      while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto raw =
            (comma == std::string_view::npos) ? rest : rest.substr(0, comma);
        // Trim surrounding whitespace.
        std::size_t lo = 0;
        std::size_t hi = raw.size();
        while (lo < hi && (raw[lo] == ' ' || raw[lo] == '\t')) {
          ++lo;
        }
        while (hi > lo && (raw[hi - 1] == ' ' || raw[hi - 1] == '\t')) {
          --hi;
        }
        const auto tok = raw.substr(lo, hi - lo);
        const auto eq_ci = [](std::string_view a, std::string_view b)
        {
          if (a.size() != b.size()) {
            return false;
          }
          for (std::size_t i = 0; i < a.size(); ++i) {
            const auto av = static_cast<unsigned char>(a[i]);
            const auto bv = static_cast<unsigned char>(b[i]);
            const auto al = (av >= 'A' && av <= 'Z')
                ? static_cast<unsigned char>(av + 32U)
                : av;
            const auto bl = (bv >= 'A' && bv <= 'Z')
                ? static_cast<unsigned char>(bv + 32U)
                : bv;
            if (al != bl) {
              return false;
            }
          }
          return true;
        };
        if (eq_ci(tok, "backward") || eq_ci(tok, "all")) {
          return true;
        }
        if (comma == std::string_view::npos) {
          break;
        }
        rest.remove_prefix(comma + 1);
      }
      return false;
    };
    if (!passes.has_value() || passes->empty()) {
      passes = std::string("backward");
    } else if (!already_includes_backward(*passes)) {
      passes = *passes + ",backward";
    }
  }
  if (opts.sddp_hot_start) {
    planning.options.sddp_options.cut_recovery_mode =
        *opts.sddp_hot_start ? HotStartMode::replace : HotStartMode::none;
  }

  // `--fcut-log` is a permanent CLI shorthand for
  // `--set sddp_options.fcut_log=...` — no deprecation warning.
  if (opts.fcut_log) {
    planning.options.sddp_options.fcut_log = opts.fcut_log;
  }

  // --recover gates whether recovery happens at all.
  // When not passed (or explicitly false), force recovery_mode to "none"
  // so JSON config alone cannot trigger recovery.
  if (!opts.recover.value_or(false)) {
    planning.options.sddp_options.recovery_mode = RecoveryMode::none;
  }

  if (opts.no_mip.value_or(false)) {
    // `--no-mip` LP-relaxes every phase by forcing
    // `model_options.continuous_phases = "all"`.  Overrides any JSON
    // value so the shortcut is uniformly authoritative — users who want
    // a partial relaxation should drop the flag and set
    // `continuous_phases` directly via `--set`.
    planning.options.model_options.continuous_phases = OptName {"all"};
  }

  if (opts.naming_dialect.has_value()) {
    planning.options.model_options.naming_dialect = opts.naming_dialect;
  }

  if (opts.mip_gap.has_value()) {
    // CLI shortcut wins over any prior JSON setting — `--set
    // solver_options.mip_gap=…` is still available for full control,
    // but the bespoke flag is uniformly authoritative when both fire.
    planning.options.solver_options.mip_gap = opts.mip_gap;
  }

  if (opts.time_limit.has_value()) {
    // CLI wins over JSON, same pattern as --mip-gap.  0 means "no
    // limit" — the SolverOptions::time_limit field is optional so we
    // pass the raw value through; backends apply only when *value > 0.
    planning.options.solver_options.time_limit = opts.time_limit;
  }

  if (opts.mip_start_enable.has_value()) {
    // CLI flag → monolithic_options.mip_start.enabled.  The staged controls
    // stay reachable via `--set
    // monolithic_options.mip_start.<stage>.<field>=…`.
    auto& mip_start = planning.options.monolithic_options.mip_start;
    if (!mip_start.has_value()) {
      mip_start.emplace();
    }
    mip_start->enabled = *opts.mip_start_enable;
  }

  if (opts.no_scale.value_or(false)) {
    // `--no-scale` disables every auto-scaling / equilibration
    // mechanism for debug / physical-unit validation, and
    // intentionally OVERRIDES JSON values to ensure full coverage.
    // This is used as a diagnostic for cut-construction unit math
    // (e.g. juan/gtopt_iplp's LB-compounding bug, where
    // scale_objective=1000 in the JSON would otherwise leak through
    // and prevent isolation of scale-related defects).
    //
    // To probe a specific scale axis in isolation, leave `--no-scale`
    // off and set just that one value via `--set <path>=<value>`.
    planning.options.model_options.scale_objective = 1.0;
    planning.options.model_options.scale_theta = 1.0;
    planning.options.model_options.scale_loss_link = 1.0;
    planning.options.lp_matrix_options.equilibration_method =
        LpEquilibrationMethod::none;
    // Kill switch for the per-element auto-scale heuristics in
    // `PlanningLP::auto_scale_theta / _reservoirs / _lng_terminals`
    // — without this the reservoir energy/flow variable_scales would
    // still be computed and applied at LP construction.
    planning.options.model_options.auto_scale = false;
  }

  if (opts.lp_reduction.value_or(false)) {
    // `--lp-reduction` turns ON the SOURCE zero-column/row elimination
    // (default is OFF).  Intended for weak-presolve backends (CLP / CBC /
    // HiGHS) that benefit from the smaller LP; CPLEX/Gurobi presolve
    // reduces the un-reduced LP equivalently so they gain nothing.
    planning.options.model_options.lp_reduction = true;
  }
  if (opts.no_lp_reduction.value_or(false)) {
    // `--no-lp-reduction` force-disables it (the default), overriding a
    // JSON `model_options.lp_reduction=true`.  Applied last so it wins if
    // both flags are passed.
    planning.options.model_options.lp_reduction = false;
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

  // CPU quota: clamp `physical_concurrency()` once for the rest of the
  // process so every work-pool factory's `cpu_factor × physical_cores`
  // calculation gets the right slice of the host.  Must happen before
  // any pool is constructed.  Values outside (0, 100) are silently
  // ignored by `set_cpu_quota_pct` (no-op clamp).
  if (opts.cpu_quota) {
    set_cpu_quota_pct(*opts.cpu_quota);
    if (get_cpu_quota_pct() > 0.0) {
      spdlog::info("cpu-quota={:.0f}% → effective physical cores: {} of {}",
                   *opts.cpu_quota,
                   physical_concurrency(),
                   detected_physical_concurrency());
    } else {
      spdlog::warn(
          "cpu-quota={} is outside (0, 100) — ignored; "
          "physical_concurrency()={}",
          *opts.cpu_quota,
          physical_concurrency());
    }
  }

  // Memory quota: translate "% of MemTotal" to an absolute MB value
  // and route through the existing `pool_memory_limit_mb` pipeline.
  // Takes precedence over `--memory-limit` when both are set.
  std::optional<double> resolved_memory_limit_mb;
  if (opts.memory_quota && *opts.memory_quota > 0.0
      && *opts.memory_quota < 100.0)
  {
    const auto snap = MemoryMonitor::get_system_memory_snapshot();
    if (snap.total_mb > 0.0) {
      resolved_memory_limit_mb = snap.total_mb * (*opts.memory_quota) / 100.0;
      spdlog::info("memory-quota={:.0f}% of {:.0f} MB → memory_limit={:.0f} MB",
                   *opts.memory_quota,
                   snap.total_mb,
                   *resolved_memory_limit_mb);
    } else {
      spdlog::warn(
          "memory-quota={}% requested but MemTotal could not be read; "
          "ignoring",
          *opts.memory_quota);
    }
  } else if (opts.memory_quota) {
    spdlog::warn("memory-quota={} is outside (0, 100) — ignored",
                 *opts.memory_quota);
  }

  if (resolved_memory_limit_mb) {
    planning.options.sddp_options.pool_memory_limit_mb =
        resolved_memory_limit_mb;
  } else if (opts.memory_limit) {
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
    planning.options.write_out = parse_output_selection(*opts.write_out);
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
      .lp_error = get_opt<bool>(vm, "lp-error"),
      .lp_compression = get_opt<std::string>(vm, "lp-compression"),
      .lp_coeff_ratio_threshold = get_opt<double>(vm, "lp-coeff-ratio"),
      .json_file = get_opt<std::string>(vm, "json-file"),
      .print_stats = get_opt<bool>(vm, "stats"),
      .trace_log = get_opt<std::string>(vm, "trace-log"),
      .async_logger = [&vm]() -> std::optional<bool>
      {
        // `--no-async-logger` (pure flag) is authoritative when set —
        // it forces sync mode regardless of `--async-logger=...`.
        if (vm.contains("no-async-logger")) {
          return false;
        }
        return get_opt<bool>(vm, "async-logger");
      }(),
      .lp_dump_backward = get_opt<std::string>(vm, "lp-dump-backward"),
      .fcut_log = get_opt<bool>(vm, "fcut-log"),
      .cut_directory = get_opt<std::string>(vm, "cut-directory"),
      .log_directory = get_opt<std::string>(vm, "log-directory"),
      .sddp_max_iterations = get_opt<int>(vm, "sddp-max-iterations"),
      .sddp_min_iterations = get_opt<int>(vm, "sddp-min-iterations"),
      .sddp_convergence_tol = get_opt<double>(vm, "sddp-convergence-tol"),
      .sddp_elastic_penalty = get_opt<double>(vm, "sddp-elastic-penalty"),
      .sddp_elastic_mode = get_opt<std::string>(vm, "sddp-elastic-mode"),
      .constraint_mode = get_opt<std::string>(vm, "constraint-mode"),
      .sddp_num_apertures = get_opt<int>(vm, "sddp-num-apertures"),
      .sddp_aperture_chunk_size =
          get_opt<std::string>(vm, "aperture-chunk-size")
              .and_then([](const std::string& s)
                        { return parse_aperture_chunk_size(s); }),
      .recover = get_opt<bool>(vm, "recover"),
      // Prefer the new `--memory-saving` name; fall back to the
      // deprecated `--low-memory` alias for backward compatibility.
      .memory_saving =
          get_opt<std::string>(vm, "memory-saving")
              .or_else([&] { return get_opt<std::string>(vm, "low-memory"); }),
      .memory_limit = get_opt<std::string>(vm, "memory-limit"),
      .memory_quota = get_opt<double>(vm, "memory-quota"),
      .sddp_cpu_factor =
          get_opt<double>(vm, "cpu-factor")
              .or_else([&] { return get_opt<double>(vm, "sddp-cpu-factor"); }),
      .cpu_quota = get_opt<double>(vm, "cpu-quota"),
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
      .lp_reduction = get_opt<bool>(vm, "lp-reduction"),
      .no_lp_reduction = get_opt<bool>(vm, "no-lp-reduction"),
      .no_mip = get_opt<bool>(vm, "no-mip"),
      .naming_dialect = get_opt<std::string>(vm, "naming-dialect"),
      .mip_gap = get_opt<double>(vm, "mip-gap"),
      .time_limit = get_opt<double>(vm, "time-limit"),
      .mip_start_enable = get_opt<bool>(vm, "mip-start"),
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
  opts.matrix_eps = get_dbl("matrix-eps");
  opts.lp_only = get_bool("lp-only");
  opts.lp_debug = get_bool("lp-debug");
  opts.lp_error = get_bool("lp-error");
  opts.lp_compression = get_str("lp-compression");
  opts.lp_coeff_ratio_threshold = get_dbl("lp-coeff-ratio");

  // Debug / output
  opts.json_file = get_str("json-file");
  opts.print_stats = get_bool("stats");
  opts.trace_log = get_str("trace-log");
  opts.async_logger = get_bool("async-logger");
  opts.lp_dump_backward = get_str("lp-dump-backward");
  opts.fcut_log = get_bool("fcut-log");

  // SDDP directories
  opts.cut_directory = get_str("cut-directory");
  opts.log_directory = get_str("log-directory");

  // SDDP tuning
  opts.sddp_max_iterations = get_int("sddp-max-iterations");
  opts.sddp_min_iterations = get_int("sddp-min-iterations");
  opts.sddp_convergence_tol = get_dbl("sddp-convergence-tol");
  opts.sddp_elastic_penalty = get_dbl("sddp-elastic-penalty");
  opts.sddp_elastic_mode = get_str("sddp-elastic-mode");
  opts.constraint_mode = get_str("constraint-mode");
  opts.sddp_num_apertures = get_int("sddp-num-apertures");
  // aperture-chunk-size accepts "auto" (→0) or an integer literal.
  if (const auto s = get_str("aperture-chunk-size")) {
    opts.sddp_aperture_chunk_size = parse_aperture_chunk_size(*s);
  }
  // Prefer the new key; fall back to the deprecated `low-memory` alias.
  opts.memory_saving = get_str("memory-saving");
  if (!opts.memory_saving) {
    opts.memory_saving = get_str("low-memory");
  }
  opts.memory_limit = get_str("memory-limit");
  opts.memory_quota = get_dbl("memory-quota");
  opts.sddp_cpu_factor = get_dbl("cpu-factor");
  if (!opts.sddp_cpu_factor) {
    opts.sddp_cpu_factor = get_dbl("sddp-cpu-factor");
  }
  opts.cpu_quota = get_dbl("cpu-quota");
  opts.build_mode = get_str("build-mode");
  opts.write_out = get_str("write-out");

  // Solver
  opts.solver = get_str("solver");
  if (const auto raw = get_str("algorithm")) {
    try {
      opts.algorithm = parse_lp_algorithm(*raw);
    } catch (...) {
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
        } catch (...) {
        }
      }
      return std::nullopt;
    };
    auto sv_dbl = [&](const std::string& key) -> std::optional<double>
    {
      if (const auto kv = sec_map.find(key); kv != sec_map.end()) {
        try {
          return std::stod(kv->second);
        } catch (...) {
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
      } catch (...) {
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
  merge(opts.lp_error, defaults.lp_error);
  merge(opts.lp_compression, defaults.lp_compression);
  merge(opts.lp_coeff_ratio_threshold, defaults.lp_coeff_ratio_threshold);
  merge(opts.json_file, defaults.json_file);
  merge(opts.print_stats, defaults.print_stats);
  merge(opts.trace_log, defaults.trace_log);
  merge(opts.async_logger, defaults.async_logger);
  merge(opts.lp_dump_backward, defaults.lp_dump_backward);
  merge(opts.fcut_log, defaults.fcut_log);
  merge(opts.cut_directory, defaults.cut_directory);
  merge(opts.log_directory, defaults.log_directory);
  merge(opts.sddp_max_iterations, defaults.sddp_max_iterations);
  merge(opts.sddp_min_iterations, defaults.sddp_min_iterations);
  merge(opts.sddp_convergence_tol, defaults.sddp_convergence_tol);
  merge(opts.sddp_elastic_penalty, defaults.sddp_elastic_penalty);
  merge(opts.sddp_elastic_mode, defaults.sddp_elastic_mode);
  merge(opts.constraint_mode, defaults.constraint_mode);
  merge(opts.sddp_num_apertures, defaults.sddp_num_apertures);
  merge(opts.sddp_aperture_chunk_size, defaults.sddp_aperture_chunk_size);
  merge(opts.memory_saving, defaults.memory_saving);
  merge(opts.no_scale, defaults.no_scale);
  merge(opts.lp_reduction, defaults.lp_reduction);
  merge(opts.no_lp_reduction, defaults.no_lp_reduction);
  merge(opts.no_mip, defaults.no_mip);
  merge(opts.naming_dialect, defaults.naming_dialect);
  merge(opts.mip_gap, defaults.mip_gap);
  merge(opts.time_limit, defaults.time_limit);
  merge(opts.memory_limit, defaults.memory_limit);
  merge(opts.memory_quota, defaults.memory_quota);
  merge(opts.sddp_cpu_factor, defaults.sddp_cpu_factor);
  merge(opts.cpu_quota, defaults.cpu_quota);
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
