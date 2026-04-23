/**
 * @file      test_monolithic_options_json.hpp
 * @brief     JSON serialization tests for MonolithicOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_monolithic_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("MonolithicOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "solve_mode": "sequential",
    "boundary_cuts_file": "boundary.csv",
    "boundary_cuts_mode": "combined",
    "boundary_max_iterations": 10,
    "solver_options": {
      "algorithm": 1,
      "threads": 4,
      "presolve": true,
      "log_level": 0,
      "time_limit": 3600.0
    }
  })";

  const auto opts = daw::json::from_json<MonolithicOptions>(json_data);

  REQUIRE(opts.solve_mode.has_value());
  CHECK(*opts.solve_mode == SolveMode::sequential);
  REQUIRE(opts.boundary_cuts_file.has_value());
  CHECK(*opts.boundary_cuts_file == "boundary.csv");
  REQUIRE(opts.boundary_cuts_mode.has_value());
  CHECK(*opts.boundary_cuts_mode == BoundaryCutsMode::combined);
  REQUIRE(opts.boundary_max_iterations.has_value());
  CHECK(*opts.boundary_max_iterations == 10);
  REQUIRE(opts.solver_options.has_value());
  CHECK(opts.solver_options->algorithm == LPAlgo::primal);
  CHECK(opts.solver_options->threads == 4);
  REQUIRE(opts.solver_options->time_limit.has_value());
  CHECK(opts.solver_options->time_limit.value_or(0.0)
        == doctest::Approx(3600.0));
}

TEST_CASE("MonolithicOptions JSON - Missing fields stay nullopt")
{
  const std::string_view json_data = R"({
    "solve_mode": "monolithic"
  })";

  const auto opts = daw::json::from_json<MonolithicOptions>(json_data);

  REQUIRE(opts.solve_mode.has_value());
  CHECK(*opts.solve_mode == SolveMode::monolithic);
  CHECK_FALSE(opts.boundary_cuts_file.has_value());
  CHECK_FALSE(opts.boundary_cuts_mode.has_value());
  CHECK_FALSE(opts.boundary_max_iterations.has_value());
  // json_class_null creates default SolverOptions when key is absent
  if (opts.solver_options.has_value()) {
    CHECK(opts.solver_options->algorithm == LPAlgo::default_algo);
  }
}

TEST_CASE("MonolithicOptions JSON - Round-trip serialization")
{
  const MonolithicOptions original {
      .solve_mode = SolveMode::sequential,
      .boundary_cuts_file = "cuts.csv",
      .boundary_cuts_mode = BoundaryCutsMode::separated,
      .boundary_max_iterations = 5,
  };

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<MonolithicOptions>(json);

  CHECK(rt.solve_mode == original.solve_mode);
  CHECK(rt.boundary_cuts_file == original.boundary_cuts_file);
  CHECK(rt.boundary_cuts_mode == original.boundary_cuts_mode);
  CHECK(rt.boundary_max_iterations == original.boundary_max_iterations);
}

TEST_CASE("MonolithicOptions JSON - Empty object")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<MonolithicOptions>(json_data);

  CHECK_FALSE(opts.solve_mode.has_value());
  CHECK_FALSE(opts.boundary_cuts_file.has_value());
}
