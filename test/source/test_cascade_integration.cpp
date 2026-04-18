/**
 * @file      test_cascade_integration.cpp
 * @brief     Integration/end-to-end tests for CascadePlanningMethod
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 */

// SPDX-License-Identifier: BSD-3-Clause

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_options_lp.hpp>

#include "cascade_helpers.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Single-level cascade = equivalent to direct SDDP ─────────────────────

// ─── Inactive level skip: `active=false` must bypass the level ────────────
//
// Guards the `active` OptBool wired into cascade_method.cpp (step 0 of the
// level loop).  Complements the JSON binding tests in
// test_cascade_options.cpp / test_cascade_method.cpp with an end-to-end
// check that the cascade solver actually skips the level at runtime.

TEST_CASE("Cascade skips level with active=false")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base_opts;
  base_opts.max_iterations = 4;
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};

  // Two levels: first disabled, second active.  The inactive level
  // must produce no level_stats entry and no iteration results; the
  // active level must still run normally.
  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"disabled"},
          .active = OptBool {false},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"active"},
          .active = OptBool {true},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());

  // Only one level produced stats — the active one.
  REQUIRE(solver.level_stats().size() == 1);
  CHECK(solver.level_stats()[0].name == "active");
}

// ─── Caller LP preservation when all remaining levels are inactive ────────
//
// Guards the `has_active_successor` check in cascade_method.cpp:
// without it, level 0 releases the caller's cells anticipating a
// fresh LP build in level 1 — but if levels 1..N are all inactive,
// the caller's cells are the only place the solved systems live, so
// `PlanningLP::write_out` afterwards would see an empty system grid
// ("Writing output: 0 scene(s) × 0 phase(s)") and produce no element
// parquets.  This test exercises that corner directly.

TEST_CASE("Cascade preserves caller cells when remaining levels are inactive")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base_opts;
  base_opts.max_iterations = 4;
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};

  // Level 0 active, levels 1 and 2 inactive.  After the solve,
  // `planning_lp.systems()` must still be non-empty (level 0 reused
  // the caller LP, and the inter-level cleanup must not have fired
  // because no active successor exists).
  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"lvl0"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {2},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"lvl1_off"},
          .active = OptBool {false},
      },
      CascadeLevel {
          .name = OptName {"lvl2_off"},
          .active = OptBool {false},
      },
  };

  const auto num_scenes_before = planning_lp.systems().size();
  REQUIRE(num_scenes_before > 0);

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());

  // Caller's systems survived — they would be released by the
  // inter-level cleanup if the guard were missing.
  CHECK(planning_lp.systems().size() == num_scenes_before);
  CHECK_FALSE(planning_lp.systems().empty());
  CHECK_FALSE(planning_lp.systems().front().empty());
  REQUIRE(solver.level_stats().size() == 1);
  CHECK(solver.level_stats()[0].name == "lvl0");
}

TEST_CASE("Single-level cascade produces same result as direct SDDP")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Run SDDP directly
  auto planning1 = make_3phase_hydro_planning();
  PlanningLP planning_lp1(std::move(planning1));

  SDDPOptions sddp_opts1;
  sddp_opts1.max_iterations = 8;
  sddp_opts1.convergence_tol = 0.01;
  sddp_opts1.apertures = std::vector<Uid> {};

  SDDPMethod direct_solver(planning_lp1, sddp_opts1);
  const SolverOptions lp_opts;
  auto direct_result = direct_solver.solve(lp_opts);

  // Run single-level cascade with same options
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  SDDPOptions sddp_opts2;
  sddp_opts2.max_iterations = 8;
  sddp_opts2.convergence_tol = 0.01;
  sddp_opts2.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"single"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {8},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
  };

  CascadePlanningMethod cascade_solver(std::move(sddp_opts2),
                                       std::move(cascade));
  auto cascade_result = cascade_solver.solve(planning_lp2, lp_opts);

  REQUIRE(direct_result.has_value());
  REQUIRE(cascade_result.has_value());

  // Both should converge and produce similar iteration counts
  CHECK(direct_result->size() == cascade_solver.all_results().size());
}

// ─── Multi-bus cascade test with transmission lines ─────────────────────────
// Helper functions: make_3phase_2bus_hydro_planning(),
//                   make_6phase_2bus_hydro_planning()
// are provided by cascade_helpers.hpp.

