/**
 * @file      test_sddp_options.hpp
 * @brief     Unit tests for SddpOptions struct
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/sddp_options.hpp>

TEST_CASE("SddpOptions - Default construction")
{
  const SddpOptions opts {};

  // All fields should be nullopt/empty
  CHECK_FALSE(opts.cut_sharing_mode.has_value());
  CHECK_FALSE(opts.cut_directory.has_value());
  CHECK_FALSE(opts.api_enabled.has_value());
  CHECK_FALSE(opts.update_lp_skip.has_value());
  CHECK_FALSE(opts.max_iterations.has_value());
  CHECK_FALSE(opts.min_iterations.has_value());
  CHECK_FALSE(opts.convergence_tol.has_value());
  CHECK_FALSE(opts.elastic_penalty.has_value());
  CHECK_FALSE(opts.alpha_min.has_value());
  CHECK_FALSE(opts.alpha_max.has_value());
  CHECK_FALSE(opts.cut_recovery_mode.has_value());
  CHECK_FALSE(opts.recovery_mode.has_value());
  CHECK_FALSE(opts.save_per_iteration.has_value());
  CHECK_FALSE(opts.cuts_input_file.has_value());
  CHECK_FALSE(opts.sentinel_file.has_value());
  CHECK_FALSE(opts.elastic_mode.has_value());
  CHECK_FALSE(opts.multi_cut_threshold.has_value());
  CHECK_FALSE(opts.apertures.has_value());
  CHECK_FALSE(opts.aperture_directory.has_value());
  CHECK_FALSE(opts.aperture_timeout.has_value());
  CHECK_FALSE(opts.save_aperture_lp.has_value());
  CHECK_FALSE(opts.warm_start.has_value());
  CHECK_FALSE(opts.boundary_cuts_file.has_value());
  CHECK_FALSE(opts.boundary_cuts_mode.has_value());
  CHECK_FALSE(opts.boundary_max_iterations.has_value());
  CHECK_FALSE(opts.named_cuts_file.has_value());
  CHECK_FALSE(opts.max_cuts_per_phase.has_value());
  CHECK_FALSE(opts.cut_prune_interval.has_value());
  CHECK_FALSE(opts.prune_dual_threshold.has_value());
  CHECK_FALSE(opts.single_cut_storage.has_value());
  CHECK_FALSE(opts.max_stored_cuts.has_value());
  CHECK_FALSE(opts.use_clone_pool.has_value());
  CHECK_FALSE(opts.simulation_mode.has_value());
  CHECK_FALSE(opts.stationary_tol.has_value());
  CHECK_FALSE(opts.stationary_window.has_value());
  CHECK_FALSE(opts.solver_options.has_value());
}

TEST_CASE("SddpOptions - Construction with iteration control fields")
{
  const SddpOptions opts {
      .max_iterations = 200,
      .min_iterations = 5,
      .convergence_tol = 1e-3,
  };

  REQUIRE(opts.max_iterations.has_value());
  CHECK(*opts.max_iterations == 200);
  REQUIRE(opts.min_iterations.has_value());
  CHECK(*opts.min_iterations == 5);
  REQUIRE(opts.convergence_tol.has_value());
  CHECK(*opts.convergence_tol == doctest::Approx(1e-3));
}

TEST_CASE("SddpOptions - Construction with cut management fields")
{
  const SddpOptions opts {
      .cut_sharing_mode = "expected",
      .cut_directory = "my_cuts",
      .cut_recovery_mode = "append",
      .recovery_mode = "cuts",
      .save_per_iteration = true,
      .cuts_input_file = "initial_cuts.csv",
      .elastic_mode = "multi_cut",
      .multi_cut_threshold = 20,
      .max_cuts_per_phase = 50,
      .cut_prune_interval = 5,
      .prune_dual_threshold = 1e-6,
      .single_cut_storage = true,
      .max_stored_cuts = 1000,
  };

  REQUIRE(opts.cut_sharing_mode.has_value());
  CHECK(*opts.cut_sharing_mode == "expected");
  REQUIRE(opts.cut_directory.has_value());
  CHECK(*opts.cut_directory == "my_cuts");
  REQUIRE(opts.cut_recovery_mode.has_value());
  CHECK(*opts.cut_recovery_mode == "append");
  REQUIRE(opts.recovery_mode.has_value());
  CHECK(*opts.recovery_mode == "cuts");
  REQUIRE(opts.save_per_iteration.has_value());
  CHECK(*opts.save_per_iteration == true);
  REQUIRE(opts.cuts_input_file.has_value());
  CHECK(*opts.cuts_input_file == "initial_cuts.csv");
  REQUIRE(opts.elastic_mode.has_value());
  CHECK(*opts.elastic_mode == "multi_cut");
  REQUIRE(opts.multi_cut_threshold.has_value());
  CHECK(*opts.multi_cut_threshold == 20);
  REQUIRE(opts.max_cuts_per_phase.has_value());
  CHECK(*opts.max_cuts_per_phase == 50);
  REQUIRE(opts.cut_prune_interval.has_value());
  CHECK(*opts.cut_prune_interval == 5);
  REQUIRE(opts.prune_dual_threshold.has_value());
  CHECK(*opts.prune_dual_threshold == doctest::Approx(1e-6));
  REQUIRE(opts.single_cut_storage.has_value());
  CHECK(*opts.single_cut_storage == true);
  REQUIRE(opts.max_stored_cuts.has_value());
  CHECK(*opts.max_stored_cuts == 1000);
}

TEST_CASE("SddpOptions - Construction with aperture fields")
{
  const SddpOptions opts {
      .apertures =
          Array<Uid> {
              1,
              2,
              3,
          },
      .aperture_directory = "aperture_data",
      .aperture_timeout = 30.0,
      .save_aperture_lp = true,
  };

  REQUIRE(opts.apertures.has_value());
  REQUIRE(opts.apertures->size() == 3);
  CHECK((*opts.apertures)[0] == 1);
  CHECK((*opts.apertures)[1] == 2);
  CHECK((*opts.apertures)[2] == 3);
  REQUIRE(opts.aperture_directory.has_value());
  CHECK(*opts.aperture_directory == "aperture_data");
  REQUIRE(opts.aperture_timeout.has_value());
  CHECK(*opts.aperture_timeout == doctest::Approx(30.0));
  REQUIRE(opts.save_aperture_lp.has_value());
  CHECK(*opts.save_aperture_lp == true);
}

TEST_CASE("SddpOptions - Empty apertures array means pure Benders")
{
  const SddpOptions opts {
      .apertures = Array<Uid> {},
  };

  REQUIRE(opts.apertures.has_value());
  CHECK(opts.apertures->empty());
}

TEST_CASE("SddpOptions - Construction with boundary cut fields")
{
  const SddpOptions opts {
      .boundary_cuts_file = "boundary.csv",
      .boundary_cuts_mode = "combined",
      .boundary_max_iterations = 50,
      .named_cuts_file = "named.csv",
  };

  REQUIRE(opts.boundary_cuts_file.has_value());
  CHECK(*opts.boundary_cuts_file == "boundary.csv");
  REQUIRE(opts.boundary_cuts_mode.has_value());
  CHECK(*opts.boundary_cuts_mode == "combined");
  REQUIRE(opts.boundary_max_iterations.has_value());
  CHECK(*opts.boundary_max_iterations == 50);
  REQUIRE(opts.named_cuts_file.has_value());
  CHECK(*opts.named_cuts_file == "named.csv");
}

TEST_CASE("SddpOptions - Construction with advanced tuning fields")
{
  const SddpOptions opts {
      .elastic_penalty = 1e5,
      .alpha_min = -1e6,
      .alpha_max = 1e10,
      .warm_start = false,
      .use_clone_pool = false,
      .simulation_mode = true,
      .stationary_tol = 0.01,
      .stationary_window = 20,
  };

  REQUIRE(opts.elastic_penalty.has_value());
  CHECK(*opts.elastic_penalty == doctest::Approx(1e5));
  REQUIRE(opts.alpha_min.has_value());
  CHECK(*opts.alpha_min == doctest::Approx(-1e6));
  REQUIRE(opts.alpha_max.has_value());
  CHECK(*opts.alpha_max == doctest::Approx(1e10));
  REQUIRE(opts.warm_start.has_value());
  CHECK(*opts.warm_start == false);
  REQUIRE(opts.use_clone_pool.has_value());
  CHECK(*opts.use_clone_pool == false);
  REQUIRE(opts.simulation_mode.has_value());
  CHECK(*opts.simulation_mode == true);
  REQUIRE(opts.stationary_tol.has_value());
  CHECK(*opts.stationary_tol == doctest::Approx(0.01));
  REQUIRE(opts.stationary_window.has_value());
  CHECK(*opts.stationary_window == 20);
}

TEST_CASE("SddpOptions - Construction with nested solver_options")
{
  const SddpOptions opts {
      .solver_options =
          SolverOptions {
              .algorithm = LPAlgo::dual,
              .threads = 1,
              .presolve = false,
              .reuse_basis = true,
          },
  };

  REQUIRE(opts.solver_options.has_value());
  CHECK(opts.solver_options->algorithm == LPAlgo::dual);
  CHECK(opts.solver_options->threads == 1);
  CHECK(opts.solver_options->presolve == false);
  CHECK(opts.solver_options->reuse_basis == true);
}

TEST_CASE("SddpOptions - Merge fills missing fields")
{
  SddpOptions base {
      .max_iterations = 100,
      .convergence_tol = 1e-4,
      .elastic_mode = "single_cut",
  };

  SddpOptions overlay {
      .cut_sharing_mode = "expected",
      .min_iterations = 5,
      .elastic_penalty = 1e6,
      .warm_start = true,
  };

  base.merge(std::move(overlay));

  // Base preserved
  REQUIRE(base.max_iterations.has_value());
  CHECK(*base.max_iterations == 100);
  REQUIRE(base.convergence_tol.has_value());
  CHECK(*base.convergence_tol == doctest::Approx(1e-4));
  REQUIRE(base.elastic_mode.has_value());
  CHECK(*base.elastic_mode == "single_cut");

  // New from overlay
  REQUIRE(base.cut_sharing_mode.has_value());
  CHECK(*base.cut_sharing_mode == "expected");
  REQUIRE(base.min_iterations.has_value());
  CHECK(*base.min_iterations == 5);
  REQUIRE(base.elastic_penalty.has_value());
  CHECK(*base.elastic_penalty == doctest::Approx(1e6));
  REQUIRE(base.warm_start.has_value());
  CHECK(*base.warm_start == true);
}

TEST_CASE("SddpOptions - Merge overwrites existing (overlay wins)")
{
  SddpOptions base {
      .max_iterations = 100,
      .elastic_mode = "single_cut",
  };

  SddpOptions overlay {
      .max_iterations = 999,
      .elastic_mode = "multi_cut",
  };

  base.merge(std::move(overlay));

  // Overlay wins: set fields overwrite base
  REQUIRE(base.max_iterations.has_value());
  CHECK(*base.max_iterations == 999);
  REQUIRE(base.elastic_mode.has_value());
  CHECK(*base.elastic_mode == "multi_cut");
}

TEST_CASE("SddpOptions - Merge apertures replaces when set")
{
  SddpOptions base {
      .apertures =
          Array<Uid> {
              1,
              2,
          },
  };

  SddpOptions overlay {
      .apertures =
          Array<Uid> {
              10,
              20,
              30,
          },
  };

  base.merge(std::move(overlay));

  REQUIRE(base.apertures.has_value());
  REQUIRE(base.apertures->size() == 3);
  CHECK((*base.apertures)[0] == 10);
}

TEST_CASE("SddpOptions - Merge apertures not replaced when overlay absent")
{
  SddpOptions base {
      .apertures =
          Array<Uid> {
              1,
              2,
          },
  };

  SddpOptions overlay {};

  base.merge(std::move(overlay));

  REQUIRE(base.apertures.has_value());
  CHECK(base.apertures->size() == 2);
}

TEST_CASE("SddpOptions - Merge nested solver_options")
{
  SUBCASE("both have solver_options: inner merge")
  {
    SddpOptions base {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::barrier,
                .optimal_eps = 1e-8,
            },
    };

    SddpOptions overlay {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
                .feasible_eps = 1e-7,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.solver_options.has_value());
    // Non-optional fields keep base
    CHECK(base.solver_options->algorithm == LPAlgo::barrier);
    // Optional: base keeps its own
    REQUIRE(base.solver_options->optimal_eps.has_value());
    CHECK(base.solver_options->optimal_eps.value_or(0.0)
          == doctest::Approx(1e-8));
    // Optional: overlay fills missing
    REQUIRE(base.solver_options->feasible_eps.has_value());
    CHECK(base.solver_options->feasible_eps.value_or(0.0)
          == doctest::Approx(1e-7));
  }

  SUBCASE("base has no solver_options: takes overlay")
  {
    SddpOptions base {};

    SddpOptions overlay {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
                .threads = 8,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.solver_options.has_value());
    CHECK(base.solver_options->algorithm == LPAlgo::dual);
    CHECK(base.solver_options->threads == 8);
  }

  SUBCASE("overlay has no solver_options: base unchanged")
  {
    SddpOptions base {
        .solver_options =
            SolverOptions {
                .algorithm = LPAlgo::primal,
            },
    };

    SddpOptions overlay {};

    base.merge(std::move(overlay));

    REQUIRE(base.solver_options.has_value());
    CHECK(base.solver_options->algorithm == LPAlgo::primal);
  }
}

TEST_CASE(
    "SddpOptions - Default construction has no forward/backward solver "
    "options")
{
  const SddpOptions opts {};

  CHECK_FALSE(opts.forward_solver_options.has_value());
  CHECK_FALSE(opts.backward_solver_options.has_value());
}

TEST_CASE("SddpOptions - Construction with forward/backward solver options")
{
  const SddpOptions opts {
      .forward_solver_options =
          SolverOptions {
              .algorithm = LPAlgo::barrier,
              .threads = 4,
          },
      .backward_solver_options =
          SolverOptions {
              .algorithm = LPAlgo::dual,
              .threads = 1,
              .reuse_basis = true,
          },
  };

  REQUIRE(opts.forward_solver_options.has_value());
  CHECK(opts.forward_solver_options->algorithm == LPAlgo::barrier);
  CHECK(opts.forward_solver_options->threads == 4);

  REQUIRE(opts.backward_solver_options.has_value());
  CHECK(opts.backward_solver_options->algorithm == LPAlgo::dual);
  CHECK(opts.backward_solver_options->threads == 1);
  CHECK(opts.backward_solver_options->reuse_basis == true);
}

TEST_CASE("SddpOptions - Merge forward/backward solver_options")
{
  SUBCASE("both have forward_solver_options: inner merge")
  {
    SddpOptions base {
        .forward_solver_options =
            SolverOptions {
                .algorithm = LPAlgo::barrier,
                .optimal_eps = 1e-8,
            },
    };

    SddpOptions overlay {
        .forward_solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
                .feasible_eps = 1e-7,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.forward_solver_options.has_value());
    CHECK(base.forward_solver_options->algorithm == LPAlgo::barrier);
    REQUIRE(base.forward_solver_options->optimal_eps.has_value());
    CHECK(base.forward_solver_options->optimal_eps.value_or(0.0)
          == doctest::Approx(1e-8));
    REQUIRE(base.forward_solver_options->feasible_eps.has_value());
    CHECK(base.forward_solver_options->feasible_eps.value_or(0.0)
          == doctest::Approx(1e-7));
  }

  SUBCASE("base has no backward_solver_options: takes overlay")
  {
    SddpOptions base {};

    SddpOptions overlay {
        .backward_solver_options =
            SolverOptions {
                .algorithm = LPAlgo::dual,
                .reuse_basis = true,
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.backward_solver_options.has_value());
    CHECK(base.backward_solver_options->algorithm == LPAlgo::dual);
    CHECK(base.backward_solver_options->reuse_basis == true);
  }

  SUBCASE("overlay has no forward_solver_options: base unchanged")
  {
    SddpOptions base {
        .forward_solver_options =
            SolverOptions {
                .algorithm = LPAlgo::primal,
            },
    };

    SddpOptions overlay {};

    base.merge(std::move(overlay));

    REQUIRE(base.forward_solver_options.has_value());
    CHECK(base.forward_solver_options->algorithm == LPAlgo::primal);
    CHECK_FALSE(base.backward_solver_options.has_value());
  }
}
