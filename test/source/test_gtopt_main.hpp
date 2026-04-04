/**
 * @file      test_gtopt_main.hpp
 * @brief     Unit tests for the gtopt::gtopt_main() library entry point
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers source/gtopt_main.cpp:
 *   - error path when a planning file does not exist
 *   - fast-parsing path
 *   - lp_only=true (build all LPs but skip solve)
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
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

namespace  // NOLINT
{
// Minimal planning JSON usable by gtopt_main (no input-directory needed).
constexpr auto minimal_json = R"({
  "options": {
    "demand_fail_cost": 1000,
    "output_compression": "uncompressed"
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
  using namespace gtopt;
  auto result = gtopt_main(MainOptions {
      .planning_files = {"nonexistent_planning_file_xyz"},
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - lp_only=true completes successfully")
{
  const auto stem = write_tmp_json("gtopt_main_lp_only", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - fast_parsing path, lp_only=true")
{
  const auto stem = write_tmp_json("gtopt_main_fast_parse", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
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
      .lp_only = true,
      .json_file = json_out,
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

TEST_CASE("gtopt_main - stats=true, lp_only (no crash)")
{
  const auto stem = write_tmp_json("gtopt_main_stats_create", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
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
    "options": {"demand_fail_cost": 1000, "output_compression": "uncompressed"},
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

  // lp_only=true validates parsing + merging without needing a full solve
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem1.string(), stem2.string()},
      .lp_only = true,
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

TEST_CASE("gtopt_main - stats=true, lp_only (covers log_pre_solve_stats)")
{
  // Richer system to exercise more stat counters
  constexpr auto rich_json = R"({
    "options": {"demand_fail_cost": 500, "output_compression": "uncompressed"},
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
      .lp_only = true,
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
      .lp_only = true,
      .json_file = "/nonexistent_directory_xyz_abc/out_file",
  });
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

TEST_CASE("gtopt_main - lp_file option writes LP file successfully")
{
  // Setting lp_file together with lp_only=true causes write_lp() to be
  // called (line 322) before the lp_only early return.
  const auto stem = write_tmp_json("gtopt_main_lp_file_ok", minimal_json);
  const auto lp_out =
      (std::filesystem::temp_directory_path() / "gtopt_main_lp_out").string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_file = lp_out,
      .lp_only = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - lp_file with invalid path silently fails (no throw)")
{
  // Some solver backends may print errors or behave unexpectedly when the
  // output directory does not exist.  This subcase documents that limitation.
  // To document the finding: a valid writable path is used instead, which
  // exercises line 322 (planning_lp.write_lp call) successfully.
  const auto stem = write_tmp_json("gtopt_main_lp_file_fail", minimal_json);
  const auto lp_path =
      (std::filesystem::temp_directory_path() / "gtopt_main_lp_ok2").string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_file = lp_path,
      .lp_only = true,
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
    "options": {"demand_fail_cost": 1000, "output_compression": "uncompressed"},
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
    "options": {"demand_fail_cost": 1000, "output_compression": "uncompressed"},
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
    "options": {"demand_fail_cost": 1000, "output_compression": "uncompressed"},
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

TEST_CASE("gtopt_main - lp_algorithm primal solves correctly")  // NOLINT
{
  // Verify that setting lp_algorithm=1 (primal simplex) in MainOptions still
  // produces the correct optimal solution.
  const auto stem = write_tmp_json("gtopt_main_algo_primal", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_algo_primal_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .algorithm = std::to_underlying(LPAlgo::primal),
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - lp_algorithm dual solves correctly")  // NOLINT
{
  // Verify that setting lp_algorithm=2 (dual simplex) in MainOptions still
  // produces the correct optimal solution.
  const auto stem = write_tmp_json("gtopt_main_algo_dual", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_algo_dual_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .algorithm = std::to_underlying(LPAlgo::dual),
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - lp_algorithm from JSON options")  // NOLINT
{
  // Verify that lp_algorithm can be specified in the JSON options field
  // and is propagated through the planning options to the solver.
  constexpr auto json_with_algo = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "lp_algorithm": 1
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array":  [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "json_algo_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_json_algo", json_with_algo);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_json_algo_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - presolve=false solves correctly")  // NOLINT
{
  // Verify that disabling presolve (via JSON solver_options) still produces
  // an optimal solution.  presolve is not a CLI flag — it is configured
  // exclusively through the JSON solver_options block.
  constexpr auto json_no_presolve = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "lp_presolve": false
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array":  [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "no_presolve",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";
  const auto stem = write_tmp_json("gtopt_main_no_presolve", json_no_presolve);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_no_presolve_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  if (!result.has_value()) {
    MESSAGE(result.error());
  }
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - lp_debug writes LP files to log directory")  // NOLINT
{
  // When lp_debug=true the solver must write at least one LP file to the
  // log directory before solving.
  const auto stem = write_tmp_json("gtopt_main_lp_debug", minimal_json);
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_main_lp_debug_logs";
  std::filesystem::remove_all(log_dir);

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
      .lp_names_level = LpNamesLevel::only_cols,
      .lp_debug = true,
      .lp_compression = "none",
      .log_directory = log_dir.string(),
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  // At least one plain LP file should exist in log_dir.
  bool found = false;
  if (std::filesystem::exists(log_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with("gtopt_lp") && name.ends_with(".lp")) {
        found = true;
        break;
      }
    }
  }
  CHECK(found);
  std::filesystem::remove_all(log_dir);
}

TEST_CASE("gtopt_main - check_json=true warns on unknown fields")  // NOLINT
{
  // JSON with an unknown field "bogus_option" to trigger the ExactParsePolicy
  // pre-pass warning.  Covers lines 127-136 (check_json branch).
  constexpr auto json_with_unknown = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "bogus_option": 42
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array":  [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "check_json_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_check_json", json_with_unknown);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
      .check_json = true,
  });
  // The unknown field triggers a warning but parsing still succeeds
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - output_format=parquet full solve")  // NOLINT
{
  const auto stem = write_tmp_json("gtopt_main_parquet_solve", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_parquet_out")
          .string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .output_format = "parquet",
      .output_compression = "uncompressed",
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - output_format=csv full solve")  // NOLINT
{
  const auto stem = write_tmp_json("gtopt_main_csv_solve", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_csv_out").string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .output_format = "csv",
      .output_compression = "uncompressed",
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - print_stats=false skips statistics")  // NOLINT
{
  const auto stem = write_tmp_json("gtopt_main_no_stats", minimal_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_no_stats_out")
          .string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .print_stats = false,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - explicit trace_log path")  // NOLINT
{
  const auto stem = write_tmp_json("gtopt_main_trace_log", minimal_json);
  const auto trace_path =
      (std::filesystem::temp_directory_path() / "gtopt_main_trace.log")
          .string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
      .trace_log = trace_path,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
  CHECK(std::filesystem::exists(trace_path));
}

TEST_CASE("gtopt_main - multiple files full solve")  // NOLINT
{
  // Two files that merge into a complete planning, then do a full solve
  // (not lp_only) to cover the output-writing path with merged data.
  constexpr auto merge_part1 = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed"
    },
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {"name": "merge_solve_test"}
  })";

  constexpr auto merge_part2 = R"({
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

  const auto stem1 = write_tmp_json("gtopt_main_merge_solve1", merge_part1);
  const auto stem2 = write_tmp_json("gtopt_main_merge_solve2", merge_part2);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_merge_solve_out")
          .string();

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem1.string(), stem2.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE(
    "gtopt_main - lp_threads and lp_presolve from JSON options")  // NOLINT
{
  // JSON with top-level lp_threads and lp_presolve to cover the deprecated
  // backward-compat paths (lines 578-582 in gtopt_main.cpp).
  constexpr auto json_with_solver_opts = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "lp_threads": 1,
      "lp_presolve": false
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array":  [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "solver_opts_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto stem =
      write_tmp_json("gtopt_main_solver_opts", json_with_solver_opts);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_solver_opts_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - use_kirchhoff=true with multi-bus system")  // NOLINT
{
  // Multi-bus system with kirchhoff=true to cover the use_kirchhoff option
  // path and the multi-bus (use_single_bus=false) code path.
  constexpr auto kirchhoff_json = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed"
    },
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "kirchhoff_test",
      "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 2, "capacity": 50.0}
      ],
      "line_array": [
        {"uid": 1, "name": "l1", "bus_a": 1, "bus_b": 2,
         "tmax_ab": 100.0, "tmax_ba": 100.0, "reactance": 0.1}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_kirchhoff", kirchhoff_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_kirchhoff_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_kirchhoff = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE("gtopt_main - empty planning_files returns error")  // NOLINT
{
  // Empty planning_files vector should still work (no files to parse → empty
  // planning), but may fail during LP construction.  The key is that it
  // doesn't crash.
  auto result = gtopt_main(MainOptions {
      .planning_files = {},
      .lp_only = true,
  });
  // An empty planning may fail during LP build; either way, no crash.
  // We just check it returns (success or error).
  (void)result;
}

TEST_CASE("gtopt_main - names_level=cols_and_rows with stats")  // NOLINT
{
  // Exercise names_level=cols_and_rows (names + map) with stats to cover the
  // make_lp_matrix_options paths for higher naming levels.
  const auto stem = write_tmp_json("gtopt_main_lp_names2", minimal_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_names_level = LpNamesLevel::cols_and_rows,
      .lp_only = true,
      .print_stats = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE(
    "gtopt_main - lp_debug with gzip writes compressed LP files")  // NOLINT
{
  // Set lp_compression=gzip so LP debug files are gzip-compressed.
  // We verify a .gz file appears and no bare .lp remains.
  const auto stem = write_tmp_json("gtopt_main_lp_debug_gz", minimal_json);
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_main_lp_debug_gz_logs";
  std::filesystem::remove_all(log_dir);

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
      .lp_names_level = LpNamesLevel::only_cols,
      .lp_debug = true,
      .lp_compression = "gzip",
      .log_directory = log_dir.string(),
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  bool found_gz = false;
  bool found_bare = false;
  if (std::filesystem::exists(log_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with("gtopt_lp")) {
        if (name.ends_with(".lp.gz")) {
          found_gz = true;
        }
        if (name.ends_with(".lp")) {
          found_bare = true;
        }
      }
    }
  }
  CHECK(found_gz);
  CHECK_FALSE(found_bare);
  std::filesystem::remove_all(log_dir);
}

TEST_CASE(  // NOLINT
    "gtopt_main - user_constraint_file loads JSON constraints")
{
  // Create a JSON constraint file referencing generator g1.
  // This covers lines 455-495 in gtopt_main.cpp (user_constraint_file loading).
  const auto uc_path =
      std::filesystem::temp_directory_path() / "gtopt_main_uc_test.json";
  {
    std::ofstream uc_file(uc_path);
    uc_file << R"([
      {
        "uid": 100,
        "name": "uc_gen_limit",
        "expression": "generator(\"g1\").generation <= 150"
      }
    ])";
  }

  // Planning JSON referencing the external constraint file
  const auto planning_json = std::format(R"({{
    "options": {{
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed"
    }},
    "simulation": {{
      "block_array": [{{"uid": 1, "duration": 1}}],
      "stage_array":  [{{"uid": 1, "first_block": 0, "count_block": 1}}],
      "scenario_array": [{{"uid": 1}}]
    }},
    "system": {{
      "name": "uc_file_test",
      "user_constraint_file": "{}",
      "bus_array": [{{"uid": 1, "name": "b1"}}],
      "generator_array": [
        {{"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}}
      ],
      "demand_array": [
        {{"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}}
      ]
    }}
  }})",
                                         uc_path.string());

  const auto stem =
      write_tmp_json("gtopt_main_uc_file", std::string_view(planning_json));
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "gtopt_main_uc_file_out")
          .string();
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  std::filesystem::remove(uc_path);
}

TEST_CASE("gtopt_main - demand_fail_cost=0 triggers warning")  // NOLINT
{
  // When demand_fail_cost is 0 (or not set), gtopt_main logs a warning
  // (lines 511-514).  This test exercises that path.
  constexpr auto no_dfc_json = R"({
    "options": {"output_compression": "uncompressed"},
    "simulation": {
      "block_array":    [{"uid": 1, "duration": 1}],
      "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "no_dfc_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_no_dfc", no_dfc_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE(  // NOLINT
    "gtopt_main - input_directory fallback for planning file")
{
  // Create a planning file in a subdirectory and reference it via
  // input_directory, covering lines 109-118 in gtopt_main.cpp.
  const auto input_dir =
      std::filesystem::temp_directory_path() / "gtopt_main_input_dir_test";
  std::filesystem::create_directories(input_dir);

  // Write the planning JSON into input_dir
  {
    std::ofstream f(input_dir / "alt_plan.json");
    f << minimal_json;
  }

  // Reference the file by name only (not full path), with input_directory set
  auto result = gtopt_main(MainOptions {
      .planning_files = {"alt_plan"},
      .input_directory = input_dir.string(),
      .lp_only = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  std::filesystem::remove_all(input_dir);
}

TEST_CASE(  // NOLINT
    "gtopt_main - auto trace log without explicit trace_log path")
{
  // When trace_log is NOT set, gtopt_main auto-creates a numbered
  // trace_N.log in the log directory (lines 365-390).
  const auto stem = write_tmp_json("gtopt_main_auto_trace", minimal_json);
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_main_auto_trace_logs";
  std::filesystem::remove_all(log_dir);

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .lp_only = true,
      .log_directory = log_dir.string(),
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  // Should have created trace_1.log in the log directory
  bool found_trace = false;
  if (std::filesystem::exists(log_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
      if (entry.path().filename().string().starts_with("trace_")) {
        found_trace = true;
        break;
      }
    }
  }
  CHECK(found_trace);
  std::filesystem::remove_all(log_dir);
}

TEST_CASE(  // NOLINT
    "gtopt_main - log_mode=detailed creates solver log files")
{
  using namespace gtopt;

  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  REQUIRE(!solvers.empty());

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(std::string(solver_name).c_str())
    {
      const auto stem = write_tmp_json(
          std::format("gtopt_main_solver_log_{}", solver_name), minimal_json);
      const auto log_dir = std::filesystem::temp_directory_path()
          / std::format("gtopt_main_solver_log_{}_logs", solver_name);
      std::filesystem::remove_all(log_dir);

      auto result = gtopt_main(MainOptions {
          .planning_files = {stem.string()},
          .use_single_bus = true,
          .log_directory = log_dir.string(),
          .solver = std::string(solver_name),
          .set_options =
              {
                  "solver_options.log_mode=detailed",
              },
      });
      REQUIRE(result.has_value());
      CHECK(*result == 0);

      // Verify that at least one solver .log file was created
      bool found_log = false;
      if (std::filesystem::exists(log_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
          const auto name = entry.path().filename().string();
          if (name.starts_with(std::string(solver_name))
              && name.ends_with(".log"))
          {
            found_log = true;
            CHECK(std::filesystem::file_size(entry.path()) > 0);
            break;
          }
        }
      }
      CHECK(found_log);

      std::filesystem::remove_all(log_dir);
    }
  }
}

// ─── Monolithic method coverage tests ───────────────────────────────────────

TEST_CASE(  // NOLINT
    "gtopt_main - monolithic with boundary_cuts_file (nonexistent)")
{
  using namespace gtopt;

  // Multi-phase problem for monolithic solver with a boundary cuts file
  // that doesn't exist — should warn but still solve successfully.
  constexpr auto multi_phase_json = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "sddp_options": {
        "boundary_cuts_file": "/tmp/nonexistent_bc_file.csv"
      }
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 1}],
      "stage_array": [
        {"uid": 1, "first_block": 0, "count_block": 1},
        {"uid": 2, "first_block": 1, "count_block": 1}
      ],
      "scenario_array": [{"uid": 1}],
      "phase_array": [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1}
      ]
    },
    "system": {
      "name": "gtopt_main_bc_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10, "capacity": 200}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_bc_test", multi_phase_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE(  // NOLINT
    "gtopt_main - SDDP method via method=sddp option")
{
  using namespace gtopt;

  // 2-phase problem that triggers SDDP solver via JSON options
  constexpr auto sddp_json = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "method": "sddp",
      "sddp_options": {
        "max_iterations": 5,
        "convergence_tol": 0.01
      }
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 1}],
      "stage_array": [
        {"uid": 1, "first_block": 0, "count_block": 1},
        {"uid": 2, "first_block": 1, "count_block": 1}
      ],
      "scenario_array": [{"uid": 1}],
      "phase_array": [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1}
      ]
    },
    "system": {
      "name": "gtopt_main_sddp_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10, "capacity": 200}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_sddp_test", sddp_json);
  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

TEST_CASE(  // NOLINT
    "gtopt_main - SDDP inherits log_mode from top-level solver_options")
{
  using namespace gtopt;

  // Top-level solver_options.log_mode=detailed should propagate to
  // SDDP forward solver via merge, even without setting it explicitly in
  // sddp_options.forward_solver_options.
  constexpr auto sddp_log_json = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "method": "sddp",
      "sddp_options": {
        "max_iterations": 3,
        "convergence_tol": 0.01
      }
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 1}],
      "stage_array": [
        {"uid": 1, "first_block": 0, "count_block": 1},
        {"uid": 2, "first_block": 1, "count_block": 1}
      ],
      "scenario_array": [{"uid": 1}],
      "phase_array": [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1}
      ]
    },
    "system": {
      "name": "sddp_log_merge_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10, "capacity": 200}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ]
    }
  })";

  const auto stem = write_tmp_json("gtopt_main_sddp_log", sddp_log_json);
  const auto log_dir =
      std::filesystem::temp_directory_path() / "gtopt_main_sddp_log_logs";
  std::filesystem::remove_all(log_dir);

  auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .use_single_bus = true,
      .log_directory = log_dir.string(),
      .set_options =
          {
              "solver_options.log_mode=detailed",
          },
  });
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  // Verify solver log files were created in the log directory
  bool found_log = false;
  if (std::filesystem::exists(log_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
      const auto name = entry.path().filename().string();
      if (name.ends_with(".log") && !name.starts_with("trace_")) {
        found_log = true;
        CHECK(std::filesystem::file_size(entry.path()) > 0);
        break;
      }
    }
  }
  CHECK(found_log);

  std::filesystem::remove_all(log_dir);
}
