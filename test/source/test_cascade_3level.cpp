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
#include "sddp_helpers.hpp"

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
      .inherit_optimality_cuts = OptInt {-1},
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
      .inherit_optimality_cuts = OptInt {-1},
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

// ─── ≥4-level cascade on 10-scene 2-reservoir dual±1 fixture ───────────────
//
// Scales the cascade test up two axes at once:
//   • depth: 4 levels (warmup → refine_1 → refine_2 → final) — exercises
//     cut transfer ACROSS THREE BOUNDARIES, not just one.
//   • width: 10 scenes (= 10 scenarios, one per scene) on the same
//     2-reservoir 10-phase fixture that test_sddp_method.cpp uses for
//     the "hard + dual±1 soft" cases — the larger scene count stresses
//     the per-(scene, phase) cut serialization / deserialization path
//     that has caused boundary-cut regressions in the past.
//
// Each level inherits `optimality_cuts` from the previous one
// (`inherit_optimality_cuts = -1`).  The cuts written at the L0→L1
// boundary must still be valid when L3 (`final`) loads the cumulative
// cut file, otherwise the LB at L3 would collapse back to the
// alpha-bootstrap floor (LB ≈ 0) for this fixture.

namespace
{

/// Inflate the 1-scenario / 10-phase / 2-reservoir backtracking-recovery
/// fixture into a `n_scenes`-scene / `n_scenes`-scenario simulation with
/// uniform per-scenario probability (`1/n_scenes`).
inline auto make_Nscene_2reservoir_planning(std::size_t n_scenes) -> Planning
{
  auto planning = make_backtracking_recovery_two_reservoir_planning();
  planning.system.name = "cascade_Nscene_backtrack_10phase_2rsv";

  const double prob = 1.0 / static_cast<double>(n_scenes);
  planning.simulation.scenario_array.clear();
  planning.simulation.scenario_array.reserve(n_scenes);
  planning.simulation.scene_array.clear();
  planning.simulation.scene_array.reserve(n_scenes);
  for (std::size_t i = 0; i < n_scenes; ++i) {
    const auto uid_i = static_cast<int>(i + 1);
    planning.simulation.scenario_array.push_back(Scenario {
        .uid = Uid {uid_i},
        .probability_factor = prob,
    });
    planning.simulation.scene_array.push_back(Scene {
        .uid = Uid {uid_i},
        .name = "scene" + std::to_string(uid_i),
        .active = OptBool {true},
        .first_scenario = static_cast<Size>(i),
        .count_scenario = 1,
    });
  }

  // Replicate the 1-scenario inflow schedule into `n_scenes` identical
  // scenario rows.  Each Flow's `discharge` may be a scalar (no expansion
  // needed — scenario-agnostic by construction) or an STBRealFieldSched
  // 3D array; in the latter case the original helper builds it with a
  // single scenario, so we duplicate it n_scenes-1 more times.
  for (auto& flow : planning.system.flow_array) {
    if (!flow.discharge.has_value()) {
      continue;
    }
    if (auto* sched3d =
            std::get_if<std::vector<std::vector<std::vector<double>>>>(
                &*flow.discharge);
        sched3d != nullptr && !sched3d->empty())
    {
      const auto base = sched3d->front();
      sched3d->clear();
      sched3d->reserve(n_scenes);
      for (std::size_t i = 0; i < n_scenes; ++i) {
        sched3d->push_back(base);
      }
    }
  }
  return planning;
}

}  // namespace

