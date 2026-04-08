/**
 * @file      gtopt_lp_runner.hpp
 * @brief     LP build, solve, stats, and output writing
 * @date      2026-04-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Separated from gtopt_main.cpp so that the heavy Arrow/Parquet headers
 * (pulled in via planning_lp.hpp) compile in their own translation unit.
 */

#pragma once

#include <expected>
#include <string>

#include <gtopt/planning.hpp>

namespace gtopt
{

struct MainOptions;

/// Build the LP model, solve it, and write output.
///
/// Wraps the full LP pipeline: matrix construction, optional LP-file dump,
/// coefficient stats, solver invocation, post-solve stats, and output
/// writing.  Returns 0 on success, 1 when the solver finds no optimal
/// solution, or an error string on hard failures.
[[nodiscard]] std::expected<int, std::string> build_solve_and_output(
    Planning&& planning, const MainOptions& opts);

}  // namespace gtopt
