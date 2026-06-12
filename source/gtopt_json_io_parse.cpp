/**
 * @file      gtopt_json_io_parse.cpp
 * @brief     JSON parsing of Planning files (split from gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains parse_planning_files() — instantiates from_json<Planning> with
 * StrictParsePolicy, which rejects unknown JSON fields (exact mapping).
 */

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/json_canonicalize.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

Planning parse_planning_json(std::string_view json_content)
{
  // Rewrite alternative key names (PyPSA / pandapower / PLEXOS /
  // PSR SDDP / PLP / gtopt-modern) to their canonical gtopt form
  // before the strict daw::json parser sees the document.  See
  // include/gtopt/json_canonicalize.hpp and
  // docs/analysis/naming-conventions.md §9 / §10.
  const auto canonical = canonicalize_json_keys(json_content);
  return daw::json::from_json<Planning>(canonical, StrictParsePolicy);
}

std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    const std::optional<std::string>& input_directory,
    std::string_view enforce_dialect)
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

      const auto json_result = daw::read_file(fpath.string());

      if (!json_result) {
        return std::unexpected(
            std::format("Failed to read input file '{}'", planning_file));
      }

      spdlog::info("  Parsing input file {}", fpath.string());

      // `canonical` must outlive the catch block: `daw::json::json_exception`
      // carries raw pointers into the buffer it was parsed from, and
      // `to_formatted_string` walks back from the error position looking
      // for the surrounding line.  Passing a different buffer (the
      // pre-canonicalisation source) causes that walk to segfault when
      // the canonicalisation shifted byte offsets — see the
      // `BAT_ALICANTO` reproducer on `support/plp/2_years`.
      const auto canonical =
          canonicalize_json_keys(json_result.value(), enforce_dialect);
      try {
        auto plan =
            daw::json::from_json<Planning>(canonical, StrictParsePolicy);
        my_planning.merge(std::move(plan));
      } catch (const daw::json::json_exception& jex) {
        return std::unexpected(
            std::format("JSON parsing error in file '{}': {}",
                        fpath.string(),
                        to_formatted_string(jex, canonical.c_str())));
      }

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Unexpected error processing file '{}': {}",
                      planning_file,
                      ex.what()));
    }
  }

  spdlog::info("  Parse all input files time {:.3f}s", sw.elapsed().count());
  return my_planning;
}

}  // namespace gtopt
