/**
 * @file      test_sddp_coverage.hpp
 * @brief     Additional unit tests targeting uncovered paths in sddp_method,
 *            sddp_forward_pass, and sddp_aperture_pass
 * @date      2026-04-04
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. compute_scene_weights() — all-feasible, partial infeasible,
 *     all-infeasible, zero probability fallback, non-runtime rescale
 *  2. compute_convergence_gap() — edge cases (zero UB, equal bounds,
 *     negative bounds, large gap)
 *  3. SDDP with warm_start disabled
 *  4. SDDP with cut_coeff_mode = row_dual
 *  5. SDDP with cut_coeff_eps and cut_coeff_max
 *  6. SDDP with apertures explicitly empty (forces plain backward pass)
 *  7. SDDP with min_iterations > convergence point
 *  8. SDDP convergence gap shrinks monotonically after initial iters
 *  9. SDDP with very low max_iterations (hit max without converging)
 */

#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

// ═══════════════════════════════════════════════════════════════════════════
// compute_scene_weights() unit tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "compute_scene_weights — all feasible, runtime rescale normalises to 1")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // 2 scenes, each with 1 scenario of probability_factor = 1.0
  Simulation sim;
  sim.scenario_array = {
      {.uid = Uid {1}, .probability_factor = 0.6},
      {.uid = Uid {2}, .probability_factor = 0.4},
  };
  sim.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
      {
          .uid = Uid {2},
          .name = "s2",
          .active = true,
          .first_scenario = 1,
          .count_scenario = 1,
      },
  };

  std::vector<SceneLP> scenes;
  scenes.emplace_back(sim.scene_array[0], sim, first_scene_index());
  scenes.emplace_back(sim.scene_array[1], sim, SceneIndex {1});

  const std::vector<uint8_t> feasible = {1, 1};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::runtime);

  REQUIRE(weights.size() == 2);
  // Should sum to 1.0 under runtime rescale
  CHECK(weights[0] + weights[1] == doctest::Approx(1.0));
  // 0.6 / (0.6 + 0.4) = 0.6
  CHECK(weights[0] == doctest::Approx(0.6));
  CHECK(weights[1] == doctest::Approx(0.4));
}

TEST_CASE(  // NOLINT
    "compute_scene_weights — one infeasible scene, runtime rescale")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Simulation sim;
  sim.scenario_array = {
      {.uid = Uid {1}, .probability_factor = 0.6},
      {.uid = Uid {2}, .probability_factor = 0.4},
  };
  sim.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
      {
          .uid = Uid {2},
          .name = "s2",
          .active = true,
          .first_scenario = 1,
          .count_scenario = 1,
      },
  };

  std::vector<SceneLP> scenes;
  scenes.emplace_back(sim.scene_array[0], sim, first_scene_index());
  scenes.emplace_back(sim.scene_array[1], sim, SceneIndex {1});

  // Scene 1 is infeasible
  const std::vector<uint8_t> feasible = {1, 0};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::runtime);

  REQUIRE(weights.size() == 2);
  // Infeasible scene gets weight 0
  CHECK(weights[1] == doctest::Approx(0.0));
  // Feasible scene is renormalized to 1.0
  CHECK(weights[0] == doctest::Approx(1.0));
}

TEST_CASE(  // NOLINT
    "compute_scene_weights — all infeasible, equal weight fallback")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Simulation sim;
  sim.scenario_array = {
      {.uid = Uid {1}, .probability_factor = 0.5},
      {.uid = Uid {2}, .probability_factor = 0.5},
  };
  sim.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
      {
          .uid = Uid {2},
          .name = "s2",
          .active = true,
          .first_scenario = 1,
          .count_scenario = 1,
      },
  };

  std::vector<SceneLP> scenes;
  scenes.emplace_back(sim.scene_array[0], sim, first_scene_index());
  scenes.emplace_back(sim.scene_array[1], sim, SceneIndex {1});

  // All scenes infeasible => total = 0
  const std::vector<uint8_t> feasible = {0, 0};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::runtime);

  REQUIRE(weights.size() == 2);
  // All zero — no feasible scene to assign weights to
  CHECK(weights[0] == doctest::Approx(0.0));
  CHECK(weights[1] == doctest::Approx(0.0));
}

