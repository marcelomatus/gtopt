/**
 * @file      gtopt_main.cpp
 * @brief     Core application entry point for the gtopt optimizer
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implementation of the gtopt_main() function, which ties together JSON
 * parsing, option application, LP construction, solving, and output writing.
 *
 * Key options handled here:
 *  - `planning_files`: list of JSON case file stems to load and merge.
 *  - `fast_parsing`: use lenient (non-strict) JSON parsing.
 *  - `just_build_lp`: build all scene/phase LP matrices but skip solving;
 *    validating input without running the solver.
 *  - `json_file`: write the merged Planning to a JSON file before solving.
 *  - `lp_file`: write the flat LP model to a `.lp` file before solving.
 *  - `print_stats`: log pre- and post-solve system statistics.
 *  - `lp_presolve`: enable/disable LP presolve in the solver.
 *  - `lp_debug`: when true, save LP debug files (one per scene/phase for
 *    the monolithic solver; one per iteration/scene/phase for SDDP) to the
 *    `log_directory`.  If `output_compression` is set (e.g. `"gzip"`), the
 *    files are compressed automatically and the originals removed.
 */

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/app_options.hpp>
#include <gtopt/check_lp.hpp>
#include <gtopt/error.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/lp_stats.hpp>
#include <gtopt/pampl_parser.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

namespace
{

constexpr auto FastParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::no,
    daw::json::options::ExcludeSpecialEscapes::no,
    daw::json::options::UseExactMappingsByDefault::no>;

constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::no>;

/// Same as StrictParsePolicy but rejects any JSON key not listed in the schema.
/// Used by the --check-json pass to surface typos / unknown fields.
constexpr auto ExactParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

/**
 * @brief Parse planning JSON files into a Planning object.
 *
 * Iterates over each path in @p planning_files, reads the JSON, and merges
 * it into a single Planning.  Returns an error string on the first failure.
 *
 * @param planning_files Paths to input JSON files (without extension).
 * @param strict_parsing Use strict parse policy when true.
 * @param check_json     Warn (via spdlog) about unrecognised JSON fields.
 * @return The merged Planning or an error message.
 */
[[nodiscard]] std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    bool strict_parsing,
    bool check_json)
{
  const spdlog::stopwatch sw;
  Planning my_planning;

  for (const auto& planning_file : planning_files) {
    try {
      std::filesystem::path fpath(planning_file);
      fpath.replace_extension(".json");

      // Check existence before calling daw::read_file, which is noexcept
      // and would call std::terminate via std::filesystem::file_size if
      // the file is missing.
      if (!std::filesystem::exists(fpath)) {
        return std::unexpected(
            std::format("Input file '{}' does not exist", fpath.string()));
      }

      // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - stream leak false
      // positive in daw::read_file
      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        return std::unexpected(
            std::format("Failed to read input file '{}'", planning_file));
      }

      spdlog::info(std::format("  Parsing input file {}", fpath.string()));

      // Optional pre-pass: parse with exact-mapping policy to surface any
      // JSON key not listed in the schema.  This parses the file a second
      // time (performance trade-off), but only when --check-json is
      // explicitly requested.  Warnings only — parsing always proceeds with
      // the normal policy below.
      if (check_json) {
        try {
          (void)daw::json::from_json<Planning>(json_result.value(),
                                               ExactParsePolicy);
        } catch (const daw::json::json_exception& jex) {
          spdlog::warn("Unknown JSON field in '{}': {}",
                       fpath.string(),
                       to_formatted_string(jex, json_result.value().c_str()));
        }
      }

      try {
        if (strict_parsing) {
          auto plan = daw::json::from_json<Planning>(json_result.value(),
                                                     StrictParsePolicy);
          my_planning.merge(std::move(plan));
        } else {
          auto plan = daw::json::from_json<Planning>(json_result.value(),
                                                     FastParsePolicy);
          my_planning.merge(std::move(plan));
        }
      } catch (const daw::json::json_exception& jex) {
        return std::unexpected(
            std::format("JSON parsing error in file '{}': {}",
                        fpath.string(),
                        to_formatted_string(jex, json_result.value().c_str())));
      }

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Unexpected error processing file '{}': {}",
                      planning_file,
                      ex.what()));
    }
  }

  spdlog::info(std::format("  Parse all input files time {:.3f}s",
                           sw.elapsed().count()));
  return my_planning;
}

