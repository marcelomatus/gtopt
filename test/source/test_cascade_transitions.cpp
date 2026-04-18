/**
 * @file      test_cascade_transitions.cpp
 * @brief     Unit tests for CascadePlanningMethod level aperture/transition
 * semantics
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 */

// SPDX-License-Identifier: BSD-3-Clause

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_options_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Aperture semantics in cascade levels ──────────────────────────────────

TEST_CASE("Cascade level aperture semantics")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SUBCASE("empty apertures = pure Benders")
  {
    SDDPOptions base;
    base.max_iterations = 5;
    base.convergence_tol = 0.01;
    // base has no apertures (nullopt)

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"benders_only"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {3},
                    .apertures = Array<Uid> {},  // empty = no apertures
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
  }

  SUBCASE("nullopt apertures inherits from base/per-phase")
  {
    SDDPOptions base;
    base.max_iterations = 5;
    base.convergence_tol = 0.01;
    base.apertures = std::vector<Uid> {};  // base = no apertures

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"inherit_apertures"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {3},
                    // apertures absent (nullopt) → uses base setting
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
  }
}

// ─── Multi-level with transitions ──────────────────────────────────────────

TEST_CASE("Cascade 2-level with target inheritance")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"sddp_with_targets"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.10},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("results from both levels")
  {
    // 2 levels: (up to 3 + 1 final fwd) + (up to 5 + 1 final fwd) = 10
    CHECK(solver.all_results().size() >= 2);
    CHECK(solver.all_results().size() <= 10);
  }
}

// ─── Multi-level with cut inheritance ──────────────────────────────────────

TEST_CASE("Cascade 2-level with optimality cut inheritance")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(!solver.all_results().empty());
}

// ─── LP reuse when model_options is absent ─────────────────────────────────

TEST_CASE("Cascade reuses LP when model_options absent")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"initial"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"reuse_lp"},
          // No model_options → reuses previous LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(solver.all_results().size() >= 2);
}

// ─── 3-level cascade with mixed transitions ───────────────────────────────

TEST_CASE("Cascade 3-level mixed transitions")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.sddp_options.convergence_tol = OptReal {0.01};
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"fast_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"guided_benders"},
          // No model_options → reuse LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {0.5},
                  .target_penalty = OptReal {300.0},
              },
      },
      CascadeLevel {
          .name = OptName {"final_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_penalty = OptReal {200.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  // 3 levels × up to (3 iterations + 1 final fwd) each = up to 12 total
  CHECK(solver.all_results().size() <= 12);
  CHECK(solver.all_results().size() >= 3);
}

// ─── Cascade with 5-phase reservoir and cut+target transitions ─────────────

TEST_CASE("Cascade 5-phase with dual threshold cut filter")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_5phase_reservoir_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"filtered_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_penalty = OptReal {500.0},
                  .optimality_dual_threshold = OptReal {0.001},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(!solver.all_results().empty());
}

// ─── Cascade global convergence_tol as default ─────────────────────────────

TEST_CASE("Cascade global convergence_tol applies to all levels")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-6;  // very tight
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.sddp_options.convergence_tol =
      OptReal {0.5};  // very loose global override
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
                  // No per-level convergence_tol → uses cascade global 0.5
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  // Loose tolerance should converge fast
  CHECK(solver.all_results().size() < 10);
}

}  // anonymous namespace
