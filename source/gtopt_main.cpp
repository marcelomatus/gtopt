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
    const std::optional<bool>& fast_parsing)
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

    Planning planning;
    {
      spdlog::stopwatch sw;

      for (auto&& planning_file : planning_files) {
        try {
          std::filesystem::path fpath(planning_file);

          // Filesystem operations can throw
          try {
            fpath.replace_extension(".json");
          } catch (const std::filesystem::filesystem_error& ex) {
            return std::unexpected(
                std::format("Filesystem error processing path '{}': {}",
                            planning_file,
                            ex.what()));
          }

          // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - stream leak is in
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
              planning.merge(std::move(plan));
            } else {
              auto plan = daw::json::from_json<Planning>(json_result.value(),
                                                         FastParsePolicy);
              planning.merge(std::move(plan));
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
      apply_cli_options(planning,
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
          jfile << daw::json::to_json(planning) << '\n';

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

      spdlog::stopwatch sw;
      PlanningLP planning_lp {std::move(planning), flat_opts};
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
      {
        spdlog::stopwatch sw;

        auto result = planning_lp.resolve();
        spdlog::info(std::format("planning  {}s", sw));

        optimal = result.has_value();

        if (!optimal && result.error().is_error()) {
          spdlog::warn(std::format("Solver returned non-optimal solution: {}",
                                   result.error().message));
        }
      }

      if (optimal) {
        spdlog::stopwatch sw;

        try {
          planning_lp.write_out();
        } catch (const std::exception& ex) {
          return std::unexpected(
              std::format("Error writing output: {}", ex.what()));
        }

        spdlog::info(std::format("writing output  {}s", sw));
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
