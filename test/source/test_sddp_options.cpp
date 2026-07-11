/**
 * @file      test_sddp_options.hpp
 * @brief     Unit tests for SddpOptions struct
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <format>
#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_sddp_options.hpp>
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

// ═════════════════════════════════════════════════════════════════════════
// JSON ingestion: removed cut-sharing modes hard-error with the directed
// message (T6 / coverage-audit G6).  The runtime helper
// `parse_cut_sharing_mode` is covered elsewhere; THIS pins the daw-json
// custom-constructor branch in `json_sddp_options.hpp` — the path every
// planning JSON actually takes.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("SddpOptions JSON - removed cut_sharing_mode names throw")  // NOLINT
{
  using namespace std::string_view_literals;
  for (const auto name :
       {"accumulate"sv, "broadcast_mean"sv, "expected"sv, "max"sv})
  {
    const auto json = std::format(R"({{"cut_sharing_mode":"{}"}})", name);
    bool threw = false;
    std::string msg;
    try {
      (void)daw::json::from_json<SddpOptions>(std::string_view {json});
    } catch (const std::exception& e) {
      threw = true;
      msg = e.what();
    }
    INFO("mode ", name);
    CHECK(threw);
    // The dedicated removal message — not the generic unknown-enum error.
    CHECK(msg.find("REMOVED") != std::string::npos);
  }
  for (const auto name : {"none"sv, "multicut"sv, "markov"sv}) {
    const auto json = std::format(R"({{"cut_sharing_mode":"{}"}})", name);
    CHECK_NOTHROW(
        (void)daw::json::from_json<SddpOptions>(std::string_view {json}));
  }
}

// ═════════════════════════════════════════════════════════════════════════
// Full-population JSON round-trip (plumbing guard B1(a)): every field is
// set to a distinct non-default sentinel, serialized, re-parsed, and
// compared field-by-field.  The three lists in `json_sddp_options.hpp`
// (constructor parameters, json_member_list, to_json_data tuple) are
// POSITION-synced, so transposing two same-typed entries compiles clean
// and silently swaps fields — this test is what catches that; the
// `static_assert` on `SddpOptions::json_field_count` catches the
// added-field-missing-list case.  Real values are binary-exact
// (k / 2^n) so `==` round-trips through the JSON text.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("SddpOptions JSON - full-population round-trip")  // NOLINT
{
  SddpOptions opts;
  opts.cut_sharing_mode = CutSharingMode::multicut;
  opts.forward_sampling_mode = ForwardSamplingMode::resampled;
  opts.integer_cuts_mode = IntegerCutsMode::strengthened;
  opts.markov_states = Array<int> {0, 1};
  opts.markov_transition = Array<double> {0.25, 0.75, 0.5, 0.5};
  opts.cut_directory = "cutdir_s";
  opts.api_enabled = false;
  opts.update_lp_skip = 3;
  opts.max_iterations = 41;
  opts.min_iterations = 7;
  opts.convergence_tol = 0.25;
  opts.elastic_penalty = 321.5;
  opts.scale_alpha = 2.25;
  opts.cut_recovery_mode = HotStartMode::append;
  opts.recovery_mode = RecoveryMode::cuts;
  opts.save_per_iteration = false;
  opts.cuts_input_file = "cuts_in.parquet";
  opts.sentinel_file = "stop.now";
  opts.elastic_mode = ElasticFilterMode::multi_cut;
  opts.multi_cut_threshold = 9;
  opts.apertures = Array<Uid> {Uid {3}, Uid {5}};
  opts.num_apertures = 4;
  opts.aperture_selection_mode = "stride";
  opts.aperture_directory = "apdir";
  opts.aperture_system_file = "apsys.json";
  opts.aperture_timeout = 12.5;
  opts.save_aperture_lp = true;
  opts.lp_debug_passes = "aperture";
  opts.aperture_use_manual_clone = false;
  opts.aperture_drop_fcuts = true;
  opts.aperture_chunk_size = 6;
  opts.aperture_solve_mode = ApertureSolveMode::screened;
  opts.aperture_screen_count = 5;
  opts.aperture_seed_basis = true;
  opts.basis_cross_mode = BasisCrossMode::forward_to_backward;
  opts.boundary_cuts_file = "bcuts.csv";
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  opts.boundary_cut_sharing_mode = BoundaryCutSharingMode::multicut;
  opts.boundary_cuts_mean_shift = true;
  opts.boundary_max_iterations = 13;
  opts.missing_cut_var_mode = MissingCutVarMode::skip_cut;
  opts.max_cuts_per_phase = 77;
  opts.cut_prune_interval = 19;
  opts.prune_dual_threshold = 0.015625;
  opts.single_cut_storage = true;
  opts.max_stored_cuts = 1234;
  opts.simulation_mode = true;
  opts.low_memory_mode = LowMemoryMode::compress;
  opts.memory_codec = CompressionCodec::lz4;
  opts.cut_coeff_eps = 0.03125;
  opts.convergence_mode = ConvergenceMode::gap_stationary;
  opts.state_variable_lookup_mode = StateVariableLookupMode::cross_phase;
  opts.stationary_tol = 0.0625;
  opts.stationary_window = 5;
  opts.convergence_confidence = 0.75;
  opts.stationary_gap_ceiling = 0.125;
  opts.terminal_failure_threshold = 11;
  opts.forward_max_fallbacks = 21;
  opts.forward_fail_stop = true;
  opts.forward_infeas_rollback = true;
  opts.backward_resolve_target = false;
  opts.backward_max_fallbacks = 8;
  opts.max_async_spread = 3;
  opts.fact_eps = 0.0078125;
  opts.fact_max_cycles = 42;
  opts.fcut_log = true;
  {
    SolverOptions fso;
    fso.threads = 2;
    opts.forward_solver_options = fso;
    SolverOptions bso;
    bso.threads = 3;
    opts.backward_solver_options = bso;
  }

  const std::string json = daw::json::to_json(opts);
  const auto rt = daw::json::from_json<SddpOptions>(std::string_view {json});

  CHECK(rt.cut_sharing_mode == opts.cut_sharing_mode);
  CHECK(rt.forward_sampling_mode == opts.forward_sampling_mode);
  CHECK(rt.integer_cuts_mode == opts.integer_cuts_mode);
  CHECK(rt.markov_states == opts.markov_states);
  CHECK(rt.markov_transition == opts.markov_transition);
  CHECK(rt.cut_directory == opts.cut_directory);
  CHECK(rt.api_enabled == opts.api_enabled);
  CHECK(rt.update_lp_skip == opts.update_lp_skip);
  CHECK(rt.max_iterations == opts.max_iterations);
  CHECK(rt.min_iterations == opts.min_iterations);
  CHECK(rt.convergence_tol == opts.convergence_tol);
  CHECK(rt.elastic_penalty == opts.elastic_penalty);
  CHECK(rt.scale_alpha == opts.scale_alpha);
  CHECK(rt.cut_recovery_mode == opts.cut_recovery_mode);
  CHECK(rt.recovery_mode == opts.recovery_mode);
  CHECK(rt.save_per_iteration == opts.save_per_iteration);
  CHECK(rt.cuts_input_file == opts.cuts_input_file);
  CHECK(rt.sentinel_file == opts.sentinel_file);
  CHECK(rt.elastic_mode == opts.elastic_mode);
  CHECK(rt.multi_cut_threshold == opts.multi_cut_threshold);
  CHECK(rt.apertures == opts.apertures);
  CHECK(rt.num_apertures == opts.num_apertures);
  CHECK(rt.aperture_selection_mode == opts.aperture_selection_mode);
  CHECK(rt.aperture_directory == opts.aperture_directory);
  CHECK(rt.aperture_system_file == opts.aperture_system_file);
  CHECK(rt.aperture_timeout == opts.aperture_timeout);
  CHECK(rt.save_aperture_lp == opts.save_aperture_lp);
  CHECK(rt.lp_debug_passes == opts.lp_debug_passes);
  CHECK(rt.aperture_use_manual_clone == opts.aperture_use_manual_clone);
  CHECK(rt.aperture_drop_fcuts == opts.aperture_drop_fcuts);
  CHECK(rt.aperture_chunk_size == opts.aperture_chunk_size);
  CHECK(rt.aperture_solve_mode == opts.aperture_solve_mode);
  CHECK(rt.aperture_screen_count == opts.aperture_screen_count);
  CHECK(rt.aperture_seed_basis == opts.aperture_seed_basis);
  CHECK(rt.basis_cross_mode == opts.basis_cross_mode);
  CHECK(rt.boundary_cuts_file == opts.boundary_cuts_file);
  CHECK(rt.boundary_cuts_mode == opts.boundary_cuts_mode);
  CHECK(rt.boundary_cut_sharing_mode == opts.boundary_cut_sharing_mode);
  CHECK(rt.boundary_cuts_mean_shift == opts.boundary_cuts_mean_shift);
  CHECK(rt.boundary_max_iterations == opts.boundary_max_iterations);
  CHECK(rt.missing_cut_var_mode == opts.missing_cut_var_mode);
  CHECK(rt.max_cuts_per_phase == opts.max_cuts_per_phase);
  CHECK(rt.cut_prune_interval == opts.cut_prune_interval);
  CHECK(rt.prune_dual_threshold == opts.prune_dual_threshold);
  CHECK(rt.single_cut_storage == opts.single_cut_storage);
  CHECK(rt.max_stored_cuts == opts.max_stored_cuts);
  CHECK(rt.simulation_mode == opts.simulation_mode);
  CHECK(rt.low_memory_mode == opts.low_memory_mode);
  CHECK(rt.memory_codec == opts.memory_codec);
  CHECK(rt.cut_coeff_eps == opts.cut_coeff_eps);
  CHECK(rt.convergence_mode == opts.convergence_mode);
  CHECK(rt.state_variable_lookup_mode == opts.state_variable_lookup_mode);
  CHECK(rt.stationary_tol == opts.stationary_tol);
  CHECK(rt.stationary_window == opts.stationary_window);
  CHECK(rt.convergence_confidence == opts.convergence_confidence);
  CHECK(rt.stationary_gap_ceiling == opts.stationary_gap_ceiling);
  CHECK(rt.terminal_failure_threshold == opts.terminal_failure_threshold);
  CHECK(rt.forward_max_fallbacks == opts.forward_max_fallbacks);
  CHECK(rt.forward_fail_stop == opts.forward_fail_stop);
  CHECK(rt.forward_infeas_rollback == opts.forward_infeas_rollback);
  CHECK(rt.backward_resolve_target == opts.backward_resolve_target);
  CHECK(rt.backward_max_fallbacks == opts.backward_max_fallbacks);
  CHECK(rt.max_async_spread == opts.max_async_spread);
  CHECK(rt.fact_eps == opts.fact_eps);
  CHECK(rt.fact_max_cycles == opts.fact_max_cycles);
  CHECK(rt.fcut_log == opts.fcut_log);
  CHECK((rt.forward_solver_options.has_value()
         && rt.forward_solver_options->threads == 2));
  CHECK((rt.backward_solver_options.has_value()
         && rt.backward_solver_options->threads == 3));

  // String-level fixed point AFTER one parse cycle: serialize the
  // parsed value, re-parse, serialize again — the two strings must be
  // byte-identical (catches asymmetric to_json/from_json drift in the
  // SddpOptions contract).  The comparison deliberately starts from
  // `rt`, not `opts`: the NESTED SolverOptions contract is not a
  // one-pass fixed point (an unset `scaling` parses back engaged — a
  // pre-existing quirk of that contract, outside this guard's scope).
  const std::string json2 = daw::json::to_json(rt);
  const auto rt2 = daw::json::from_json<SddpOptions>(std::string_view {json2});
  CHECK(daw::json::to_json(rt2) == json2);
}

// NOLINTEND(bugprone-unchecked-optional-access)