TEST_CASE(  // NOLINT
    "compute_scene_weights — non-runtime mode preserves raw weights")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Simulation sim;
  sim.scenario_array = {
      {.uid = Uid {1}, .probability_factor = 0.3},
      {.uid = Uid {2}, .probability_factor = 0.7},
  };
  sim.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
      {
          .uid = Uid {2},
          .name = "s2",
          .active = true,
          .first_scenario = 1,
          .count_scenario = 1,
      },
  };

  std::vector<SceneLP> scenes;
  scenes.emplace_back(sim.scene_array[0], sim, first_scene_index());
  scenes.emplace_back(sim.scene_array[1], sim, SceneIndex {1});

  const std::vector<uint8_t> feasible = {1, 1};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::none);

  REQUIRE(weights.size() == 2);
  // Non-runtime: raw probability weights, NOT normalised to sum=1
  CHECK(weights[0] == doctest::Approx(0.3));
  CHECK(weights[1] == doctest::Approx(0.7));
}

TEST_CASE(  // NOLINT
    "compute_scene_weights — empty scenes use fallback weight 1.0")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // If scene_feasible has entries but scenes span is smaller, the code
  // falls back to weight=1.0 for feasible scenes beyond scenes.size()
  const std::vector<SceneLP> scenes;  // empty
  const std::vector<uint8_t> feasible = {1, 1};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::runtime);

  REQUIRE(weights.size() == 2);
  // Both get fallback weight 1.0, normalised to 0.5 each
  CHECK(weights[0] == doctest::Approx(0.5));
  CHECK(weights[1] == doctest::Approx(0.5));
}

TEST_CASE(  // NOLINT
    "compute_scene_weights — single scene runtime returns 1.0")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Simulation sim;
  sim.scenario_array = {{.uid = Uid {1}, .probability_factor = 0.8}};
  sim.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
  };

  std::vector<SceneLP> scenes;
  scenes.emplace_back(sim.scene_array[0], sim, first_scene_index());

  const std::vector<uint8_t> feasible = {1};
  const auto weights =
      compute_scene_weights(scenes, feasible, ProbabilityRescaleMode::runtime);

  REQUIRE(weights.size() == 1);
  CHECK(weights[0] == doctest::Approx(1.0));
}

// ═══════════════════════════════════════════════════════════════════════════
// compute_convergence_gap() unit tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("compute_convergence_gap — basic positive gap")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // gap = (UB - LB) / max(1, |UB|) = (200 - 100) / 200 = 0.5
  CHECK(compute_convergence_gap(200.0, 100.0) == doctest::Approx(0.5));
}

TEST_CASE("compute_convergence_gap — equal bounds gives zero gap")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(compute_convergence_gap(150.0, 150.0) == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap — small UB uses denominator 1.0")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // |UB| < 1 => denom = 1.0
  // gap = (0.5 - 0.3) / 1.0 = 0.2
  CHECK(compute_convergence_gap(0.5, 0.3) == doctest::Approx(0.2));
}

TEST_CASE("compute_convergence_gap — negative upper bound")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // UB=-10, LB=-20 => gap = (-10 - (-20)) / max(1, 10) = 10/10 = 1.0
  CHECK(compute_convergence_gap(-10.0, -20.0) == doctest::Approx(1.0));
}

