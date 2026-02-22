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
 *   - stats=true (pre- and post-solve statistics)
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
  auto result = gtopt_main(MainOptions {
      .planning_files = {"nonexistent_planning_file_xyz"},
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - just_create=true completes successfully")
{
  const auto stem = write_tmp_json("gtopt_main_just_create", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .just_create = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - fast_parsing path, just_create=true")
{
  const auto stem = write_tmp_json("gtopt_main_fast_parse", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .just_create = true,
      .fast_parsing = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - json_file output written")
{
  const auto stem = write_tmp_json("gtopt_main_json_out", minimal_json);
  const auto json_out =
      (std::filesystem::temp_directory_path() / "gtopt_main_out").string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .json_file = json_out,
      .just_create = true,
  });
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
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_output").string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - stats=true, just_create (no crash)")
{
  const auto stem = write_tmp_json("gtopt_main_stats_create", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .just_create = true,
      .print_stats = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - stats=true, full solve")
{
  const auto stem = write_tmp_json("gtopt_main_stats_solve", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_stats_out")
          .string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .print_stats = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - multiple planning files merged")
{
  // Split the minimal JSON into two fragments that merge into one valid plan:
  // part1 provides options + simulation, part2 provides the system components.
  constexpr auto part1 = R"({
    "options": {"demand_fail_cost": 1000},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {"name": "merge_test"}
  })";

  constexpr auto part2 = R"({
    "system": {
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto stem1 = write_tmp_json("gtopt_main_merge1", part1);
  const auto stem2 = write_tmp_json("gtopt_main_merge2", part2);

  // just_create=true validates parsing + merging without needing a full solve
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem1.string(), stem2.string()},
      .just_create = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - returns error for invalid JSON content")
{
  const auto stem = write_tmp_json("gtopt_main_bad_json", "{ invalid json !!!");
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - stats=true, just_create (covers log_pre_solve_stats)")
{
  // Richer system to exercise more stat counters
  constexpr auto rich_json = R"({
    "options": {"demand_fail_cost": 500},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 2}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "rich_stats_test",
      "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 100.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ],
      "line_array": [
        {"uid": 1, "name": "l1", "bus_a": 1, "bus_b": 2,
         "tmax_ab": 100.0, "tmax_ba": 100.0, "reactance": 0.1}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_rich_stats", rich_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .just_create = true,
      .print_stats = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}
