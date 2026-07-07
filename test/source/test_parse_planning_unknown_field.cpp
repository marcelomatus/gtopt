/**
 * @file      test_parse_planning_unknown_field.cpp
 * @brief     Regression: parse_planning_files must report unknown-field
 *            errors cleanly (no segfault).
 * @copyright BSD-3-Clause
 *
 * Reproduces the segfault triggered by a stale field in
 * `support/plp/2_years/.../bat_<something>.json` once the
 * `StrictParsePolicy` rejected the unknown key.  The crash was in
 * `daw::json::to_formatted_string` when it tried to render the source
 * context: the catch in `parse_planning_files` passed it the
 * pre-canonicalisation buffer while the `json_exception` carried
 * pointers into the canonicalised buffer.  The mismatched offsets sent
 * the formatter's "walk back to the previous newline" loop past the
 * start of the buffer and into unmapped memory.
 *
 * The fix moves `canonical` out of the inner try-block so it outlives
 * the catch handler, then passes `canonical.c_str()` to
 * `to_formatted_string`.  This test pins that contract.
 */

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>

using namespace gtopt;

namespace
{
// NOLINTBEGIN(cert-err58-cpp)
const std::string kUnknownTopLevelJson = R"({
  "options": {},
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1.0}],
    "stage_array": [{"uid": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "tiny",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "totally_made_up_array": [{"uid": 999, "field_that_does_not_exist": 42}]
  }
})";

// Same shape as the BAT_ALICANTO trigger: an unknown field deep inside
// a nested array element.  `gcost` is the post-`e54b795a4` stale field
// on Battery that the bat_4b regression already covers; here we use a
// freshly invented name so the test stays independent of future
// renames.
const std::string kBatteryUnknownFieldJson = R"({
  "options": {},
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1.0}],
    "stage_array": [{"uid": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "tiny",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "battery_array": [{
      "uid": 1,
      "name": "bat1",
      "bus": "b1",
      "emax": 100,
      "emin": 0,
      "capacity": 100,
      "pmax_charge": 10,
      "pmax_discharge": 10,
      "input_efficiency": 0.9,
      "output_efficiency": 0.9,
      "this_field_does_not_exist_in_battery_schema_v2": 12345
    }]
  }
})";

// The `output_layout` option was removed when output became long-only.  A
// JSON that still carries it must be rejected (strict parse), prompting the
// user to drop the key rather than silently ignoring a now-meaningless value.
const std::string kRemovedOutputLayoutJson = R"({
  "options": {"output_layout": "wide"},
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1.0}],
    "stage_array": [{"uid": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "tiny",
    "bus_array": [{"uid": 1, "name": "b1"}]
  }
})";
// NOLINTEND(cert-err58-cpp)

std::string write_temp_json(const std::filesystem::path& dir,
                            std::string_view stem,
                            std::string_view content)
{
  const auto path = dir / (std::string {stem} + ".json");
  std::ofstream(path) << content;
  return path.string();
}
}  // namespace

TEST_CASE(  // NOLINT
    "parse_planning_files - unknown top-level array returns std::unexpected "
    "with a non-empty error message (no segfault)")
{
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_parse_unknown_field";
  std::filesystem::create_directories(tmpdir);
  const auto path =
      write_temp_json(tmpdir, "unknown_top_level", kUnknownTopLevelJson);

  const auto result = parse_planning_files({path}, std::nullopt);

  REQUIRE_FALSE(result.has_value());
  const auto& err = result.error();
  // Sanity: message names the file and mentions JSON.
  CHECK(err.find("JSON parsing error") != std::string::npos);
  CHECK(err.find(path) != std::string::npos);
  // The error message must be non-empty / printable — the original bug
  // segfaulted inside `to_formatted_string` before it could return.
  CHECK(err.size() > 64);
}

TEST_CASE(  // NOLINT
    "parse_planning_files - unknown nested battery field reports cleanly "
    "(BAT_ALICANTO segfault regression)")
{
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_parse_unknown_field";
  std::filesystem::create_directories(tmpdir);
  const auto path =
      write_temp_json(tmpdir, "battery_unknown", kBatteryUnknownFieldJson);

  const auto result = parse_planning_files({path}, std::nullopt);

  REQUIRE_FALSE(result.has_value());
  const auto& err = result.error();
  CHECK(err.find("JSON parsing error") != std::string::npos);
  // The formatter should successfully render *some* source context
  // (the original bug segfaulted before returning any context).
  CHECK(err.size() > 64);
}

TEST_CASE(  // NOLINT
    "parse_planning_files - removed `output_layout` option is rejected")
{
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_parse_unknown_field";
  std::filesystem::create_directories(tmpdir);
  const auto path = write_temp_json(
      tmpdir, "removed_output_layout", kRemovedOutputLayoutJson);

  const auto result = parse_planning_files({path}, std::nullopt);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("JSON parsing error") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "parse_planning_json - unknown field throws json_exception (no segfault)")
{
  // Direct entry point bypasses parse_planning_files's std::expected
  // wrapper and surfaces the json_exception itself.  Used to confirm
  // that the `to_formatted_string` call site is the only place where
  // the dangling-pointer pattern can arise.
  CHECK_THROWS(
      []
      {
        [[maybe_unused]] const auto _ =
            parse_planning_json(kUnknownTopLevelJson);
      }());
}
