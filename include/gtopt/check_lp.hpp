/**
 * @file      check_lp.hpp
 * @brief     Utility for running gtopt_check_lp diagnostics on error LP files
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides run_check_lp_diagnostic() which looks for the gtopt_check_lp
 * script on PATH and, if found, spawns it directly via @c posix_spawn (no
 * shell) with @c --quiet @c --no-color, returning the captured output for
 * logging.
 *
 * The @c --quiet flag makes gtopt_check_lp:
 *   - never fail (always exits with code 0),
 *   - never block for user input,
 *   - try every available local solver and optionally NEOS,
 *   - warn instead of error on missing config, missing LP file, or solver
 *     failures.
 */

#pragma once

#include <string>

namespace gtopt
{

/**
 * @brief Run gtopt_check_lp on an LP file and return the diagnostic output.
 *
 * Searches PATH for the @c gtopt_check_lp binary.  If found, spawns it
 * directly (without invoking a shell) via @c posix_spawn with:
 * @code
 *   gtopt_check_lp --quiet --no-color --timeout <timeout_seconds> <lp_file>
 * @endcode
 * and returns the captured stdout+stderr.  If the binary is not on PATH or
 * the file does not exist, an empty string is returned so callers can skip
 * logging silently.
 *
 * The @c --quiet flag ensures the child process never stalls waiting for
 * input and always exits with code 0, even when no solver is available or
 * NEOS is unreachable.
 *
 * @param lp_file        Full path to the LP file (may include or omit the
 *                       .lp extension).
 * @param timeout_seconds Maximum execution time in seconds (default: 10).
 * @return Captured diagnostic output, or an empty string if unavailable.
 */
[[nodiscard]] std::string run_check_lp_diagnostic(const std::string& lp_file,
                                                  int timeout_seconds = 10);

}  // namespace gtopt
