/**
 * @file      gtopt_json_io_parse.cpp
 * @brief     JSON parsing of Planning files (split from gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains parse_planning_files() — the heaviest DAW JSON instantiation
 * (from_json<Planning> with multiple parse policies).  Splitting into its
 * own TU lets it compile in parallel with the other JSON I/O functions.
 */

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_planning.hpp>
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

/// Same as StrictParsePolicy but rejects any JSON key not listed in the
/// schema. Used by the --check-json pass to surface typos / unknown fields.
constexpr auto ExactParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

}  // namespace

std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    bool strict_parsing,
    bool check_json,
    const std::optional<std::string>& input_directory)
{
  const spdlog::stopwatch sw;
  Planning my_planning;

  for (const auto& planning_file : planning_files) {
    try {
      std::filesystem::path fpath(planning_file);
      fpath.replace_extension(".json");

      // Check existence before calling daw::read_file, which is noexcept
      // and would call std::terminate via std::filesystem::file_size if
      // the file is missing.  If the file is not found in the current
      // directory, try the input_directory as a fallback.
      if (!std::filesystem::exists(fpath) && input_directory.has_value()) {
        auto alt =
            std::filesystem::path(input_directory.value()) / fpath.filename();
        if (std::filesystem::exists(alt)) {
          spdlog::info("  Found '{}' in input_directory '{}'",
                       fpath.filename().string(),
                       input_directory.value());
          fpath = std::move(alt);
        }
      }
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

      // Optional pre-pass: parse with exact-mapping policy to surface
      // any JSON key not listed in the schema.  This parses the file a
      // second time (performance trade-off), but only when --check-json
      // is explicitly requested.  Warnings only — parsing always
      // proceeds with the normal policy below.
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

}  // namespace gtopt