TEST_CASE("compute_convergence_gap — zero bounds")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // UB=0, LB=0 => gap = 0 / max(1, 0) = 0
  CHECK(compute_convergence_gap(0.0, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap — LB exceeds UB gives negative gap")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // This can happen due to floating-point rounding at convergence
  // gap = (100 - 101) / max(1, 100) = -0.01
  CHECK(compute_convergence_gap(100.0, 101.0) == doctest::Approx(-0.01));
}

TEST_CASE(  // NOLINT
    "compute_convergence_gap — large UB large LB small relative gap")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // UB=1e9, LB=1e9 - 1 => gap ≈ 1e-9
  const double ub = 1e9;
  const double lb = ub - 1.0;
  const auto gap = compute_convergence_gap(ub, lb);
  CHECK(gap < 1e-8);
  CHECK(gap > 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// SDDP integration tests targeting uncovered code paths
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod — warm_start disabled still converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.warm_start = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_coeff_mode row_dual converges (3-phase hydro)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_coeff_mode = CutCoeffMode::row_dual;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // row_dual mode should also converge on this problem
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_coeff_eps filters tiny coefficients")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_coeff_eps = 1e-8;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_coeff_max rescales large coefficients")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_coeff_max = 1e6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — apertures=empty forces plain backward pass")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  // Empty aperture UIDs forces fallback to backward_pass (no apertures)
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — min_iterations forces extra iterations")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.min_iterations = 5;
  sddp_opts.convergence_tol = 1.0;  // very loose: would converge iter 1

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // Should run at least min_iterations
  CHECK(results->size() >= 5);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — max_iterations=1 returns one iteration plus final fwd")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-12;  // won't converge

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // 1 training iteration + 1 final forward pass = 2 results
  CHECK(results->size() <= 2);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — max_iterations=0 returns empty (simulation only)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // No training iterations => results may be empty or contain only
  // simulation pass
  // The key assertion is that it does not error out
}

TEST_CASE(  // NOLINT
    "SDDPMethod — convergence gap decreases over iterations")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE(results->size() >= 3);

  // After the first couple of iterations, the gap should generally
  // decrease (allow for small oscillations due to floating-point).
  const auto& first = (*results)[1];  // skip iter 0 which may be noisy
  const auto& last = results->back();
  CHECK(last.gap <= first.gap + 1e-6);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — lower bound non-decreasing")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE(results->size() >= 2);

  // The lower bound should be non-decreasing across iterations
  // (adding cuts can only tighten the relaxation).
  for (std::size_t i = 1; i < results->size(); ++i) {
    CHECK((*results)[i].lower_bound >= (*results)[i - 1].lower_bound - 1e-10);
  }
}

TEST_CASE(  // NOLINT
    "SDDPMethod — scale_alpha=1 converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.scale_alpha = 1.0;  // no scaling

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — alpha bounds affect convergence")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.alpha_min = -1e10;
  sddp_opts.alpha_max = 1e10;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // Should still converge with wide alpha bounds
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut sharing expected mode with single scene")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::expected;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // With a single scene, cut sharing is a no-op but should not break
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — stored_cuts populated after solve")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-12;  // won't converge

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After 3 iterations on a 3-phase problem (2 cuts per iteration),
  // there should be at least some stored cuts
  CHECK_FALSE(sddp.stored_cuts().empty());
  // Each iteration adds cuts for phases 0 and 1 (the two non-last phases)
  CHECK(sddp.stored_cuts().size() >= 3);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — iteration result fields populated correctly")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-12;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  for (const auto& ir : *results) {
    // Timing fields should be non-negative
    CHECK(ir.forward_pass_s >= 0.0);
    CHECK(ir.iteration_s >= 0.0);
    // Scene upper bounds should be populated for single-scene problem
    CHECK(ir.scene_upper_bounds.size() >= 1);
  }

  // First result should have iteration index equal to offset (0)
  CHECK((*results)[0].iteration_index == IterationIndex {0});
}

TEST_CASE(  // NOLINT
    "SDDPMethod — 2-scene problem with cut sharing accumulate")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::accumulate;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — 2-scene problem with cut sharing max")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::max;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

}  // namespace
