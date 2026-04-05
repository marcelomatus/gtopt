/**
 * @file      test_monolithic_options.hpp
 * @brief     Unit tests for MonolithicOptions struct
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/monolithic_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("MonolithicOptions - Default construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const MonolithicOptions opts {};

  CHECK_FALSE(opts.solve_mode.has_value());
  CHECK_FALSE(opts.boundary_cuts_file.has_value());
  CHECK_FALSE(opts.boundary_cuts_mode.has_value());
  CHECK_FALSE(opts.boundary_max_iterations.has_value());
  CHECK_FALSE(opts.solver_options.has_value());
}

TEST_CASE("MonolithicOptions - Construction with all fields")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const MonolithicOptions opts {
      .solve_mode = SolveMode::sequential,
      .boundary_cuts_file = "boundary.csv",
      .boundary_cuts_mode = BoundaryCutsMode::combined,
      .boundary_max_iterations = 10,
      .solver_options =
          SolverOptions {
              .algorithm = LPAlgo::primal,
              .threads = 4,
              .time_limit = 3600.0,
          },
  };

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

TEST_CASE("MonolithicOptions - Merge fills missing fields")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  MonolithicOptions base {
      .solve_mode = SolveMode::monolithic,
  };

  MonolithicOptions overlay {
      .boundary_cuts_file = "cuts.csv",
      .boundary_cuts_mode = BoundaryCutsMode::separated,
      .boundary_max_iterations = 5,
  };

  base.merge(std::move(overlay));

  REQUIRE(base.solve_mode.has_value());
  CHECK(*base.solve_mode == SolveMode::monolithic);
  REQUIRE(base.boundary_cuts_file.has_value());
  CHECK(*base.boundary_cuts_file == "cuts.csv");
  REQUIRE(base.boundary_cuts_mode.has_value());
  CHECK(*base.boundary_cuts_mode == BoundaryCutsMode::separated);
  REQUIRE(base.boundary_max_iterations.has_value());
  CHECK(*base.boundary_max_iterations == 5);
}

TEST_CASE("MonolithicOptions - Merge overwrites existing (overlay wins)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  MonolithicOptions base {
      .solve_mode = SolveMode::sequential,
      .boundary_cuts_mode = BoundaryCutsMode::combined,
  };

  MonolithicOptions overlay {
      .solve_mode = SolveMode::monolithic,
      .boundary_cuts_mode = BoundaryCutsMode::noload,
  };

  base.merge(std::move(overlay));

  // Overlay wins: set fields overwrite base
  REQUIRE(base.solve_mode.has_value());
  CHECK(*base.solve_mode == SolveMode::monolithic);
  REQUIRE(base.boundary_cuts_mode.has_value());
  CHECK(*base.boundary_cuts_mode == BoundaryCutsMode::noload);
}

TEST_CASE("MonolithicOptions - Merge nested solver_options")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("both set: inner merge")
  {
    MonolithicOptions base {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::barrier,
                .optimal_eps = 1e-8,
            },
    };

    MonolithicOptions overlay {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
                .feasible_eps = 1e-7,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.solver_options.has_value());
    CHECK(base.solver_options->algorithm == LPAlgo::barrier);
    REQUIRE(base.solver_options->optimal_eps.has_value());
    CHECK(base.solver_options->optimal_eps.value_or(0.0)
          == doctest::Approx(1e-8));
    REQUIRE(base.solver_options->feasible_eps.has_value());
    CHECK(base.solver_options->feasible_eps.value_or(0.0)
          == doctest::Approx(1e-7));
  }

  SUBCASE("base empty: takes overlay")
  {
    MonolithicOptions base {};

    MonolithicOptions overlay {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.solver_options.has_value());
    CHECK(base.solver_options->algorithm == LPAlgo::dual);
  }
}
