/**
 * @file      gtopt_json_io.hpp
 * @brief     JSON parsing, serialization, and --set overlay for Planning
 * @date      2026-04-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Separated from gtopt_main.cpp so that the heavy DAW JSON headers
 * compile in their own translation unit.
 */

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/planning.hpp>

namespace gtopt
{

/// Parse planning JSON files into a Planning object.
///
/// Iterates over each path in @p planning_files, reads the JSON, and merges
/// it into a single Planning.  Returns an error string on the first failure.
[[nodiscard]] std::expected<Planning, std::string> parse_planning_files(
    const std::vector<std::string>& planning_files,
    const std::optional<std::string>& input_directory = {});

/// Write the merged Planning to a JSON file.
[[nodiscard]] std::expected<void, std::string> write_json_output(
    const Planning& planning, const std::string& json_file);

/// Apply all `--set key=value` overrides to a Planning object.
/// Returns true if all options were applied successfully.
[[nodiscard]] bool apply_set_options(
    Planning& planning, const std::vector<std::string>& set_options);

/// Load user constraints from external file(s) referenced in planning.
[[nodiscard]] std::expected<void, std::string> load_user_constraints(
    Planning& planning);

}  // namespace gtopt
