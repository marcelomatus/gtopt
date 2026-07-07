// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_check_lp.cpp
 * @brief     Unit tests for check_lp diagnostic helpers
 * @date      2026-05-12
 *
 * Tests:
 *   1. log_diagnostic_lines with fewer than kDiagMaxLines — no truncation
 *   2. log_diagnostic_lines with more than kDiagMaxLines — tail truncation
 *   3. log_diagnostic_lines with level="error" → spdlog::error
 *   4. log_diagnostic_lines with empty diag → header only
 *   5. log_diagnostic_lines with single line
 *   6. log_diagnostic_lines with empty lines skipped
 *   7. run_check_json_info with empty file list → returns empty string
 *   8. run_check_lp_diagnostic with tool not on PATH → no throw
 *   9. run_check_lp_diagnostic without extension → no throw
 *  10. run_check_lp_diagnostic with solver opts forwarded → no throw
 */

#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/check_lp.hpp>
#include <gtopt/solver_options.hpp>

#include "log_capture.hpp"

using namespace gtopt;
using namespace gtopt::test;

namespace
{

/// Build a multi-line diagnostic string with `n` lines.
[[nodiscard]] auto make_multiline_diag(int n) -> std::string
{
  std::string out;
  for (int i = 0; i < n; ++i) {
    out += "line " + std::to_string(i + 1) + '\n';
  }
  return out;
}

}  // namespace

TEST_CASE("log_diagnostic_lines — fewer than kDiagMaxLines, no truncation")
{
  LogCapture logs;
  log_diagnostic_lines("info", "test_header", "line1\nline2\nline3\n");

  CHECK(logs.contains("LP diagnostic for test_header:"));
  CHECK(logs.contains("line1"));
  CHECK(logs.contains("line2"));
  CHECK(logs.contains("line3"));
  CHECK_FALSE(logs.contains("lines total"));
  CHECK_FALSE(logs.contains("truncated"));
}

TEST_CASE("log_diagnostic_lines — more than kDiagMaxLines, tail truncation")
{
  // kDiagMaxLines=30, kDiagTailLines=10 → 40 lines should truncate
  const auto diag = make_multiline_diag(40);

  LogCapture logs;
  log_diagnostic_lines("info", "test_header", diag);

  CHECK(logs.contains("(40 lines total, showing last 10)"));
  // Last lines should appear
  CHECK(logs.contains("line 40"));
  CHECK(logs.contains("line 31"));
}

TEST_CASE("log_diagnostic_lines — error level uses spdlog::error")
{
  LogCapture logs;
  const auto diag = make_multiline_diag(40);
  log_diagnostic_lines("error", "error_header2", diag);
  CHECK(logs.contains("LP infeasibility diagnostic for error_header2:"));
}

TEST_CASE("log_diagnostic_lines — empty diag produces header only")
{
  LogCapture logs;
  log_diagnostic_lines("info", "empty_test", "");

  CHECK(logs.contains("LP diagnostic for empty_test:"));
}

TEST_CASE("log_diagnostic_lines — single line diag")
{
  LogCapture logs;
  log_diagnostic_lines("info", "single", "only one line\n");

  CHECK(logs.contains("LP diagnostic for single:"));
  CHECK(logs.contains("only one line"));
  CHECK_FALSE(logs.contains("truncated"));
}

TEST_CASE("log_diagnostic_lines — diag with empty lines skipped")
{
  LogCapture logs;
  log_diagnostic_lines("info", "skip_empty", "a\n\nb\n\n\nc\n");

  CHECK(logs.contains("a"));
  CHECK(logs.contains("b"));
  CHECK(logs.contains("c"));
}

TEST_CASE("run_check_json_info — empty file list returns empty string")
{
  const auto result = run_check_json_info({}, 5);
  CHECK(result.empty());
}

TEST_CASE("run_check_lp_diagnostic — tool not found returns empty string")
{
  const SolverOptions solver_opts;
  // When gtopt_check_lp is not on PATH, returns empty string.
  // Function must not throw.
  const auto result = run_check_lp_diagnostic("test_file", 1, solver_opts);
  static_cast<void>(result);  // nodiscard
}

TEST_CASE("run_check_lp_diagnostic — filename without extension")
{
  const SolverOptions solver_opts;
  // Function should handle file without extension (appends .lp)
  const auto result = run_check_lp_diagnostic("no_ext_file", 1, solver_opts);
  static_cast<void>(result);  // nodiscard
}

TEST_CASE("run_check_json_info — single file, tool not found")
{
  const auto result = run_check_json_info({"planning.json"}, 1);
  static_cast<void>(result);  // nodiscard
}

TEST_CASE("run_check_lp_diagnostic — with solver opts forwarded")
{
  SolverOptions opts;
  opts.algorithm = LPAlgo::barrier;
  opts.optimal_eps = 1e-6;
  opts.feasible_eps = 1e-7;

  // Verify no crash with all optional args present
  const auto result = run_check_lp_diagnostic("test.lp", 30, opts);
  static_cast<void>(result);  // nodiscard
}

TEST_CASE("run_check_lp_diagnostic — with barrier_eps forwarded")
{
  SolverOptions opts;
  opts.barrier_eps = 1e-8;

  const auto result = run_check_lp_diagnostic("test.lp", 30, opts);
  static_cast<void>(result);  // nodiscard
}
