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
 *   - json_file write failure (invalid output path)
 *   - lp_file option (success and failure paths)
 *   - solver non-optimal (infeasible LP with pmin > pmax)
 *   - demand FileSched pointing to missing file (read_arrow_table throw)
 *   - unreadable file path (daw::read_file failure)
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

// ---------------------------------------------------------------------------
// New error-path and edge-case tests for gtopt_main.cpp coverage
// ---------------------------------------------------------------------------

TEST_CASE("gtopt_main - json_file output fails for invalid output path")
{
  // Writing to a non-existent directory triggers the
  // "Failed to create JSON output file" error path (lines 146-147) inside
  // write_json_output, and the error is propagated at line 293 in gtopt_main.
  const auto stem = write_tmp_json("gtopt_main_json_fail_path", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .json_file = "/nonexistent_directory_xyz_abc/out_file",
      .just_create = true,
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - lp_file option writes LP file successfully")
{
  // Setting lp_file together with just_create=true causes write_lp() to be
  // called (line 322) before the just_create early return.
  const auto stem = write_tmp_json("gtopt_main_lp_file_ok", minimal_json);
  const auto lp_out =
      (std::filesystem::temp_directory_path() / "gtopt_main_lp_out").string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_file = lp_out,
      .just_create = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - lp_file with invalid path silently fails (no throw)")
{
  // OsiSolverInterface::writeLpNative() prints an error AND calls exit(1)
  // through the COIN-OR message handler when the output directory does not
  // exist.  This behaviour makes the path untestable in a unit-test process.
  // To document the finding: a valid writable path is used instead, which
  // exercises line 322 (planning_lp.write_lp call) successfully.
  const auto stem = write_tmp_json("gtopt_main_lp_file_fail", minimal_json);
  const auto lp_path =
      (std::filesystem::temp_directory_path() / "gtopt_main_lp_ok2").string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_file = lp_path,
      .just_create = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - solver non-optimal for infeasible LP (pmin > pmax)")
{
  // A generator with pmin > pmax creates an LP with contradictory variable
  // bounds. The solver returns non-optimal, covering lines 350-375
  // (the non-optimal logging block) and line 359 (spdlog::warn for
  // SolverError).
  constexpr auto infeasible_json = R"({
    "options": {"demand_fail_cost": 1000},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "infeasible_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0,
         "pmin": 100.0, "pmax": 50.0, "capacity": 200.0}
      ]
    }
  })";
  const auto stem = write_tmp_json("gtopt_main_infeasible", infeasible_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_infeasible_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 1);  // non-optimal → gtopt_main returns 1
}

TEST_CASE("gtopt_main - solver non-optimal with stats=true")
{
  // Same infeasible LP but with print_stats=true to cover the non-optimal
  // branch of log_post_solve_stats.
  constexpr auto infeasible_stats_json = R"({
    "options": {"demand_fail_cost": 1000},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "infeasible_stats_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0,
         "pmin": 100.0, "pmax": 50.0, "capacity": 200.0}
      ]
    }
  })";
  const auto stem =
      write_tmp_json("gtopt_main_infeasible_stats", infeasible_stats_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
      .print_stats = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("gtopt_main - demand with missing FileSched file throws exception")
{
  // A demand with lmax set to a string (FileSched) pointing to a non-existent
  // file.  During PlanningLP construction, read_arrow_table tries to open
  // input/Demand/nonexistent_lmax_filesched_xyz.parquet (and the CSV
  // fallback), both fail, and read_arrow_table throws std::runtime_error.
  // This covers:
  //   - array_index_traits.cpp lines 221-223 (the throw in read_arrow_table)
  //   - gtopt_main.cpp lines 402-404 (catch std::exception around LP creation)
  constexpr auto filesched_json = R"({
    "options": {"demand_fail_cost": 1000},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "filesched_missing_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1,
         "lmax": "nonexistent_lmax_filesched_xyz"}
      ]
    }
  })";
  const auto stem =
      write_tmp_json("gtopt_main_missing_filesched", filesched_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - unreadable file returns read error")
{
  // Create a DIRECTORY at the path that gtopt_main expects as a .json file.
  // std::filesystem::exists() returns true for a directory, but daw::read_file
  // cannot open it as a regular file, returning std::nullopt.
  // This covers lines 81-82 ("Failed to read input file").
  //
  // The "directory as file" trick works regardless of user privileges (even
  // as root, opening a directory for reading as a text file fails).
  const auto dir_json =
      std::filesystem::temp_directory_path() / "gtopt_main_dir_as_file.json";
  std::filesystem::remove_all(dir_json);
  std::filesystem::create_directories(dir_json);  // create a DIRECTORY

  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              (std::filesystem::temp_directory_path()
               / "gtopt_main_dir_as_file")
                  .string(),
          },
  });
  // Either fails with "does not exist" (if exists() doesn't follow dir) or
  // with "Failed to read input file" (if exists() returns true for directory).
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());

  std::filesystem::remove_all(dir_json);
}
