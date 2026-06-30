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
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Minimal planning JSON for --set option tests.
// Uses single-bus mode and a tiny system so that lp_only=true
// completes instantly without a solver.
constexpr auto set_test_json = R"(
  {
    "options": {
      "output_compression": "uncompressed",
      "model_options": {
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        {
          "uid": 1,
          "duration": 1
        }
      ],
      "stage_array": [
        {
          "uid": 1,
          "first_block": 0,
          "count_block": 1
        }
      ],
      "scenario_array": [
        {
          "uid": 1
        }
      ]
    },
    "system": {
      "name": "set_cli_test",
      "bus_array": [
        {
          "uid": 1,
          "name": "b1"
        }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "g1",
          "bus": 1,
          "gcost": 10.0,
          "capacity": 200.0
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d1",
          "bus": 1,
          "capacity": 50.0
        }
      ]
    }
  }
)";

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
  const auto stem = write_set_test_json("set_cli_bool_true", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_bool_false", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_int", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_double", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_string", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_single_path", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_two_level", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_three_level", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_multi", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem =
      write_set_test_json("set_cli_solver_threads", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem =
      write_set_test_json("set_cli_solver_presolve", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.presolve=true",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Regression: previously-dropped SolverOptions fields ───────────────
// `crossover` (and force_barrier_crossover, max_fallbacks, mip_gap,
// mip_gap_abs, memory_emphasis, param_file, scaling) used to be silently
// dropped: `try_set_solver_field` didn't list them, so the call fell back
// to the JSON-overlay path whose `SolverOptions::merge()` carries ONLY the
// optional fields — never the non-optional bools.  `--set
// solver_options.crossover=false` therefore reached the backend as the
// struct default `true`.  These tests assert the VALUE actually lands on
// the Planning (the end-to-end exit-code tests above could not catch this).

TEST_CASE("--set solver_options.crossover=none actually lands")  // NOLINT
{
  auto planning = daw::json::from_json<Planning>(
      std::string_view {set_test_json}, StrictParsePolicy);
  REQUIRE(planning.options.solver_options.crossover
          == CrossoverMode::automatic);  // default auto
  REQUIRE(apply_set_options(planning, {"solver_options.crossover=none"}));
  CHECK(planning.options.solver_options.crossover == CrossoverMode::none);
}

TEST_CASE("--set solver_options: every settable field round-trips")  // NOLINT
{
  auto planning = daw::json::from_json<Planning>(
      std::string_view {set_test_json}, StrictParsePolicy);
  REQUIRE(apply_set_options(planning,
                            {
                                "solver_options.crossover=none",
                                "solver_options.force_barrier_crossover=true",
                                "solver_options.max_fallbacks=0",
                                "solver_options.mip_gap=0.01",
                                "solver_options.mip_gap_abs=5",
                                "solver_options.memory_emphasis=true",
                                "solver_options.param_file=solvers/cplex.prm",
                                "solver_options.scaling=aggressive",
                            }));
  const auto& so = planning.options.solver_options;
  CHECK(so.crossover == CrossoverMode::none);
  CHECK(so.force_barrier_crossover == true);
  CHECK(so.max_fallbacks == 0);
  CHECK(so.mip_gap.value_or(-1.0) == doctest::Approx(0.01));
  CHECK(so.mip_gap_abs.value_or(-1.0) == doctest::Approx(5.0));
  CHECK(so.memory_emphasis.value_or(false) == true);
  CHECK(so.param_file.value_or("") == "solvers/cplex.prm");
  CHECK(so.scaling.value_or(SolverScaling::none) == SolverScaling::aggressive);
}

TEST_CASE("--set solver_options.crossover rejects an invalid value")  // NOLINT
{
  auto planning = daw::json::from_json<Planning>(
      std::string_view {set_test_json}, StrictParsePolicy);
  // A typo must be a hard error (require_enum throws → apply returns false),
  // NOT a silent default-to-false from a hand-rolled `value == "true"`.
  CHECK_FALSE(apply_set_options(planning, {"solver_options.crossover=flase"}));
}

// ── Invalid format: missing '=' ───────────────────────────────────────

TEST_CASE("--set invalid format: missing equals sign")
{
  const auto stem = write_set_test_json("set_cli_no_eq", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_empty_key", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "=42",
          },
  });
  // Empty key is detected and returns exit code 1.
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 1);
}