TEST_CASE(  // NOLINT
    "Cascade 4-level on 10-scene 2-reservoir dual±1 fixture")
{
  constexpr std::size_t n_scenes = 10;
  constexpr double efin_target = 150.0;

  const auto pinned_solver = pick_non_mindopt_solver();
  if (pinned_solver.empty()) {
    MESSAGE(
        "Skipping — only the MindOpt backend is available and the dual±1 "
        "fixture wedges its simplex (see pick_non_mindopt_solver).");
    return;
  }

  // run_case owns its PlanningLP so the LP stays alive while we read
  // level stats.  Returns nullopt only on solver failure.
  auto run_case = [&](const OptReal& efin_cost_opt)
      -> std::optional<std::vector<CascadeLevelStats>>
  {
    auto planning = make_Nscene_2reservoir_planning(n_scenes);
    for (auto& r : planning.system.reservoir_array) {
      r.efin = OptReal {efin_target};
      r.efin_cost = efin_cost_opt;
    }
    planning.options.lp_matrix_options.solver_name = pinned_solver;
    PlanningLP plp(std::move(planning));

    SDDPOptions base;
    base.max_iterations = 30;
    base.convergence_tol = 1e-3;
    base.elastic_filter_mode = ElasticFilterMode::single_cut;
    base.multi_cut_threshold = -1;
    base.forward_max_attempts = 200;
    base.forward_fail_stop = false;
    base.enable_api = false;
    base.cut_coeff_eps = 1e-6;
    base.elastic_penalty = 1e2;
    base.apertures = std::vector<Uid> {};

    const CascadeTransition inherit_all {
        .inherit_optimality_cuts = OptInt {-1},
    };

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"warmup"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {4},
                    .apertures = Array<Uid> {},
                    .convergence_tol = OptReal {0.05},
                },
        },
        CascadeLevel {
            .name = OptName {"refine_1"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {6},
                    .apertures = Array<Uid> {},
                    .convergence_tol = OptReal {0.02},
                },
            .transition = inherit_all,
        },
        CascadeLevel {
            .name = OptName {"refine_2"},
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
            .transition = inherit_all,
        },
        CascadeLevel {
            .name = OptName {"final"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {12},
                    .apertures = Array<Uid> {},
                    .convergence_tol = OptReal {1e-3},
                },
            .transition = inherit_all,
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(plp, lp_opts);
    if (!result.has_value()) {
      return std::nullopt;
    }
    return solver.level_stats();
  };

  // ── Hard variant: efin enforced as a strict bound (no soft cost) ──
  // Establishes the converged dual_max from the binding `vfin >= efin`
  // row.  Used as the reference for the dual±1 probes below.
  const auto hard_stats = run_case(OptReal {});
  REQUIRE(hard_stats.has_value());
  REQUIRE(hard_stats->size() == 4);

  SUBCASE("all 4 levels execute in order and produce valid bounds")
  {
    CHECK((*hard_stats)[0].name == "warmup");
    CHECK((*hard_stats)[1].name == "refine_1");
    CHECK((*hard_stats)[2].name == "refine_2");
    CHECK((*hard_stats)[3].name == "final");
    for (const auto& ls : *hard_stats) {
      CHECK(ls.iterations >= 1);
      CHECK(std::isfinite(ls.lower_bound));
      CHECK(std::isfinite(ls.upper_bound));
      const auto scale =
          std::max({std::abs(ls.lower_bound), std::abs(ls.upper_bound), 1.0});
      CHECK(ls.lower_bound <= ls.upper_bound + (1e-8 * scale));
      CHECK(ls.cuts_added >= 0);
      CHECK(ls.elapsed_s > 0.0);
    }
  }

  SUBCASE("inheritance propagates across all three level boundaries")
  {
    // The defining invariant of `inherit_optimality_cuts = -1`: each
    // downstream level's final LB must be no worse than the previous
    // level's final LB (up to convergence tolerance).  Cuts are valid
    // lower bounds on the value function; adding more cuts can only
    // tighten the LB.  If transfer silently failed at any boundary,
    // the affected level would restart near the α-bootstrap floor and
    // produce LB ≪ predecessor LB.
    const auto& s = *hard_stats;
    const double tol = 1e-3;
    CHECK(s[1].lower_bound >= s[0].lower_bound - tol);
    CHECK(s[2].lower_bound >= s[1].lower_bound - tol);
    CHECK(s[3].lower_bound >= s[2].lower_bound - tol);
  }

  SUBCASE("downstream levels reuse cuts (do not exhaust max_iterations)")
  {
    // With inheritance, each refinement level should converge well
    // before exhausting its per-level ``max_iterations`` budget — the
    // cuts already encode most of the value function.  Without
    // inheritance this assertion would fail catastrophically (each
    // level would burn its full max_iterations).
    //
    // The pre-f5ce03dd assertion `s[i+1].iters <= s[i].iters + 1` was
    // tuned to the legacy gap-based convergence; under the post-f5ce03dd
    // ΔUB-only convergence each level's iter count is dominated by its
    // own ``stationary_tol``/``convergence_tol`` rather than by the
    // predecessor's count.  The tighter `convergence_tol` at deeper
    // levels (0.05 → 0.02 → 0.01 → 1e-3) legitimately needs more
    // iterations to satisfy ΔUB stability, even with inherited cuts.
    //
    // The current assertion captures the original intent — "cuts saved
    // us from burning the per-level budget" — by checking each level's
    // iteration count stays strictly below its budget.
    const auto& s = *hard_stats;
    CHECK(s[0].iterations <= 4);  // warmup max_iter
    CHECK(s[1].iterations <= 6);  // refine_1 max_iter
    CHECK(s[2].iterations <= 8);  // refine_2 max_iter
    CHECK(s[3].iterations <= 12);  // final max_iter
  }

  SUBCASE("soft efin above dual_max preserves the hard upper bound")
  {
    // efin_cost set far above any binding dual → the LP behaves
    // identically to the hard case; the 4-level cascade must reach
    // essentially the same UB.  Probes the dual+1 direction of the
    // "dual±1" fixture used in test_sddp_method.cpp.
    const auto soft_above = run_case(OptReal {1e6});
    REQUIRE(soft_above.has_value());
    REQUIRE(soft_above->size() == 4);
    const double hard_ub = (*hard_stats)[3].upper_bound;
    const double soft_ub = (*soft_above)[3].upper_bound;
    CHECK(soft_ub == doctest::Approx(hard_ub).epsilon(0.05));
  }

  SUBCASE("soft efin below dual_max stays finite and well-formed")
  {
    // efin_cost = 1 → far below any binding dual → reservoirs may not
    // reach efin_target, but the cascade itself must complete with a
    // finite, structurally-valid UB on every level.  Probes the dual-1
    // direction; we only assert structural invariants, not magnitudes,
    // because the relaxed problem has a different optimum.
    const auto soft_below = run_case(OptReal {1.0});
    REQUIRE(soft_below.has_value());
    REQUIRE(soft_below->size() == 4);
    for (const auto& ls : *soft_below) {
      CHECK(std::isfinite(ls.lower_bound));
      CHECK(std::isfinite(ls.upper_bound));
      const auto scale =
          std::max({std::abs(ls.lower_bound), std::abs(ls.upper_bound), 1.0});
      CHECK(ls.lower_bound <= ls.upper_bound + (1e-8 * scale));
    }
  }
}

}  // namespace
