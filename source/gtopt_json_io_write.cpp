/**
 * @file      gtopt_json_io_write.cpp
 * @brief     JSON serialization of Planning (split from gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains write_json_output() — instantiates to_json(Planning).
 * Splitting into its own TU lets it compile in parallel with parsing.
 */

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_planning.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

std::expected<void, std::string> write_json_output(const Planning& planning,
                                                   const std::string& json_file)
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

  spdlog::info("  Write system json file time {:.3f}s", sw.elapsed().count());
  return {};
}

}  // namespace gtopt
