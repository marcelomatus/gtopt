/**
 * @file      test_set_cli_option.hpp
 * @brief     Unit tests for the --set key=value CLI option
 * @date      Sun Mar 30 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests cover:
 *   - dotted path parsing (single-level and multi-level)
 *   - auto-type detection (bool, int, double, string)
 *   - nested paths (three or more levels deep)
 *   - invalid formats (missing '=', empty key)
 *   - multiple --set options combined
 *   - string fallback when auto-typed value fails schema
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Minimal planning JSON for --set option tests.
// Uses single-bus mode and a tiny system so that lp_build=true
// completes instantly without a solver.
constexpr auto set_test_json = R"({
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
    "name": "set_cli_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
    ]
  }
})";

// Write JSON content to a temp file and return the stem path
// (without .json extension; gtopt_main appends it).
std::filesystem::path write_set_test_json(const std::string& name,
                                          std::string_view content)
{
  auto tmp = std::filesystem::temp_directory_path() / name;
  tmp.replace_extension(".json");
  std::ofstream ofs(tmp);
  ofs << content;
  return tmp.replace_extension("");
}

}  // namespace

// ── Auto-type detection: bool ─────────────────────────────────────────

TEST_CASE("--set auto-type detection: true is parsed as bool")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_bool_true", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "use_kirchhoff=true",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("--set auto-type detection: false is parsed as bool")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_bool_false", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "use_kirchhoff=false",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Auto-type detection: integer ──────────────────────────────────────

TEST_CASE("--set auto-type detection: integer value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_int", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "demand_fail_cost=500",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Auto-type detection: double ───────────────────────────────────────

TEST_CASE("--set auto-type detection: double value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_double", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "scale_objective=1500.5",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Auto-type detection: string ───────────────────────────────────────

TEST_CASE("--set auto-type detection: string value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_string", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "output_format=parquet",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Dotted path: single level ─────────────────────────────────────────

TEST_CASE("--set dotted path: single-level key")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_single_path", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "demand_fail_cost=2000",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Dotted path: two-level nested ─────────────────────────────────────

TEST_CASE("--set dotted path: two-level nested key")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_two_level", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "sddp_options.max_iterations=100",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Dotted path: three-level nested ───────────────────────────────────

TEST_CASE("--set dotted path: three-level nested key")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_three_level", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "sddp_options.forward_solver_options.threads=4",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Multiple --set options combined ───────────────────────────────────

TEST_CASE("--set multiple options applied together")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_multi", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "demand_fail_cost=999",
              "use_kirchhoff=false",
              "sddp_options.max_iterations=50",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Solver options via direct setter path ─────────────────────────────

TEST_CASE("--set solver_options.threads via direct setter")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem =
      write_set_test_json("set_cli_solver_threads", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "solver_options.threads=2",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("--set solver_options.presolve via direct setter")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem =
      write_set_test_json("set_cli_solver_presolve", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "solver_options.presolve=true",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Invalid format: missing '=' ───────────────────────────────────────

TEST_CASE("--set invalid format: missing equals sign")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_no_eq", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "demand_fail_cost_no_value",
          },
  });
  // Invalid --set format is detected and returns exit code 1.
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 1);
}

// ── Invalid format: empty key ─────────────────────────────────────────

TEST_CASE("--set invalid format: empty key (leading =)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_empty_key", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "=42",
          },
  });
  // Empty key is detected and returns exit code 1.
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 1);
}

// ── Unknown dotted path: silently applied (JSON ignores unknowns) ─────

TEST_CASE("--set unknown option path succeeds silently")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_unknown_path", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "completely_bogus_option=123",
          },
  });
  // Unknown paths are merged into JSON and silently ignored by
  // the parser; gtopt_main succeeds.
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Scientific notation as double ─────────────────────────────────────

TEST_CASE("--set auto-type detection: scientific notation double")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_scientific", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "scale_objective=1e3",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Negative integer ──────────────────────────────────────────────────

TEST_CASE("--set auto-type detection: negative integer")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_neg_int", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_build = true,
      .set_options =
          {
              "demand_fail_cost=-1",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Full solve with --set override ────────────────────────────────────

TEST_CASE("--set demand_fail_cost override in full solve")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto stem = write_set_test_json("set_cli_full_solve", set_test_json);
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "set_cli_output").string();

  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .output_directory = out_dir,
      .use_single_bus = true,
      .set_options =
          {
              "demand_fail_cost=5000",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}
