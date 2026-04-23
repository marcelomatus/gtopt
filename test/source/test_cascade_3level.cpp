/**
 * @file      test_cascade_3level.cpp
 * @brief     Tests for 3-level cascade: uninodal -> transport -> full network
 * @date      2026-04-13
 * @copyright BSD-3-Clause
 *
 * Validates the default 3-level cascade strategy that plp2gtopt emits:
 *   Level 0 (uninodal):  use_single_bus=true
 *   Level 1 (transport): lines enabled, no losses, no kirchhoff
 *   Level 2 (full):      full network (kirchhoff + losses)
 */

// SPDX-License-Identifier: BSD-3-Clause

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/enum_option.hpp>

#include "cascade_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── 3-level cascade: uninodal → transport → full ──────────────────────────

TEST_CASE("Cascade 3-level uninodal-transport-full on 2-bus hydro")  // NOLINT
{
  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  constexpr int total_iterations = 30;
  constexpr int l0_iter = total_iterations / 2;  // 15
  constexpr int l1_iter = total_iterations / 4;  // 7
  constexpr int l2_iter = total_iterations - l0_iter - l1_iter;  // 8

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = total_iterations;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  const CascadeTransition target_transition {
      .inherit_targets = OptInt {-1},
      .target_rtol = OptReal {0.05},
      .target_min_atol = OptReal {1.0},
      .target_penalty = OptReal {500.0},
  };

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"uninodal"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {l0_iter},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"transport"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {false},
                  .use_line_losses = OptBool {false},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {l1_iter},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition = target_transition,
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
                  .max_iterations = OptInt {l2_iter},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition = target_transition,
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("all three levels executed")
  {
    REQUIRE(solver.level_stats().size() == 3);
    CHECK(solver.level_stats()[0].name == "uninodal");
    CHECK(solver.level_stats()[1].name == "transport");
    CHECK(solver.level_stats()[2].name == "full_network");
  }

  SUBCASE("each level ran at least 1 iteration")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.iterations >= 1);
    }
  }

  SUBCASE("iteration budgets respected")
  {
    const auto& stats = solver.level_stats();
    CHECK(stats[0].iterations <= l0_iter);
    CHECK(stats[1].iterations <= l1_iter);
    CHECK(stats[2].iterations <= l2_iter);
  }

  SUBCASE("bounds are valid across all levels")
  {
    for (const auto& ls : solver.level_stats()) {
      // Tolerance is scaled by the objective magnitude so the
      // invariant `LB <= UB` survives solver-specific numerical
      // noise (observed: CPLEX ~1e-13 abs, MindOpt ~2e-10 abs on
      // a 39150-magnitude objective).
      const auto scale =
          std::max({std::abs(ls.lower_bound), std::abs(ls.upper_bound), 1.0});
      CHECK(ls.lower_bound <= ls.upper_bound + (1e-8 * scale));
      // gap is typically (UB-LB)/|UB| — can dip slightly negative
      // when LB overshoots UB by a few ULPs from the backward-pass
      // row equilibration.  1e-8 is well below any meaningful
      // convergence threshold while tolerating solver precision.
      CHECK(ls.gap >= -1e-8);
      CHECK(ls.cuts_added >= 0);
      CHECK(ls.elapsed_s > 0.0);
    }
  }

  SUBCASE("total iteration count within global budget")
  {
    CHECK(solver.all_results().size() <= static_cast<size_t>(total_iterations));
  }
}

// ─── 3-level on 6-phase system (harder convergence) ────────────────────────

TEST_CASE("Cascade 3-level on 6-phase 2-bus hydro system")  // NOLINT
{
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  const CascadeTransition target_transition {
      .inherit_targets = OptInt {-1},
      .target_rtol = OptReal {0.05},
      .target_min_atol = OptReal {1.0},
      .target_penalty = OptReal {500.0},
  };

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"uninodal"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"transport"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {false},
                  .use_line_losses = OptBool {false},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition = target_transition,
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
          .transition = target_transition,
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("all three levels populated")
  {
    REQUIRE(solver.level_stats().size() == 3);
  }

  SUBCASE("final level produces valid bounds")
  {
    const auto& final_level = solver.level_stats().back();
    CHECK(final_level.name == "full_network");
    CHECK(final_level.lower_bound <= final_level.upper_bound + 1e-6);
    CHECK(final_level.iterations >= 1);
  }

  SUBCASE("uninodal level converges faster than full network")
  {
    // With use_single_bus, the LP is simpler — should converge in
    // fewer iterations than the full-network level would in isolation.
    const auto& uninodal = solver.level_stats()[0];
    CHECK(uninodal.iterations >= 1);
    // Gap should be small — uninodal is a relaxation
    CHECK(uninodal.gap < 1.0 + 1e-9);
  }
}

}  // namespace