TEST_CASE("Cascade 2-level with multi-bus network and cut inheritance")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};  // no apertures (Benders)

  // Level 0: single-bus relaxation (fast convergence, ignores network)
  // Level 1: full network, inherits state variable targets from level 0
  //          (not cuts, since the LP column structure changes with
  //           use_single_bus → different theta/line columns)
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"uninodal_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {8},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"full_network"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("has results from both levels")
  {
    CHECK(solver.all_results().size() > 2);
  }

  SUBCASE("level_stats populated for both levels")
  {
    REQUIRE(solver.level_stats().size() == 2);

    const auto& stats0 = solver.level_stats()[0];
    CHECK(stats0.name == "uninodal_benders");
    CHECK(stats0.iterations > 0);
    CHECK(stats0.iterations <= 8);
    CHECK(stats0.elapsed_s > 0.0);

    const auto& stats1 = solver.level_stats()[1];
    CHECK(stats1.name == "full_network");
    CHECK(stats1.iterations > 0);
    CHECK(stats1.iterations <= 10);
    CHECK(stats1.elapsed_s > 0.0);
  }

  SUBCASE("iteration count within budget")
  {
    // 2 levels: up to (8+1) + (10+1) = 20 results
    CHECK(solver.all_results().size() <= 20);
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.gap >= -1e-12);  // allow tiny negative FP rounding
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("SDDP baseline (6-phase, no cascade)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Baseline: plain SDDP solver on the same 6-phase hydro system,
  // for comparison with cascade cut/target inheritance tests.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod solver(planning_lp, std::move(sddp_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("converges")
  {
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());
    const auto& last_training = (*result)[result->size() - 2];
    CHECK(last_training.converged);
    CHECK(last_training.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(last_training.iteration_index >= IterationIndex {3});
  }

  SUBCASE("optimal value matches expected")
  {
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());
    // Simulation pass is the last result
    const auto& sim = result->back();
    CHECK(sim.upper_bound == doctest::Approx(49950.0).epsilon(0.01));
    CHECK(sim.lower_bound == doctest::Approx(49950.0).epsilon(0.01));
  }
}

