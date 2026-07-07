/**
 * @file      test_parse_long_mantissa.cpp
 * @brief     Regression: JSON number literals with more significant digits
 *            than fit in a uint64_t must parse to the correct double.
 * @copyright BSD-3-Clause
 *
 * daw-json-link's default (fast) real parser silently mis-parses a number
 * literal with > ~19 significant digits by orders of magnitude when the
 * document is fed as a `std::string` — exactly how `parse_planning_files`
 * feeds the buffer it reads from disk.  The strtod fallback that handles
 * over-long significands is compiled out unless `IEEE754Precise::yes` is
 * set on the parse policy.  `StrictParsePolicy` now sets that flag.
 *
 * See https://github.com/beached/daw_json_link/issues/474 (daw 3.31.0).
 *
 * Without the fix, parsing `0.33333333333333333333` (20 digits) from a file
 * yields ~0.1489 instead of 0.3333; longer literals collapse toward 1e-20.
 */

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>

using namespace gtopt;

namespace
{
// 21-significant-digit duration.  The nearest double is the same one strtod
// returns for the literal: 1.2345678901234566.
constexpr std::string_view kLongMantissaJson = R"({
  "options": {},
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1.23456789012345678901}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "tiny",
    "bus_array": [{"uid": 1, "name": "b1"}]
  }
})";
}  // namespace

TEST_CASE(  // NOLINT
    "parse_planning_files - long-mantissa number parses to correct double "
    "(daw_json_link #474 regression)")
{
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_parse_long_mantissa";
  std::filesystem::create_directories(tmpdir);
  const auto path = (tmpdir / "long_mantissa.json").string();
  std::ofstream(path) << kLongMantissaJson;

  // The file path drives the vulnerable std::string parse path inside
  // parse_planning_files (daw::read_file -> canonicalize -> from_json).
  const auto result = parse_planning_files({path}, std::nullopt);

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->simulation.block_array.empty());

  const double duration = result->simulation.block_array.front().duration;
  // Ground truth: the IEEE-754 nearest double to the literal.
  const double expected =
      std::strtod("1.23456789012345678901", nullptr);  // 1.2345678901234566

  CHECK(duration == doctest::Approx(expected).epsilon(1e-12));
  // Guard against the catastrophic mis-parse explicitly: the broken fast
  // path returned a value 5+ orders of magnitude too small.
  CHECK(duration > 1.0);
  CHECK(duration < 2.0);
}