/**
 * @brief Write the merged Planning to a JSON file.
 *
 * @param planning  The planning object to serialise.
 * @param json_file Output path stem (extension is replaced with ".json").
 * @return void on success or an error message.
 */
[[nodiscard]] std::expected<void, std::string> write_json_output(
    const Planning& planning, const std::string& json_file)
{
  const spdlog::stopwatch sw;
  std::filesystem::path jpath(json_file);

  try {
    jpath.replace_extension(".json");
  } catch (const std::filesystem::filesystem_error& ex) {
    return std::unexpected(
        std::format("Filesystem error processing JSON output path '{}': {}",
                    json_file,
                    ex.what()));
  }

  std::ofstream jfile(jpath);
  if (!jfile) {
    return std::unexpected(
        std::format("Failed to create JSON output file '{}'", jpath.string()));
  }

  try {
    jfile << daw::json::to_json(planning) << '\n';
  } catch (const daw::json::json_exception& ex) {
    return std::unexpected(
        std::format("JSON serialization error for file '{}': {}",
                    jpath.string(),
                    ex.what()));
  }

  spdlog::info(std::format("  Write system json file time {:.3f}s",
                           sw.elapsed().count()));
  return {};
}

/**
 * @brief Log pre-solve system statistics.
 *
 * Delegates to `gtopt_check_json --info` via posix_spawn (no shell, timeout-
 * protected, output captured).  Each output line is forwarded through the
 * spdlog INFO stream so that stats always appear in the same log channel as
 * the rest of the solver output.  Falls back to the built-in C++
 * implementation when the script is not installed or exits non-zero.
 *
 * @param planning_files The list of planning JSON file stems/paths.
 * @param planning        The parsed planning (used for the built-in fallback).
 */
