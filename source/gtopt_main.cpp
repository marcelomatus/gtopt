/**
 * @file      gtopt_main.cpp
 * @brief     Core application entry point for the gtopt optimizer
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implementation of the gtopt_main() function, which ties together JSON
 * parsing, option application, LP construction, solving, and output writing.
 */

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <daw/daw_read_file.h>
#include <gtopt/app_options.hpp>
#include <gtopt/error.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_options.hpp>
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

/**
 * @brief Parse planning JSON files into a Planning object.
 *
 * Iterates over each path in @p planning_files, reads the JSON, and merges
 * it into a single Planning.  Returns an error string on the first failure.
 *
 * @param planning_files Paths to input JSON files (without extension).
 * @param strict_parsing Use strict parse policy when true.
 * @return The merged Planning or an error message.
 */
[[nodiscard]] std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files, bool strict_parsing)
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
 * @brief Log pre-solve system, simulation, and option statistics.
 *
 * @param planning The planning object whose statistics are logged.
 */
void log_pre_solve_stats(const Planning& planning)
{
  const auto& sys = planning.system;
  const auto& sim = planning.simulation;
  const auto& plan_opts = planning.options;

  spdlog::info("=== System statistics ===");
  spdlog::info(  //
      std::format("  System name     : {}", sys.name));
  spdlog::info(  //
      std::format("  System version  : {}", sys.version));
  spdlog::info("=== System elements  ===");
  spdlog::info(  //
      std::format("  Buses           : {}", sys.bus_array.size()));
  spdlog::info(  //
      std::format("  Generators      : {}", sys.generator_array.size()));
  spdlog::info(  //
      std::format("  Generator profs : {}",
                  sys.generator_profile_array.size()));
  spdlog::info(  //
      std::format("  Demands         : {}", sys.demand_array.size()));
  spdlog::info(  //
      std::format("  Demand profs    : {}", sys.demand_profile_array.size()));
  spdlog::info(  //
      std::format("  Lines           : {}", sys.line_array.size()));
  spdlog::info(  //
      std::format("  Batteries       : {}", sys.battery_array.size()));
  spdlog::info(  //
      std::format("  Converters      : {}", sys.converter_array.size()));
  spdlog::info(  //
      std::format("  Reserve zones   : {}", sys.reserve_zone_array.size()));
  spdlog::info(  //
      std::format("  Reserve provisions   : {}",
                  sys.reserve_provision_array.size()));
  spdlog::info(
      std::format("  Junctions       : {}", sys.junction_array.size()));
  spdlog::info(  //
      std::format("  Waterways       : {}", sys.waterway_array.size()));
  spdlog::info(  //
      std::format("  Flows           : {}", sys.flow_array.size()));
  spdlog::info(  //
      std::format("  Reservoirs      : {}", sys.reservoir_array.size()));
  spdlog::info(  //
      std::format("  Filtrations     : {}", sys.filtration_array.size()));
  spdlog::info(  //
      std::format("  Turbines        : {}", sys.turbine_array.size()));

  spdlog::info("=== Simulation statistics ===");
  spdlog::info(  //
      std::format("  Blocks          : {}", sim.block_array.size()));
  spdlog::info(  //
      std::format("  Stages          : {}", sim.stage_array.size()));
  spdlog::info(  //
      std::format("  Scenarios       : {}", sim.scenario_array.size()));
  spdlog::info("=== Key options ===");
  spdlog::info(  //
      std::format("  use_kirchhoff   : {}",
                  plan_opts.use_kirchhoff.value_or(false) ? "true" : "false"));
  spdlog::info(  //
      std::format("  use_single_bus  : {}",
                  plan_opts.use_single_bus.value_or(false) ? "true" : "false"));
  spdlog::info(  //
      std::format("  scale_objective : {}",
                  plan_opts.scale_objective.value_or(1'000.0)));
  spdlog::info(  //
      std::format("  demand_fail_cost: {}",
                  plan_opts.demand_fail_cost.value_or(0.0)));
  spdlog::info(  //
      std::format("  input_directory : {}",
                  plan_opts.input_directory.value_or("(default)")));
  spdlog::info(  //
      std::format("  output_directory: {}",
                  plan_opts.output_directory.value_or("(default)")));
  spdlog::info(  //
      std::format("  output_format   : {}",
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
  const auto strict_parsing = !opts.fast_parsing.value_or(false);
  if (!strict_parsing) {  // NOLINT
    spdlog::info("using fast json parsing");
  }

  try {
    //
    // Parse planning JSON files
    //
    auto parse_result =
        parse_planning_files(opts.planning_files, strict_parsing);
    if (!parse_result) {
      return std::unexpected(std::move(parse_result.error()));
    }
    Planning my_planning = std::move(*parse_result);

    //
    // Update the planning options
    //
    apply_cli_options(my_planning, opts);

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
      const auto flat_opts =
          make_flat_options(opts.use_lp_names, opts.matrix_eps);
      const bool do_stats = opts.print_stats.value_or(true);

      if (do_stats) {
        log_pre_solve_stats(
            my_planning);  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
      }

      const spdlog::stopwatch sw;
      PlanningLP planning_lp {std::move(my_planning),  // NOLINT
                              flat_opts};
      spdlog::info(std::format("  Build lp {:.3f}s", sw.elapsed().count()));

      if (opts.lp_file) {
        planning_lp.write_lp(opts.lp_file.value());
      }

      if (opts.just_create.value_or(false)) {
        return 0;
      }

      spdlog::info("=== System optimization ===");
      bool optimal = false;
      {
        const spdlog::stopwatch solve_sw;

        // SolverOptions uses library defaults (primal simplex, single thread,
        // presolve enabled).  Future work: expose these through MainOptions.
        const SolverOptions solver_opts {};
        auto result = planning_lp.resolve(solver_opts);
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
          spdlog::error(
              std::format("  Solver options used:"
                          " algorithm={}, threads={}, presolve={},"
                          " optimal_eps={}, feasible_eps={}, barrier_eps={},"
                          " log_level={}",
                          solver_opts.algorithm,
                          solver_opts.threads,
                          solver_opts.presolve,
                          solver_opts.optimal_eps,
                          solver_opts.feasible_eps,
                          solver_opts.barrier_eps,
                          solver_opts.log_level));
        }
      }

      if (do_stats) {
        log_post_solve_stats(planning_lp, optimal);
      }

      if (!optimal) {
        return 1;
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
