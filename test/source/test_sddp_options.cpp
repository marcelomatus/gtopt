/**
 * @file      test_sddp_options.hpp
 * @brief     Unit tests for SddpOptions struct
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_options.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

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
  CHECK_FALSE(opts.boundary_cuts_file.has_value());
  CHECK_FALSE(opts.boundary_cuts_mode.has_value());
  CHECK_FALSE(opts.boundary_max_iterations.has_value());
  CHECK_FALSE(opts.max_cuts_per_phase.has_value());
  CHECK_FALSE(opts.cut_prune_interval.has_value());
  CHECK_FALSE(opts.prune_dual_threshold.has_value());
  CHECK_FALSE(opts.single_cut_storage.has_value());
  CHECK_FALSE(opts.max_stored_cuts.has_value());
  CHECK_FALSE(opts.simulation_mode.has_value());
  CHECK_FALSE(opts.stationary_tol.has_value());
  CHECK_FALSE(opts.stationary_window.has_value());
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
      .cut_sharing_mode = CutSharingMode::multicut,
      .cut_directory = "my_cuts",
      .cut_recovery_mode = HotStartMode::append,
      .recovery_mode = RecoveryMode::cuts,
      .save_per_iteration = true,
      .cuts_input_file = "initial_cuts.csv",
      .elastic_mode = ElasticFilterMode::multi_cut,
      .multi_cut_threshold = 20,
      .max_cuts_per_phase = 50,
      .cut_prune_interval = 5,
      .prune_dual_threshold = 1e-6,
      .single_cut_storage = true,
      .max_stored_cuts = 1000,
  };

  REQUIRE(opts.cut_sharing_mode.has_value());
  CHECK(*opts.cut_sharing_mode == CutSharingMode::multicut);
  REQUIRE(opts.cut_directory.has_value());
  CHECK(*opts.cut_directory == "my_cuts");
  REQUIRE(opts.cut_recovery_mode.has_value());
  CHECK(*opts.cut_recovery_mode == HotStartMode::append);
  REQUIRE(opts.recovery_mode.has_value());
  CHECK(*opts.recovery_mode == RecoveryMode::cuts);
  REQUIRE(opts.save_per_iteration.has_value());
  CHECK(*opts.save_per_iteration == true);
  REQUIRE(opts.cuts_input_file.has_value());
  CHECK(*opts.cuts_input_file == "initial_cuts.csv");
  REQUIRE(opts.elastic_mode.has_value());
  CHECK(*opts.elastic_mode == ElasticFilterMode::multi_cut);
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
      .boundary_cuts_mode = BoundaryCutsMode::combined,
      .boundary_max_iterations = 50,
  };

  REQUIRE(opts.boundary_cuts_file.has_value());
  CHECK(*opts.boundary_cuts_file == "boundary.csv");
  REQUIRE(opts.boundary_cuts_mode.has_value());
  CHECK(*opts.boundary_cuts_mode == BoundaryCutsMode::combined);
  REQUIRE(opts.boundary_max_iterations.has_value());
  CHECK(*opts.boundary_max_iterations == 50);

  // ``named_cuts_file`` was retired in 2026-05; internal hot-start
  // cuts now travel via ``cuts_input_file`` (Parquet) only.
}

TEST_CASE("SddpOptions - Construction with advanced tuning fields")
{
  const SddpOptions opts {
      .elastic_penalty = 1e5,
      .simulation_mode = true,
      .stationary_tol = 0.01,
      .stationary_window = 20,
  };

  REQUIRE(opts.elastic_penalty.has_value());
  CHECK(*opts.elastic_penalty == doctest::Approx(1e5));
  REQUIRE(opts.simulation_mode.has_value());
  CHECK(*opts.simulation_mode == true);
  REQUIRE(opts.stationary_tol.has_value());
  CHECK(*opts.stationary_tol == doctest::Approx(0.01));
  REQUIRE(opts.stationary_window.has_value());
  CHECK(*opts.stationary_window == 20);
}

TEST_CASE("SddpOptions - Merge fills missing fields")
{
  SddpOptions base {
      .max_iterations = 100,
      .convergence_tol = 1e-4,
      .elastic_mode = ElasticFilterMode::single_cut,
  };

  SddpOptions overlay {
      .cut_sharing_mode = CutSharingMode::multicut,
      .min_iterations = 5,
      .elastic_penalty = 1e6,
  };

  base.merge(std::move(overlay));

  // Base preserved
  REQUIRE(base.max_iterations.has_value());
  CHECK(*base.max_iterations == 100);
  REQUIRE(base.convergence_tol.has_value());
  CHECK(*base.convergence_tol == doctest::Approx(1e-4));
  REQUIRE(base.elastic_mode.has_value());
  CHECK(*base.elastic_mode == ElasticFilterMode::single_cut);

  // New from overlay
  REQUIRE(base.cut_sharing_mode.has_value());
  CHECK(*base.cut_sharing_mode == CutSharingMode::multicut);
  REQUIRE(base.min_iterations.has_value());
  CHECK(*base.min_iterations == 5);
  REQUIRE(base.elastic_penalty.has_value());
  CHECK(*base.elastic_penalty == doctest::Approx(1e6));
}

TEST_CASE("SddpOptions - Merge overwrites existing (overlay wins)")
{
  SddpOptions base {
      .max_iterations = 100,
      .elastic_mode = ElasticFilterMode::single_cut,
  };

  SddpOptions overlay {
      .max_iterations = 999,
      .elastic_mode = ElasticFilterMode::multi_cut,
  };

  base.merge(std::move(overlay));

  // Overlay wins: set fields overwrite base
  REQUIRE(base.max_iterations.has_value());
  CHECK(*base.max_iterations == 999);
  REQUIRE(base.elastic_mode.has_value());
  CHECK(*base.elastic_mode == ElasticFilterMode::multi_cut);
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
          },
  };

  REQUIRE(opts.forward_solver_options.has_value());
  CHECK(opts.forward_solver_options->algorithm == LPAlgo::barrier);
  CHECK(opts.forward_solver_options->threads == 4);

  REQUIRE(opts.backward_solver_options.has_value());
  CHECK(opts.backward_solver_options->algorithm == LPAlgo::dual);
  CHECK(opts.backward_solver_options->threads == 1);
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
            },
    };

    base.merge(std::move(overlay));

    REQUIRE(base.backward_solver_options.has_value());
    CHECK(base.backward_solver_options->algorithm == LPAlgo::dual);
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

TEST_CASE("SddpOptions - aperture_solve_mode default and construction")
{
  SUBCASE("default is unset (resolves to warm at the accessor)")
  {
    const SddpOptions opts {};
    CHECK_FALSE(opts.aperture_solve_mode.has_value());
    // The accessor PlanningOptionsLP::sddp_aperture_solve_mode() resolves an
    // absent field to warm (default flipped reduced_cost→warm 2026-07-03: the
    // aperture warm-start is faster once presolve-off + parallel chunks land).
    CHECK(opts.aperture_solve_mode.value_or(ApertureSolveMode::warm)
          == ApertureSolveMode::warm);
  }

  SUBCASE("explicit reduced_cost")
  {
    const SddpOptions opts {
        .aperture_solve_mode = ApertureSolveMode::reduced_cost,
    };
    REQUIRE(opts.aperture_solve_mode.has_value());
    CHECK(*opts.aperture_solve_mode == ApertureSolveMode::reduced_cost);
  }
}

TEST_CASE("ApertureSolveMode - enum name parsing and aliases")
{
  SUBCASE("canonical names")
  {
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "cold")
          == ApertureSolveMode::cold);
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "warm")
          == ApertureSolveMode::warm);
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "reduced_cost")
          == ApertureSolveMode::reduced_cost);
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "dual_shared")
          == ApertureSolveMode::dual_shared);
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "screened")
          == ApertureSolveMode::screened);
  }

  SUBCASE("back-compat aliases")
  {
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "warm_start")
          == ApertureSolveMode::warm);
    CHECK(require_enum<ApertureSolveMode>("aperture_solve_mode", "barrier")
          == ApertureSolveMode::reduced_cost);
  }

  SUBCASE("invalid value throws")
  {
    CHECK_FALSE(enum_from_name<ApertureSolveMode>("nope").has_value());
    CHECK_THROWS_AS(
        (void)require_enum<ApertureSolveMode>("aperture_solve_mode", "nope"),
        std::invalid_argument);
  }
}

TEST_CASE("SddpOptions - aperture_screen_count default and merge")
{
  SUBCASE("default is unset (resolves to 2 at the accessor)")
  {
    const SddpOptions opts {};
    CHECK_FALSE(opts.aperture_screen_count.has_value());
    CHECK(opts.aperture_screen_count.value_or(2) == 2);
  }

  SUBCASE("overlay wins when set")
  {
    SddpOptions base {.aperture_screen_count = 1};
    SddpOptions overlay {.aperture_screen_count = 4};
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_screen_count.has_value());
    CHECK(*base.aperture_screen_count == 4);
  }

  SUBCASE("base kept when overlay absent")
  {
    SddpOptions base {.aperture_screen_count = 3};
    SddpOptions overlay {};
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_screen_count.has_value());
    CHECK(*base.aperture_screen_count == 3);
  }
}

TEST_CASE("SddpOptions - aperture_solve_mode merge")
{
  SUBCASE("overlay wins when set")
  {
    SddpOptions base {.aperture_solve_mode = ApertureSolveMode::cold};
    SddpOptions overlay {
        .aperture_solve_mode = ApertureSolveMode::reduced_cost,
    };
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_solve_mode.has_value());
    CHECK(*base.aperture_solve_mode == ApertureSolveMode::reduced_cost);
  }

  SUBCASE("base kept when overlay absent")
  {
    SddpOptions base {.aperture_solve_mode = ApertureSolveMode::warm};
    SddpOptions overlay {};
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_solve_mode.has_value());
    CHECK(*base.aperture_solve_mode == ApertureSolveMode::warm);
  }
}

// Regression guard: `aperture_seed_basis` was added to the struct + JSON
// contract but omitted from `merge()`, so option resolution silently dropped
// it to nullopt and the feature never engaged.  Every field-by-field copy
// function (here `merge`, and `SolverOptions::overlay` for the cold-canonical
// flag) must carry each new field.
TEST_CASE("SddpOptions - aperture_seed_basis merge")  // NOLINT
{
  SUBCASE("overlay wins when set")
  {
    SddpOptions base {};
    SddpOptions overlay {.aperture_seed_basis = true};
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_seed_basis.has_value());
    CHECK(*base.aperture_seed_basis);
  }

  SUBCASE("base kept when overlay absent")
  {
    SddpOptions base {.aperture_seed_basis = true};
    SddpOptions overlay {};
    base.merge(std::move(overlay));
    REQUIRE(base.aperture_seed_basis.has_value());
    CHECK(*base.aperture_seed_basis);
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)