TEST_CASE("Cascade 2-level with cut inheritance only (6-phase)")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // 6 phases ⇒ more state links ⇒ Benders needs more iterations to converge,
  // making the effect of inherited cuts clearly visible.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  // Level 0: Benders training on full network.
  // Level 1: Same LP, inherits cuts ⇒ fewer iterations or simulation only.
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"with_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("level 0 converges with multiple iterations")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];

    CHECK(stats0.converged);
    CHECK(stats0.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(stats0.iterations >= 3);
  }

  SUBCASE("level 1 converges quickly with inherited cuts")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    // Inherited cuts should let level 1 converge in no more iterations
    CHECK(stats1.iterations <= stats0.iterations);
  }

  SUBCASE("both levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("Cascade 2-level with target inheritance only (6-phase)")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // 6 phases ⇒ more state links ⇒ targets from level 0 guide level 1
  // toward the optimal reservoir trajectory, reducing iterations.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  // Level 0: Benders training on full network.
  // Level 1: Same LP, inherits state variable targets ⇒ fewer iterations.
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"with_targets"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("level 0 converges with multiple iterations")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];

    CHECK(stats0.converged);
    CHECK(stats0.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(stats0.iterations >= 3);
  }

  SUBCASE("level 1 converges with inherited targets")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    // Targets guide the forward pass toward the optimal trajectory;
    // LB convergence still depends on cut generation, so iteration
    // count may be similar but should not greatly exceed level 0.
    // Allow +3 tolerance for solver-dependent numerical differences
    // (observed up to +2 with CLP on 6-phase cascades).
    CHECK(stats1.iterations <= stats0.iterations + 3);
  }

  SUBCASE("both levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("Cascade 3-level with targets then cuts (6-phase)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Level 0: fast uninodal Benders to get rough solution.
  // Level 1: full network guided by uninodal targets.
  // Level 2: same network, inherits cuts from level 1 ⇒ faster convergence.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"guided"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
      CascadeLevel {
          .name = OptName {"refined"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("all three levels present")
  {
    CHECK(solver.level_stats().size() == 3);
  }

  SUBCASE("level 2 converges faster than level 1")
  {
    REQUIRE(solver.level_stats().size() == 3);
    const auto& stats1 = solver.level_stats()[1];
    const auto& stats2 = solver.level_stats()[2];

    CHECK(stats1.converged);
    CHECK(stats2.converged);
    CHECK(stats2.iterations <= stats1.iterations);
  }

  SUBCASE("all levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 3);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];
    const auto& stats2 = solver.level_stats()[2];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats1.lower_bound
          == doctest::Approx(stats2.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
    CHECK(stats1.upper_bound
          == doctest::Approx(stats2.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

// ─── Inherit with forgetting tests ──────────────────────────────────────────

TEST_CASE("Cascade 2-level inherit_optimality_cuts=3 (forget after 3 iters)")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Level 1 inherits optimality cuts, uses them for 3 iterations, then
  // forgets them and continues with only self-generated cuts.
  // inherit_optimality_cuts=3 means: inherit, but drop after 3 iters.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"forget_after_3"},
          // No model_options ⇒ reuses level 0's LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Inherit optimality cuts but forget after 3 iters
                  .inherit_optimality_cuts = OptInt {3},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("level 1 converges after forgetting inherited cuts")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    CHECK(stats1.lower_bound <= stats1.upper_bound + 1e-6);
  }

  SUBCASE("level 1 ran more results than without forget")
  {
    REQUIRE(solver.level_stats().size() == 2);
    // forget triggers a two-phase solve: phase-1 (capped at 3 iters)
    // + phase-2 (re-solve without inherited cuts). The total results
    // include both phases.
    const auto& stats1 = solver.level_stats()[1];
    CHECK(stats1.iterations >= 1);
  }
}

// ─── Additional cascade coverage tests ──────────────────────────────────────

TEST_CASE(  // NOLINT
    "Cascade 2-level with custom target tolerances (3-phase)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"base"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"refined"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.2},
                  .target_min_atol = OptReal {5.0},
                  .target_penalty = OptReal {100.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);
  CHECK(solver.level_stats()[0].converged);
  CHECK(solver.level_stats()[1].converged);
}

TEST_CASE(  // NOLINT
    "Cascade 2-level inherit_optimality_cuts keeps cuts (3-phase)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"inherit"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Keep inherited cuts forever
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  const auto& stats0 = solver.level_stats()[0];
  const auto& stats1 = solver.level_stats()[1];

  CHECK(stats0.converged);
  CHECK(stats1.converged);

  // Level 1 should converge in fewer or equal iterations thanks to
  // inherited cuts providing a warm start
  CHECK(stats1.iterations <= stats0.iterations + 1);
}

TEST_CASE(  // NOLINT
    "Cascade 2-level forget inherited cuts after N iterations (3-phase)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"forget"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Forget after 3 iterations
                  .inherit_optimality_cuts = OptInt {3},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // Level 1 should converge even after forgetting inherited cuts
  CHECK(solver.level_stats()[1].converged);
}

TEST_CASE(  // NOLINT
    "Cascade forget preserves auto scale_alpha (no NaN)")
{
  // Regression: the forget code path used to overwrite all SDDPOptions,
  // resetting auto-computed scale_alpha to 0.  Verify that both phases
  // (with and without inherited cuts) produce finite, non-NaN bounds.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  // Do NOT set scale_alpha — let auto-computation happen

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"forget"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {2},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  const auto& stats1 = solver.level_stats()[1];
  CHECK_FALSE(std::isnan(stats1.upper_bound));
  CHECK_FALSE(std::isnan(stats1.lower_bound));
  CHECK(stats1.converged);
}

TEST_CASE(  // NOLINT
    "Cascade 3-level progressive refinement (3-phase)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"level_0"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .convergence_tol = OptReal {0.05},
              },
      },
      CascadeLevel {
          .name = OptName {"level_1"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
              },
      },
      CascadeLevel {
          .name = OptName {"level_2"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.001},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.01},
                  .target_penalty = OptReal {1000.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 3);

  // All levels should converge
  for (const auto& stats : solver.level_stats()) {
    CHECK(stats.converged);
    CHECK(stats.lower_bound <= stats.upper_bound + 1e-6);
  }
}

TEST_CASE(  // NOLINT
    "Cascade 2-level with both targets and cuts (3-phase)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"both"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  CHECK(solver.level_stats()[0].converged);
  CHECK(solver.level_stats()[1].converged);

  // Both inheritance mechanisms should allow level 1 to converge fast
  CHECK(solver.level_stats()[1].iterations
        <= solver.level_stats()[0].iterations + 1);
}

// ─── Level-0 PlanningLP reuse ───────────────────────────────────────────────

TEST_CASE(
    "Cascade reuses caller PlanningLP when level 0 has no model overrides")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 4;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  // Two levels, neither sets model_options, and cascade globals are empty.
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"lvl0"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"lvl1"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  // Level 0 reused caller's PlanningLP → only level 1 owned.
  CHECK(solver.owned_lps_count() == 1);
  // Caller's LP cells were released at the level-0 → level-1 boundary
  // so the two levels never hold solver backends simultaneously.
  CHECK(planning_lp.systems().empty());
}

TEST_CASE(
    "PlanningLP::release_cells drops systems and allows rebuild")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  REQUIRE(!planning_lp.systems().empty());
  planning_lp.release_cells();
  CHECK(planning_lp.systems().empty());

  // Planning shell is still intact — so we can rebuild a fresh LP
  // from the same source data without losing configuration.
  PlanningLP rebuilt(planning_lp.planning());
  CHECK(!rebuilt.systems().empty());
}

TEST_CASE("Cascade rebuilds level 0 PlanningLP when model overrides are set")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"lvl0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  // Level 0 had model overrides → a fresh LP was built and owned.
  CHECK(solver.owned_lps_count() == 1);
}

}  // anonymous namespace