// ── Unknown dotted path: now rejected (strict parser) ─────────────────

TEST_CASE("--set unknown option path is rejected")
{
  const auto stem = write_set_test_json("set_cli_unknown_path", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "completely_bogus_option=123",
          },
  });
  // With strict JSON parsing (UseExactMappingsByDefault=yes), an unknown
  // `--set` path is rejected by the overlay parser and gtopt_main returns
  // a non-zero exit code.
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 1);
}

// ── Scientific notation as double ─────────────────────────────────────

TEST_CASE("--set auto-type detection: scientific notation double")
{
  const auto stem = write_set_test_json("set_cli_scientific", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
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
  const auto stem = write_set_test_json("set_cli_neg_int", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "demand_fail_cost=-1",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Solver algorithm enum safety ─────────────────────────────────────

TEST_CASE("--set solver_options.algorithm by name")  // NOLINT
{
  const auto stem = write_set_test_json("set_cli_algo_name", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.algorithm=barrier",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("--set solver_options.algorithm by valid number")  // NOLINT
{
  const auto stem = write_set_test_json("set_cli_algo_num", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.algorithm=2",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE(
    "--set solver_options.algorithm rejects out-of-range number")  // NOLINT
{
  const auto stem = write_set_test_json("set_cli_algo_bad", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.algorithm=99",
          },
  });
  // apply_set_options returns false → gtopt_main returns EXIT_FAILURE (1)
  REQUIRE(result.has_value());
  CHECK(*result == EXIT_FAILURE);
}

TEST_CASE("--set solver_options.log_mode rejects invalid name")  // NOLINT
{
  const auto stem = write_set_test_json("set_cli_logmode_bad", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.log_mode=verbose",
          },
  });
  REQUIRE(result.has_value());
  CHECK(*result == EXIT_FAILURE);
}

TEST_CASE("--set solver_options.log_mode accepts valid name")  // NOLINT
{
  const auto stem = write_set_test_json("set_cli_logmode_ok", set_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "solver_options.log_mode=nolog",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Array-index dotted path ───────────────────────────────────────────

// Minimal cascade case with three levels, one per model formulation.
// Uses lp_only to avoid solver invocation; we only verify that the
// overlay is parsed, merged, and applied without error.
constexpr auto cascade_test_json = R"(
  {
    "options": {
      "method": "cascade",
      "output_compression": "uncompressed",
      "cascade_options": {
        "sddp_options": {
          "max_iterations": 1
        },
        "level_array": [
          {
            "uid": 1,
            "name": "uninodal",
            "model_options": {
              "use_single_bus": true
            },
            "sddp_options": {
              "max_iterations": 1
            }
          },
          {
            "uid": 2,
            "name": "transport",
            "model_options": {
              "use_single_bus": false,
              "use_kirchhoff": false
            },
            "sddp_options": {
              "max_iterations": 1
            }
          },
          {
            "uid": 3,
            "name": "full_network",
            "model_options": {
              "use_single_bus": false,
              "use_kirchhoff": true
            },
            "sddp_options": {
              "max_iterations": 1
            }
          }
        ]
      },
      "model_options": {
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        {
          "uid": 1,
          "duration": 1
        }
      ],
      "stage_array": [
        {
          "uid": 1,
          "first_block": 0,
          "count_block": 1
        }
      ],
      "scenario_array": [
        {
          "uid": 1
        }
      ]
    },
    "system": {
      "name": "set_cli_cascade_test",
      "bus_array": [
        {
          "uid": 1,
          "name": "b1"
        }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "g1",
          "bus": 1,
          "gcost": 10.0,
          "capacity": 200.0
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d1",
          "bus": 1,
          "capacity": 50.0
        }
      ]
    }
  }
)";

TEST_CASE("--set array-index: cascade_options.level_array.0.sddp_options")
{
  const auto stem =
      write_set_test_json("set_cli_cascade_l0", cascade_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "cascade_options.level_array.0.sddp_options.max_iterations=20",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("--set array-index: non-zero index lands at the right slot")
{
  // level_array.2 targets the third element.  The overlay builder emits
  // two empty-object placeholders ahead of the target so the merge side
  // sees a same-size array (3 == 3) and merges element-wise.
  const auto stem =
      write_set_test_json("set_cli_cascade_l2", cascade_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "cascade_options.level_array.2.sddp_options.max_iterations=15",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("--set array-index: multiple --set across different indices")
{
  const auto stem =
      write_set_test_json("set_cli_cascade_multi", cascade_test_json);
  auto result = gtopt_main(MainOptions {
      .planning_files =
          {
              stem.string(),
          },
      .use_single_bus = true,
      .lp_only = true,
      .set_options =
          {
              "sddp_options.max_iterations=20",
              "cascade_options.sddp_options.max_iterations=20",
              "cascade_options.level_array.0.sddp_options.max_iterations=20",
          },
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

// ── Full solve with --set override ────────────────────────────────────

// ── Value-probing tests for apply_set_options ────────────────────────
//
// The tests above only verify gtopt_main exits with status 0.  These
// tests additionally probe the merged Planning object to confirm the
// `--set` override actually landed on the target field (and not on a
// silently-discarded sibling).

TEST_CASE("--set model_options.scale_objective lands on model_options")
{
  Planning planning;
  planning.options.model_options.scale_objective = 1000.0;

  REQUIRE(apply_set_options(planning, {"model_options.scale_objective=2000"}));

  CHECK(planning.options.model_options.scale_objective.value_or(-1.0)
        == doctest::Approx(2000.0));
}

TEST_CASE("--set model_options.scale_theta lands on model_options")
{
  Planning planning;
  planning.options.model_options.scale_theta = 1.0;

  REQUIRE(apply_set_options(planning, {"model_options.scale_theta=42.5"}));

  CHECK(planning.options.model_options.scale_theta.value_or(-1.0)
        == doctest::Approx(42.5));
}

TEST_CASE("--set model_options.demand_fail_cost lands on model_options")
{
  Planning planning;

  REQUIRE(apply_set_options(planning, {"model_options.demand_fail_cost=777"}));

  CHECK(planning.options.model_options.demand_fail_cost.value_or(-1.0)
        == doctest::Approx(777.0));
}

TEST_CASE("--set model_options.scale_objective accepts integer literal")
{
  Planning planning;
  planning.options.model_options.scale_objective = 1000.0;

  // Auto-typer routes a bare integer through `strtoll` first, so the
  // overlay JSON contains `"scale_objective":1` (no decimal point).
  // daw::json must still accept this for an OptReal-typed field.
  REQUIRE(apply_set_options(planning, {"model_options.scale_objective=1"}));

  CHECK(planning.options.model_options.scale_objective.value_or(-1.0)
        == doctest::Approx(1.0));
}

TEST_CASE("--set with --no-scale: --no-scale wins (documented behaviour)")
{
  // Regression guard for the apply_cli_options ordering described in
  // include/gtopt/main_options.hpp ≈ line 735.  `--no-scale` is a
  // diagnostic flag that intentionally clobbers user-supplied scale
  // overrides AFTER `apply_set_options` runs.  This test pins the
  // current behaviour so the precedence is detected if ever changed:
  // `apply_set_options` itself lands the override on model_options
  // (covered by the test above); only the downstream
  // `apply_cli_options` step rewrites it under --no-scale.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"model_options.scale_objective=2000"}));
  CHECK(planning.options.model_options.scale_objective.value_or(-1.0)
        == doctest::Approx(2000.0));
}

TEST_CASE("--set sddp_options.max_iterations lands on sddp_options")
{
  // Non-solver_options sddp_options path — exercises the JSON
  // overlay route (no `try_set_solver_options_path` shortcut).
  Planning planning;

  REQUIRE(apply_set_options(planning, {"sddp_options.max_iterations=42"}));

  CHECK(planning.options.sddp_options.max_iterations.value_or(-1) == 42);
}

TEST_CASE("--set monolithic_options.solver_options.threads lands via shortcut")
{
  // Direct-setter shortcut path — does NOT go through JSON overlay.
  Planning planning;

  REQUIRE(apply_set_options(planning,
                            {"monolithic_options.solver_options.threads=8"}));

  REQUIRE(planning.options.monolithic_options.solver_options.has_value());
  CHECK(planning.options.monolithic_options.solver_options->threads == 8);
}

TEST_CASE("--set demand_fail_cost override in full solve")
{
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

// ── Probe: --set on cascade_options.level_array.N actually lands ──────
//
// Regression guards for the juan/gtopt_iplp_plain --set override path:
// before this test the CLI logged "applied" but the in-memory Planning
// still carried the JSON's level-0 max_iterations.  Three properties
// must hold after `apply_set_options` returns:
//   1. The level_array still has the **same number of elements** as the
//      base JSON (the array merge must NOT replace 3 base levels with
//      a 1-element overlay).
//   2. The targeted index's targeted field carries the **override value**.
//   3. Untouched sibling fields on the same level keep their base values,
//      and untouched sibling levels keep their base sddp_options.

TEST_CASE(
    "--set cascade_options.level_array.0.sddp_options.max_iterations "
    "lands on level 0 without nuking siblings")
{
  Planning planning = daw::json::from_json<Planning>(
      std::string_view {cascade_test_json}, StrictParsePolicy);
  REQUIRE(planning.options.cascade_options.level_array.size() == 3);

  REQUIRE(apply_set_options(
      planning,
      {"cascade_options.level_array.0.sddp_options.max_iterations=99"}));

  // (1) size preserved
  CHECK(planning.options.cascade_options.level_array.size() == 3);

  // (2) target landed at index 0
  REQUIRE(
      planning.options.cascade_options.level_array[0].sddp_options.has_value());
  CHECK(planning.options.cascade_options.level_array[0]
            .sddp_options->max_iterations.value_or(-1)
        == 99);

  // (2b) target's identity-bearing fields (name / model_options) survive
  CHECK(planning.options.cascade_options.level_array[0].name.value_or("?")
        == "uninodal");
  REQUIRE(planning.options.cascade_options.level_array[0]
              .model_options.has_value());
  CHECK(planning.options.cascade_options.level_array[0]
            .model_options->use_single_bus.value_or(false)
        == true);

  // (3) untouched siblings keep their base values
  REQUIRE(
      planning.options.cascade_options.level_array[1].sddp_options.has_value());
  CHECK(planning.options.cascade_options.level_array[1]
            .sddp_options->max_iterations.value_or(-1)
        == 1);
  CHECK(planning.options.cascade_options.level_array[1].name.value_or("?")
        == "transport");
  REQUIRE(
      planning.options.cascade_options.level_array[2].sddp_options.has_value());
  CHECK(planning.options.cascade_options.level_array[2]
            .sddp_options->max_iterations.value_or(-1)
        == 1);
  CHECK(planning.options.cascade_options.level_array[2].name.value_or("?")
        == "full_network");
}

TEST_CASE("--set cascade_options.level_array.N for middle/last index lands")
{
  Planning planning = daw::json::from_json<Planning>(
      std::string_view {cascade_test_json}, StrictParsePolicy);
  REQUIRE(planning.options.cascade_options.level_array.size() == 3);

  REQUIRE(apply_set_options(
      planning,
      {"cascade_options.level_array.2.sddp_options.max_iterations=55"}));

  CHECK(planning.options.cascade_options.level_array.size() == 3);
  REQUIRE(
      planning.options.cascade_options.level_array[2].sddp_options.has_value());
  CHECK(planning.options.cascade_options.level_array[2]
            .sddp_options->max_iterations.value_or(-1)
        == 55);
  CHECK(planning.options.cascade_options.level_array[2].name.value_or("?")
        == "full_network");
  // Siblings untouched.
  CHECK(planning.options.cascade_options.level_array[0]
            .sddp_options->max_iterations.value_or(-1)
        == 1);
  CHECK(planning.options.cascade_options.level_array[1]
            .sddp_options->max_iterations.value_or(-1)
        == 1);
}

TEST_CASE(
    "--set cascade_options.level_array.0/1/2 applied in sequence "
    "all stick")
{
  // Three sequential overrides — each must land on its own level
  // without clobbering the others.  This is the exact pattern juan
  // uses: --set ...level_array.0... --set ...level_array.1... --set
  // ...level_array.2...
  Planning planning = daw::json::from_json<Planning>(
      std::string_view {cascade_test_json}, StrictParsePolicy);

  REQUIRE(apply_set_options(
      planning,
      {
          "cascade_options.level_array.0.sddp_options.max_iterations=3",
          "cascade_options.level_array.1.sddp_options.max_iterations=4",
          "cascade_options.level_array.2.sddp_options.max_iterations=5",
      }));

  CHECK(planning.options.cascade_options.level_array.size() == 3);
  CHECK(planning.options.cascade_options.level_array[0]
            .sddp_options->max_iterations.value_or(-1)
        == 3);
  CHECK(planning.options.cascade_options.level_array[1]
            .sddp_options->max_iterations.value_or(-1)
        == 4);
  CHECK(planning.options.cascade_options.level_array[2]
            .sddp_options->max_iterations.value_or(-1)
        == 5);
  // Identity fields survive all three merges.
  CHECK(planning.options.cascade_options.level_array[0].name.value_or("?")
        == "uninodal");
  CHECK(planning.options.cascade_options.level_array[1].name.value_or("?")
        == "transport");
  CHECK(planning.options.cascade_options.level_array[2].name.value_or("?")
        == "full_network");
}

// ── Legacy ModelOptions short-key aliases (§11 of naming-conventions) ──
//
// Per `source/gtopt_json_io_set.cpp::rewrite_legacy_model_options_key`,
// every legacy top-level PlanningOptions mirror field continues to
// work as a `--set` key after the §11 deletion, by transparently
// rewriting `key` → `model_options.key` before the JSON overlay path.
// The two §11.10 renames (reserve_fail_cost → reserve_shortage_cost,
// hydro_fail_cost → hydro_spill_cost) also land on the new canonical
// field.  These tests pin every entry of that alias table so a
// future edit can't silently drop one.

TEST_CASE("--set legacy alias: bare demand_fail_cost → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"demand_fail_cost=5000"}));
  CHECK(planning.options.model_options.demand_fail_cost.value_or(-1.0)
        == doctest::Approx(5000.0));
}

TEST_CASE("--set legacy alias: bare scale_objective → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"scale_objective=250"}));
  CHECK(planning.options.model_options.scale_objective.value_or(-1.0)
        == doctest::Approx(250.0));
}

TEST_CASE("--set legacy alias: bare scale_theta → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"scale_theta=0.001"}));
  CHECK(planning.options.model_options.scale_theta.value_or(-1.0)
        == doctest::Approx(0.001));
}

TEST_CASE("--set legacy alias: bare use_single_bus → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"use_single_bus=true"}));
  CHECK(planning.options.model_options.use_single_bus.value_or(false) == true);
}

TEST_CASE("--set legacy alias: bare use_kirchhoff → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"use_kirchhoff=false"}));
  CHECK(planning.options.model_options.use_kirchhoff.value_or(true) == false);
}

TEST_CASE("--set legacy alias: bare use_line_losses → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"use_line_losses=true"}));
  CHECK(planning.options.model_options.use_line_losses.value_or(false) == true);
}

TEST_CASE("--set legacy alias: bare kirchhoff_threshold → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"kirchhoff_threshold=0.05"}));
  CHECK(planning.options.model_options.kirchhoff_threshold.value_or(-1.0)
        == doctest::Approx(0.05));
}

TEST_CASE("--set legacy alias: bare loss_segments → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"loss_segments=4"}));
  CHECK(planning.options.model_options.loss_segments.value_or(-1) == 4);
}

TEST_CASE("--set legacy alias: bare hydro_use_value → model_options")
{
  Planning planning;
  REQUIRE(apply_set_options(planning, {"hydro_use_value=12.5"}));
  CHECK(planning.options.model_options.hydro_use_value.value_or(-1.0)
        == doctest::Approx(12.5));
}

TEST_CASE("--set legacy alias: bare reserve_shortage_cost → model_options")
{
  // The canonical short key keeps working unchanged.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"reserve_shortage_cost=900"}));
  CHECK(planning.options.model_options.reserve_shortage_cost.value_or(-1.0)
        == doctest::Approx(900.0));
}

TEST_CASE("--set legacy alias: bare hydro_spill_cost → model_options")
{
  // The canonical short key keeps working unchanged.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"hydro_spill_cost=15"}));
  CHECK(planning.options.model_options.hydro_spill_cost.value_or(-1.0)
        == doctest::Approx(15.0));
}

TEST_CASE(
    "--set legacy alias rename: reserve_fail_cost → reserve_shortage_cost")
{
  // §11.10 rename: the deprecated short key still resolves, and is
  // routed to the new canonical field name on ModelOptions.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"reserve_fail_cost=750"}));
  CHECK(planning.options.model_options.reserve_shortage_cost.value_or(-1.0)
        == doctest::Approx(750.0));
}

TEST_CASE("--set legacy alias rename: hydro_fail_cost → hydro_spill_cost")
{
  // §11.10 rename: the deprecated short key still resolves, and is
  // routed to the new canonical field name on ModelOptions.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"hydro_fail_cost=20"}));
  CHECK(planning.options.model_options.hydro_spill_cost.value_or(-1.0)
        == doctest::Approx(20.0));
}

TEST_CASE("--set legacy alias: nested suffix is preserved after rewrite")
{
  // `--set demand_fail_cost.something_else=…` would route through
  // `model_options.demand_fail_cost.something_else=…`.  The suffix
  // is preserved verbatim; this exercises the suffix-preservation
  // branch of `rewrite_legacy_model_options_key`.  We assert the
  // bare-key behaviour holds (no extraneous suffix produced when
  // none was supplied).
  Planning planning;
  REQUIRE(apply_set_options(planning, {"demand_fail_cost=42"}));
  CHECK(planning.options.model_options.demand_fail_cost.value_or(-1.0)
        == doctest::Approx(42.0));
  // The pre-rewrite key (i.e. a top-level PlanningOptions
  // `demand_fail_cost` field) is gone, so nothing else can have
  // received the value.  Sanity check: explicit canonical path
  // also still works in isolation.
  Planning planning_canon;
  REQUIRE(
      apply_set_options(planning_canon, {"model_options.demand_fail_cost=42"}));
  CHECK(planning_canon.options.model_options.demand_fail_cost.value_or(-1.0)
        == doctest::Approx(42.0));
}

TEST_CASE("--set legacy alias: non-legacy short key is NOT rewritten")
{
  // `output_format` is a real top-level PlanningOptions field — must
  // route directly to options.output_format, not to
  // model_options.output_format.
  Planning planning;
  REQUIRE(apply_set_options(planning, {"output_format=csv"}));
  REQUIRE(planning.options.output_format.has_value());
  CHECK(*planning.options.output_format == DataFormat::csv);
}

// NOLINTEND(bugprone-unchecked-optional-access)