void log_pre_solve_stats(const std::vector<std::string>& planning_files,
                         const Planning& planning)
{
  const auto stats = run_check_json_info(planning_files);
  if (!stats.empty()) {
    // Forward each line through the spdlog INFO stream.
    for (const auto line : std::views::split(std::string_view {stats}, '\n')) {
      const std::string_view sv {line.begin(), line.end()};
      if (!sv.empty()) {
        spdlog::info("{}", sv);
      }
    }
    return;
  }

  // Built-in fallback: print via spdlog when the external tool is absent.
  SPDLOG_DEBUG("gtopt_check_json not found on PATH; using built-in stats");

  const auto& sys = planning.system;
  const auto& sim = planning.simulation;
  const auto& plan_opts = planning.options;

  spdlog::info("=== System statistics ===");
  spdlog::info(std::format("  System name     : {}", sys.name));
  spdlog::info(std::format("  System version  : {}", sys.version));
  spdlog::info("=== System elements  ===");
  spdlog::info(std::format("  Buses           : {}", sys.bus_array.size()));
  spdlog::info(
      std::format("  Generators      : {}", sys.generator_array.size()));
  spdlog::info(std::format("  Generator profs : {}",
                           sys.generator_profile_array.size()));
  spdlog::info(std::format("  Demands         : {}", sys.demand_array.size()));
  spdlog::info(
      std::format("  Demand profs    : {}", sys.demand_profile_array.size()));
  spdlog::info(std::format("  Lines           : {}", sys.line_array.size()));
  spdlog::info(std::format("  Batteries       : {}", sys.battery_array.size()));
  spdlog::info(
      std::format("  Converters      : {}", sys.converter_array.size()));
  spdlog::info(
      std::format("  Reserve zones   : {}", sys.reserve_zone_array.size()));
  spdlog::info(std::format("  Reserve provisions   : {}",
                           sys.reserve_provision_array.size()));
  spdlog::info(
      std::format("  Junctions       : {}", sys.junction_array.size()));
  spdlog::info(
      std::format("  Waterways       : {}", sys.waterway_array.size()));
  spdlog::info(std::format("  Flows           : {}", sys.flow_array.size()));
  spdlog::info(
      std::format("  Reservoirs      : {}", sys.reservoir_array.size()));
  spdlog::info(
      std::format("  Filtrations     : {}", sys.filtration_array.size()));
  spdlog::info(std::format("  Turbines        : {}", sys.turbine_array.size()));
  spdlog::info("=== Simulation statistics ===");
  spdlog::info(std::format("  Blocks          : {}", sim.block_array.size()));
  spdlog::info(std::format("  Stages          : {}", sim.stage_array.size()));
  spdlog::info(
      std::format("  Scenarios       : {}", sim.scenario_array.size()));
  spdlog::info("=== Key options ===");
  spdlog::info(
      std::format("  use_kirchhoff   : {}",
                  plan_opts.use_kirchhoff.value_or(false) ? "true" : "false"));
  spdlog::info(
      std::format("  use_single_bus  : {}",
                  plan_opts.use_single_bus.value_or(false) ? "true" : "false"));
  spdlog::info(std::format("  scale_objective : {}",
                           plan_opts.scale_objective.value_or(1'000.0)));
  spdlog::info(std::format("  demand_fail_cost: {}",
                           plan_opts.demand_fail_cost.value_or(0.0)));
  spdlog::info(std::format("  input_directory : {}",
                           plan_opts.input_directory.value_or("(default)")));
  spdlog::info(std::format("  output_directory: {}",
                           plan_opts.output_directory.value_or("(default)")));
  spdlog::info(std::format("  output_format   : {}",
                           plan_opts.output_format.value_or("csv")));
}

/**
 * @brief Log post-solve solution statistics.
 *
 * Solve time is already printed unconditionally as "planning {:.3f}s" and
 * scale_objective is already printed in log_pre_solve_stats, so neither is
 * repeated here.
 *
 * @param planning_lp  The solved PlanningLP model.
 * @param optimal      True when the solver returned an optimal solution.
 */
void log_post_solve_stats(const PlanningLP& planning_lp, bool optimal)
{
  spdlog::info("=== Solution statistics ===");
  spdlog::info(std::format("  Status          : {}",
                           optimal ? "optimal" : "non-optimal"));

  if (optimal && !planning_lp.systems().empty()
      && !planning_lp.systems().front().empty())
  {
    const auto& lp_if =
        planning_lp.systems().front().front().linear_interface();
    const auto& plp_opts = planning_lp.options();
    const double scale = plp_opts.scale_objective();
    const double obj_scaled = lp_if.get_obj_value();
    const double obj_unscaled = obj_scaled * scale;
    spdlog::info(std::format("  LP variables    : {}", lp_if.get_numcols()));
    spdlog::info(std::format("  LP constraints  : {}", lp_if.get_numrows()));
    spdlog::info(std::format("  Obj (scaled)    : {:.6g}", obj_scaled));
    spdlog::info(std::format("  Obj (unscaled)  : {:.6g}", obj_unscaled));
    spdlog::info(std::format("  Solver kappa    : {:.6g}", lp_if.get_kappa()));
  }
}

}  // namespace

[[nodiscard]] std::expected<int, std::string> gtopt_main(
    const MainOptions& opts)
{
  // ── Set up trace log file if requested ──
  // When --trace-log is specified, a file sink is added so that all
  // SPDLOG_TRACE messages are captured for later review.
  if (opts.trace_log.has_value()) {
    try {
      auto trace_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
          opts.trace_log.value(), /*truncate=*/true);
      trace_sink->set_level(spdlog::level::trace);
      spdlog::default_logger()->sinks().push_back(std::move(trace_sink));
      spdlog::set_level(spdlog::level::trace);
      spdlog::info(std::format("trace log file: {}", opts.trace_log.value()));
    } catch (const spdlog::spdlog_ex& ex) {
      spdlog::warn(std::format("could not open trace log file: {}", ex.what()));
    }
  }

  const auto strict_parsing = !opts.fast_parsing.value_or(false);
  if (!strict_parsing) {  // NOLINT
    spdlog::info("using fast json parsing");
  }

  try {
    //
    // Parse planning JSON files
    //
    auto parse_result = parse_planning_files(
        opts.planning_files, strict_parsing, opts.check_json.value_or(false));
    if (!parse_result) {
      return std::unexpected(std::move(parse_result.error()));
    }
    Planning my_planning = std::move(*parse_result);

    //
    // Update the planning options
    //
    apply_cli_options(my_planning, opts);

    // Propagate just_build_lp into planning options so the SDDP solver
    // also sees it when called via planning_lp.resolve().
    if (opts.just_build_lp) {
      my_planning.options.just_build_lp = opts.just_build_lp;
    }

    //
    // Load user constraints from external file (if specified)
    //
    if (const auto& uc_file = my_planning.system.user_constraint_file) {
      const auto filepath = std::filesystem::path {*uc_file};
      const auto ext = filepath.extension().string();

      try {
        // Determine next UID to avoid collisions with inline constraints
        const auto& existing = my_planning.system.user_constraint_array;
        Uid next_uid = Uid {1};
        for (const auto& uc : existing) {
          if (uc.uid >= next_uid) {
            next_uid = Uid {static_cast<int>(uc.uid) + 1};
          }
        }

        std::vector<UserConstraint> loaded;
        if (ext == ".pampl") {
          loaded = PamplParser::parse_file(filepath.string(), next_uid);
          spdlog::info(
              std::format("Loaded {} user constraint(s) from PAMPL file '{}'",
                          loaded.size(),
                          filepath.string()));
        } else {
          // Default: treat as JSON array of UserConstraint
          auto file_content = daw::read_file(filepath.string());
          if (!file_content) {
            return std::unexpected(std::format(
                "Cannot read user_constraint_file '{}'", filepath.string()));
          }
          loaded =
              daw::json::from_json<std::vector<UserConstraint>>(*file_content);
          spdlog::info(
              std::format("Loaded {} user constraint(s) from JSON file '{}'",
                          loaded.size(),
                          filepath.string()));
        }

        // Append loaded constraints to the planning system
        auto& arr = my_planning.system.user_constraint_array;
        arr.insert(arr.end(),
                   std::make_move_iterator(loaded.begin()),
                   std::make_move_iterator(loaded.end()));

      } catch (const std::exception& ex) {
        return std::unexpected(
            std::format("Error loading user_constraint_file '{}': {}",
                        filepath.string(),
                        ex.what()));
      }
    }

    //
    // Write JSON output if requested
    //
    if (opts.json_file) {
      auto write_result =
          write_json_output(my_planning, opts.json_file.value());
      if (!write_result) {
        return std::unexpected(std::move(write_result.error()));
      }
    }

    //
    // Create and load the LP
    //
    try {
      const bool do_stats = opts.print_stats.value_or(true);
      const auto flat_opts =
          make_flat_options(opts.use_lp_names, opts.matrix_eps, do_stats);

      if (do_stats) {
        log_pre_solve_stats(opts.planning_files, my_planning);
      }

      spdlog::info("=== Building LP model ===");
      const spdlog::stopwatch sw;
      PlanningLP planning_lp {std::move(my_planning),  // NOLINT
                              flat_opts};
      spdlog::info(
          std::format("  Build lp time {:.3f}s", sw.elapsed().count()));

      if (opts.lp_file) {
        planning_lp.write_lp(opts.lp_file.value());
      }

      // LP coefficient static analysis: the stats were computed during
      // to_flat() and stored in the LinearInterface of each scene×phase LP.
      // When the global coefficient ratio is below 1e5 only a one-line
      // summary is emitted; otherwise per-scene/phase details are shown.
      if (do_stats) {
        std::vector<ScenePhaseLPStats> lp_entries;
        for (auto&& [si, phase_systems] :
             std::views::enumerate(planning_lp.systems()))
        {
          for (auto&& [pi, system_lp] : std::views::enumerate(phase_systems)) {
            const auto& li = system_lp.linear_interface();
            lp_entries.push_back({
                .scene_uid = static_cast<int>(system_lp.scene().uid()),
                .phase_uid = static_cast<int>(system_lp.phase().uid()),
                .num_vars = static_cast<int>(li.get_numcols()),
                .num_constraints = static_cast<int>(li.get_numrows()),
                .stats_nnz = li.lp_stats_nnz(),
                .stats_zeroed = li.lp_stats_zeroed(),
                .stats_max_abs = li.lp_stats_max_abs(),
                .stats_min_abs = li.lp_stats_min_abs(),
                .stats_max_col = static_cast<int>(li.lp_stats_max_col()),
                .stats_min_col = static_cast<int>(li.lp_stats_min_col()),
                .stats_max_col_name = std::string(li.lp_stats_max_col_name()),
                .stats_min_col_name = std::string(li.lp_stats_min_col_name()),
            });
          }
        }
        log_lp_stats_summary(lp_entries);
      }

      // just_build_lp: LP matrix assembly is done (all scene×phase LPs exist
      // in memory and, if lp_file was set, are saved to disk).  Exit before
      // ANY solving — this applies to both the monolithic and SDDP solvers.
      if (opts.just_build_lp.value_or(false)) {
        spdlog::info("just_build_lp: all LP matrices built, skipping solve");
        return 0;
      }

      spdlog::info("=== System optimization ===");
      bool optimal = false;
      {
        const spdlog::stopwatch solve_sw;

        // Build SolverOptions from Planning options (JSON) merged with any
        // CLI overrides already applied via apply_cli_options.
        const auto& plp_opts_ref = planning_lp.options();
        // Start from the solver_options sub-object (which carries any
        // tolerance values set via JSON "solver_options": {...}).
        SolverOptions solver_opts = plp_opts_ref.solver_options();
        // Individual top-level fields override the sub-object values.
        if (const auto algo = plp_opts_ref.lp_algorithm()) {
          solver_opts.algorithm = static_cast<LPAlgo>(*algo);
        }
        if (const auto thr = plp_opts_ref.lp_threads()) {
          solver_opts.threads = *thr;
        }
        if (const auto pre = plp_opts_ref.lp_presolve()) {
          solver_opts.presolve = *pre;
        }
        const auto result = planning_lp.resolve(solver_opts);
        const auto solve_elapsed =
            std::chrono::duration<double>(solve_sw.elapsed()).count();
        spdlog::info(std::format("  Optimization time {:.3f}s", solve_elapsed));

        optimal = result.has_value();

        if (!optimal) {
          const auto& err = result.error();
          // Use warn for non-optimal (e.g. time-limit reached with a feasible
          // incumbent), error only for hard failures such as infeasibility.
          const auto msg = std::format(
              "Solver did not find an optimal solution: "
              "{} (code={})",
              err.message,
              static_cast<int>(err.code));
          if (err.code == ErrorCode::SolverError) {
            spdlog::warn(msg);
          } else {
            spdlog::error(msg);
          }
          // Format optional tolerances: show "(default)" when not set.
          const auto eps_str = [](std::optional<double> v) -> std::string
          { return v ? std::format("{}", *v) : "(default)"; };
          spdlog::error(
              std::format("  Solver options used:"
                          " algorithm={}, threads={}, presolve={},"
                          " optimal_eps={}, feasible_eps={}, barrier_eps={},"
                          " log_level={}",
                          solver_opts.algorithm,
                          solver_opts.threads,
                          solver_opts.presolve,
                          eps_str(solver_opts.optimal_eps),
                          eps_str(solver_opts.feasible_eps),
                          eps_str(solver_opts.barrier_eps),
                          solver_opts.log_level));
        }
      }

      if (do_stats) {
        log_post_solve_stats(planning_lp, optimal);
      }

      if (!optimal) {
        return 1;  // non optimal solution found, but no critical error occurred
      }

      spdlog::info("=== Output writing ===");
      const spdlog::stopwatch out_sw;
      try {
        planning_lp.write_out();
      } catch (const std::exception& ex) {
        return std::unexpected(
            std::format("Error writing output: {}", ex.what()));
      }

      spdlog::info(
          std::format("  Write output time {:.3f}s", out_sw.elapsed().count()));

      // Return main conventional success
      return 0;

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error during LP creation or solving: {}", ex.what()));
    }
  } catch (const std::exception& ex) {
    return std::unexpected(
        std::format("Unexpected critical error: {}", ex.what()));
  } catch (...) {
    return std::unexpected("Unknown critical error occurred");
  }
}

}  // namespace gtopt
