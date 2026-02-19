/**
 * @file      test_gtopt_main.cpp
 * @brief     Unit tests for the gtopt::gtopt_main() library entry point
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers source/gtopt_main.cpp:
 *   - error path when a planning file does not exist
 *   - fast-parsing path
 *   - just_create=true (build LP but skip solve)
 *   - json_file output
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>

using namespace gtopt;

namespace
{
// Minimal planning JSON usable by gtopt_main (no input-directory needed).
constexpr auto minimal_json = R"({
  "options": {
    "demand_fail_cost": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array":  [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "gtopt_main_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
    ]
  }
})";

// Write content to a .json temp file and return its stem (path without .json).
std::filesystem::path write_tmp_json(const std::string& name,
                                     std::string_view content)
{
  auto tmp = std::filesystem::temp_directory_path() / name;
  tmp.replace_extension(".json");
  std::ofstream ofs(tmp);
  ofs << content;
  return tmp.replace_extension("");  // gtopt_main appends .json itself
}
}  // namespace

TEST_CASE("gtopt_main - returns error for nonexistent file")
{
  const std::vector<std::string> files {"nonexistent_planning_file_xyz"};
  auto result = gtopt_main(files,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt);
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - just_create=true completes successfully")
{
  const auto stem = write_tmp_json("gtopt_main_just_create", minimal_json);
  const std::vector<std::string> files {stem.string()};

  auto result = gtopt_main(files,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::optional<bool>(true),  // just_create
                           std::nullopt);
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - fast_parsing path, just_create=true")
{
  const auto stem = write_tmp_json("gtopt_main_fast_parse", minimal_json);
  const std::vector<std::string> files {stem.string()};

  auto result = gtopt_main(files,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::optional<bool>(true),   // just_create
                           std::optional<bool>(true));  // fast_parsing
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - json_file output written")
{
  const auto stem = write_tmp_json("gtopt_main_json_out", minimal_json);
  const std::vector<std::string> files {stem.string()};

  const auto json_out =
      (std::filesystem::temp_directory_path() / "gtopt_main_out").string();

  auto result = gtopt_main(files,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::optional<std::string>(json_out),  // json_file
                           std::optional<bool>(true),  // just_create
                           std::nullopt);
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  // Verify the JSON output file was created
  const auto out_path =
      std::filesystem::path(json_out).replace_extension(".json");
  CHECK(std::filesystem::exists(out_path));
}

TEST_CASE("gtopt_main - full solve with single-bus override")
{
  const auto stem = write_tmp_json("gtopt_main_solve", minimal_json);
  const std::vector<std::string> files {stem.string()};

  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_output").string();

  auto result = gtopt_main(files,
                           std::nullopt,
                           std::nullopt,
                           std::optional<std::string>(out_dir),  // output_dir
                           std::nullopt,
                           std::nullopt,
                           std::optional<bool>(true),  // use_single_bus
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt,
                           std::nullopt);
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}
