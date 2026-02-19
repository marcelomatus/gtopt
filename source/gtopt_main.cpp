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
      std::filesystem::path fpath(planning_file);
      fpath.replace_extension(".json");
      // NOLINTNEXTLINE(clang-analyzer-unix.Stream) - stream leak is in
      // third-party daw_read_file.h
      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        return std::unexpected(
            std::format("problem reading input file {}", planning_file));
      }
      spdlog::info(std::format("parsing input file {}", fpath.string()));

      auto&& json_doc = json_result.value();
      try {
        if (strict_parsing) {
          auto plan =
              daw::json::from_json<Planning>(json_doc, StrictParsePolicy);
          planning.merge(std::move(plan));
        } else {
          auto plan = daw::json::from_json<Planning>(json_doc, FastParsePolicy);
          planning.merge(std::move(plan));
        }
      } catch (daw::json::json_exception const& jex) {
        spdlog::critical(
            std::format("parsing file {} failed, {}",
                        fpath.string(),
                        to_formatted_string(jex, json_doc.c_str())));
      } catch (...) {
        throw;
      }
    }

    spdlog::info(std::format("parsing all json files {}s", sw));
  }

  //
  // update the planning options
  //
  apply_cli_options(planning,
                    use_single_bus,
                    use_kirchhoff,
                    use_lp_names,
                    input_directory,
                    input_format,
                    output_directory,
                    output_format,
                    compression_format);

  //

  if (json_file) {
    spdlog::stopwatch sw;

    std::filesystem::path jpath(json_file.value());
    jpath.replace_extension(".json");
    std::ofstream jfile(jpath);
    if (jfile) {
      jfile << daw::json::to_json(planning) << '\n';
    } else {
      spdlog::error(std::format("can't create json file {}", jpath.string()));
    }

    spdlog::info(std::format("writing system json file {}s", sw));
  }

  //
  // create and load the lp
  //

  const auto flat_opts = make_flat_options(use_lp_names, matrix_eps);

  spdlog::stopwatch sw;
  PlanningLP planning_lp {std::move(planning), flat_opts};
  spdlog::info(std::format("creating lp {}s", sw));

  if (lp_file) {
    planning_lp.write_lp(lp_file.value());
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
  }

  if (optimal) {
    spdlog::stopwatch sw;

    planning_lp.write_out();
    spdlog::info(std::format("writing output  {}s", sw));
  }

  return optimal ? 0 : 1;
}

}  // namespace gtopt
