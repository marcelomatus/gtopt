/**
 * @file      resolve_planning_args.hpp
 * @brief     Resolve planning file arguments and directory context
 * @date      2026-03-21
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a single free function that normalises the planning-file list
 * and directory options before any JSON parsing takes place.  Three
 * convenience features are implemented:
 *
 * 1. **Directory as argument**: `dir` → `dir/basename(dir).json` with
 *    input/output directories set relative to `dir`.
 * 2. **Bare stem fallback**: `name` (not a dir, not a file) → `name.json`.
 * 3. **Auto-detect CWD**: no files given → look for `cwd_name.json`.
 */

#pragma once

#include <expected>
#include <string>

#include <gtopt/gtopt_main.hpp>

namespace gtopt
{

/**
 * @brief Resolve planning file arguments and directory context.
 *
 * See the file-level documentation for the three features implemented.
 *
 * @param opts  A copy of the original MainOptions (modified in place).
 * @return The resolved MainOptions, or an error string.
 */
[[nodiscard]] std::expected<MainOptions, std::string> resolve_planning_args(
    MainOptions opts);

}  // namespace gtopt
