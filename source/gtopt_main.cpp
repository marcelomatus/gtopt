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
#include <span>
#include <string>

#include <daw/daw_read_file.h>
#include <gtopt/app_options.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

[[nodiscard]] std::expected<int, std::string> gtopt_main(
    std::span<const std::string> planning_files,
    const std::optional<std::string>& input_directory,
    const std::optional<std::string>& input_format,
    const std::optional<std::string>& output_directory,
    const std::optional<std::string>& output_format,
    const std::optional<std::string>& compression_format,
    const std::optional<bool>& use_single_bus,
    const std::optional<bool>& use_kirchhoff,
    const std::optional<std::string>& lp_file,
    const std::optional<int>& use_lp_names,
    const std::optional<double>& matrix_eps,
    const std::optional<std::string>& json_file,
    const std::optional<bool>& just_create,
    const std::optional<bool>& fast_parsing,
    const std::optional<bool>& print_stats)
{
  try {
    //
    // parsing the system from json
    //
    static constexpr auto FastParsePolicy = daw::json::options::parse_flags<
        daw::json::options::PolicyCommentTypes::hash,
        daw::json::options::CheckedParseMode::no,
        daw::json::options::ExcludeSpecialEscapes::no,
        daw::json::options::UseExactMappingsByDefault::no>;

    static constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
        daw::json::options::PolicyCommentTypes::hash,
        daw::json::options::CheckedParseMode::yes,
        daw::json::options::ExcludeSpecialEscapes::yes,
        daw::json::options::UseExactMappingsByDefault::no>;

    const auto strict_parsing = !fast_parsing.value_or(false);
    if (!strict_parsing) {  // NOLINT
      spdlog::info("using fast json parsing");
    }

    Planning my_planning;
    {
      spdlog::stopwatch sw;

      for (auto&& planning_file : planning_files) {
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

          spdlog::info(std::format("parsing input file {}", fpath.string()));

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
            return std::unexpected(std::format(
                "JSON parsing error in file '{}': {}",
                fpath.string(),
                to_formatted_string(jex, json_result.value().c_str())));
          }

        } catch (const std::bad_alloc& ex) {
          return std::unexpected(
              std::format("Out of memory while processing file '{}': {}",
                          planning_file,
                          ex.what()));
        } catch (const std::exception& ex) {
          return std::unexpected(
              std::format("Unexpected error processing file '{}': {}",
                          planning_file,
                          ex.what()));
        }
      }

      spdlog::info(std::format("parsing all json files {}s", sw));
    }

    //
    // update the planning options
    //
    try {
      apply_cli_options(my_planning,
                        use_single_bus,
                        use_kirchhoff,
                        use_lp_names,
                        input_directory,
                        input_format,
                        output_directory,
                        output_format,
                        compression_format);
    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error applying CLI options: {}", ex.what()));
    }

    //
    // Write JSON output if requested
    //
    if (json_file) {
      try {
        spdlog::stopwatch sw;

        std::filesystem::path jpath(json_file.value());

        try {
          jpath.replace_extension(".json");
        } catch (const std::filesystem::filesystem_error& ex) {
          return std::unexpected(std::format(
              "Filesystem error processing JSON output path '{}': {}",
              json_file.value(),
              ex.what()));
        }

        std::ofstream jfile(jpath);
        if (!jfile) {
          return std::unexpected(std::format(
              "Failed to create JSON output file '{}'", jpath.string()));
        }

        try {
          jfile << daw::json::to_json(my_planning) << '\n';

          if (!jfile) {
            return std::unexpected(std::format(
                "Failed to write JSON output file '{}'", jpath.string()));
          }
        } catch (const daw::json::json_exception& ex) {
          return std::unexpected(
              std::format("JSON serialization error for file '{}': {}",
                          jpath.string(),
                          ex.what()));
        }

        spdlog::info(std::format("writing system json file {}s", sw));

      } catch (const std::exception& ex) {
        return std::unexpected(std::format(
            "Error writing JSON file '{}': {}", json_file.value(), ex.what()));
      }
    }

    //
    // create and load the lp
    //
    try {
      const auto flat_opts = make_flat_options(use_lp_names, matrix_eps);

      // --- pre-solve stats ---
      const bool do_stats = print_stats.value_or(false);
      if (do_stats) {
        const auto& sys = my_planning.system;
        const auto& sim = my_planning.simulation;
        const auto& opts = my_planning.options;
        spdlog::info("=== System statistics ===");
        spdlog::info(std::format("  System name     : {}", sys.name));
        spdlog::info(
            std::format("  Buses           : {}", sys.bus_array.size()));
        spdlog::info(
            std::format("  Generators      : {}", sys.generator_array.size()));
        spdlog::info(
            std::format("  Demands         : {}", sys.demand_array.size()));
        spdlog::info(
            std::format("  Lines           : {}", sys.line_array.size()));
        spdlog::info(
            std::format("  Batteries       : {}", sys.battery_array.size()));
        spdlog::info(
            std::format("  Converters      : {}", sys.converter_array.size()));
        spdlog::info(std::format("  Reserve zones   : {}",
                                 sys.reserve_zone_array.size()));
        spdlog::info(
            std::format("  Junctions       : {}", sys.junction_array.size()));
        spdlog::info(
            std::format("  Reservoirs      : {}", sys.reservoir_array.size()));
        spdlog::info(
            std::format("  Turbines        : {}", sys.turbine_array.size()));
        spdlog::info("=== Simulation statistics ===");
        spdlog::info(
            std::format("  Blocks          : {}", sim.block_array.size()));
        spdlog::info(
            std::format("  Stages          : {}", sim.stage_array.size()));
        spdlog::info(
            std::format("  Scenarios       : {}", sim.scenario_array.size()));
        spdlog::info("=== Key options ===");
        spdlog::info(
            std::format("  use_kirchhoff   : {}",
                        opts.use_kirchhoff.value_or(false) ? "true" : "false"));
        spdlog::info(std::format(
            "  use_single_bus  : {}",
            opts.use_single_bus.value_or(false) ? "true" : "false"));
        spdlog::info(std::format("  scale_objective : {}",
                                 opts.scale_objective.value_or(1'000.0)));
        spdlog::info(std::format("  demand_fail_cost: {}",
                                 opts.demand_fail_cost.value_or(0.0)));
        spdlog::info(std::format("  input_directory : {}",
                                 opts.input_directory.value_or("(default)")));
        spdlog::info(std::format("  output_directory: {}",
                                 opts.output_directory.value_or("(default)")));
        spdlog::info(std::format("  output_format   : {}",
                                 opts.output_format.value_or("csv")));
      }

      spdlog::stopwatch sw;
      PlanningLP planning_lp {std::move(my_planning),  // NOLINT
                              flat_opts};
      spdlog::info(std::format("creating lp {}s", sw));

      if (lp_file) {
        try {
          planning_lp.write_lp(lp_file.value());
        } catch (const std::exception& ex) {
          return std::unexpected(std::format(
              "Error writing LP file '{}': {}", lp_file.value(), ex.what()));
        }
      }

      if (just_create.value_or(false)) {
        return 0;
      }

      bool optimal = false;
      double solve_elapsed = 0.0;
      {
        spdlog::stopwatch solve_sw;

        const SolverOptions lp_opts {};
        auto result = planning_lp.resolve(lp_opts);
        solve_elapsed =
            std::chrono::duration<double>(solve_sw.elapsed()).count();
        spdlog::info(std::format("planning  {:.3f}s", solve_elapsed));

        optimal = result.has_value();

        if (!optimal) {
          const auto& err = result.error();
          spdlog::error(std::format("Solver failed: {} (code={})",
                                    err.message,
                                    static_cast<int>(err.code)));
          spdlog::error(
              std::format("  Solver options used:"
                          " algorithm={}, threads={}, presolve={},"
                          " optimal_eps={}, feasible_eps={}, barrier_eps={},"
                          " log_level={}",
                          lp_opts.algorithm,
                          lp_opts.threads,
                          lp_opts.presolve,
                          lp_opts.optimal_eps,
                          lp_opts.feasible_eps,
                          lp_opts.barrier_eps,
                          lp_opts.log_level));
        }
      }

      // --- post-solve stats ---
      if (do_stats) {
        spdlog::info("=== Solution statistics ===");
        spdlog::info(std::format("  Status          : {}",
                                 optimal ? "optimal" : "non-optimal"));
        spdlog::info(std::format("  Solve time      : {:.3f}s", solve_elapsed));

        if (optimal && !planning_lp.systems().empty()
            && !planning_lp.systems().front().empty())
        {
          const auto& lp_if =
              planning_lp.systems().front().front().linear_interface();
          const auto& plp_opts = planning_lp.options();
          const double scale = plp_opts.scale_objective();
          const double obj_scaled = lp_if.get_obj_value();
          const double obj_unscaled = obj_scaled * scale;
          spdlog::info(
              std::format("  LP variables    : {}", lp_if.get_numcols()));
          spdlog::info(
              std::format("  LP constraints  : {}", lp_if.get_numrows()));
          spdlog::info(std::format("  Obj (scaled)    : {:.6g}", obj_scaled));
          spdlog::info(std::format("  Obj (unscaled)  : {:.6g}", obj_unscaled));
          spdlog::info(std::format("  scale_objective : {:.6g}", scale));
          spdlog::info(
              std::format("  Solver kappa    : {:.6g}", lp_if.get_kappa()));
        }
      }

      if (optimal) {
        spdlog::stopwatch out_sw;

        try {
          planning_lp.write_out();
        } catch (const std::exception& ex) {
          return std::unexpected(
              std::format("Error writing output: {}", ex.what()));
        }

        spdlog::info(std::format("writing output  {}s", out_sw));
      }

      return optimal ? 0 : 1;

    } catch (const std::bad_alloc& ex) {
      return std::unexpected(
          std::format("Out of memory while creating LP: {}", ex.what()));
    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error during LP creation or solving: {}", ex.what()));
    }

  } catch (const std::bad_alloc& ex) {
    return std::unexpected(
        std::format("Critical: Out of memory: {}", ex.what()));
  } catch (const std::exception& ex) {
    return std::unexpected(
        std::format("Unexpected critical error: {}", ex.what()));
  } catch (...) {
    return std::unexpected("Unknown critical error occurred");
  }
}

}  // namespace gtopt
