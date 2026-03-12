/**
 * @file      check_lp.hpp
 * @brief     Utility for running gtopt-check-lp diagnostics on error LP files
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides run_check_lp_diagnostic() which looks for the gtopt-check-lp
 * script on PATH and, if found, runs it with --analyze-only --no-color on the
 * given LP file, returning the captured output for logging.
 */

#pragma once

#include <string>

namespace gtopt
{

/**
 * @brief Run gtopt-check-lp on an LP file and return the diagnostic output.
 *
 * Searches PATH for the @c gtopt-check-lp binary.  If found, executes:
 * @code
 *   gtopt-check-lp --analyze-only --no-color --timeout <timeout_seconds> <lp_file>
 * @endcode
 * and returns the captured stdout+stderr.  If the binary is not on PATH or
 * the file does not exist, an empty string is returned so callers can skip
 * logging silently.
 *
 * @param lp_file        Full path to the LP file (may include or omit the
 *                       .lp extension).
 * @param timeout_seconds Maximum execution time in seconds (default: 10).
 * @return Captured diagnostic output, or an empty string if unavailable.
 */
[[nodiscard]] std::string run_check_lp_diagnostic(
    const std::string& lp_file,
    int timeout_seconds = 10);

}  // namespace gtopt
