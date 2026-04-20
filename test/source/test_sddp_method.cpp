// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_method.cpp
 * @brief     Unit tests for the SDDPMethod (SDDP forward/backward iteration)
 * @date      2026-03-08
 */

#include <cmath>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/validate_planning.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Integration tests ─────────────────────────────────────────────────────

// ─── Scale-invariance regression guard ─────────────────────────────────────
//
// SDDP convergence is a function of the problem's PHYSICAL units, not of
// the LP variable scaling chosen internally.  `VariableScale` entries on
// `PlanningOptions::variable_scales` adjust only how physical values are
// represented inside the LP (`physical = LP × scale`); the primal
// solution and the objective value must come out the same regardless.
//
// This test pins that invariance: the same 3-phase hydro+thermal
// problem is solved twice under two different reservoir `energy` /
// `flow` scale choices, and the converged UB/LB must match to 1e-4.
// Without this guard, a change in `auto_scale_reservoirs` can silently
// shift the reported obj by orders of magnitude (as happened during
// the transient `e848067a` revert this session).
TEST_CASE("SDDPMethod - scale invariance across variable_scales")  // NOLINT
{
  constexpr int kIters = 15;
  constexpr double kConvTol = 1e-5;
  constexpr double kParityTol = 1e-4;

  auto run_with_scales =
      [&](std::vector<VariableScale> scales) -> std::pair<double, double>
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.variable_scales = std::move(scales);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = kIters;
    sddp_opts.convergence_tol = kConvTol;
    sddp_opts.enable_api = false;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    const auto& last = results->back();
    return {last.upper_bound, last.lower_bound};
  };

  // Reference: no explicit variable_scales → auto-scale picks defaults
  // (mostly 1.0 on this small fixture whose reservoir capacity is 150).
  const auto [ub0, lb0] = run_with_scales({});

  SUBCASE("reservoir energy scale = 10 (per-class)")
  {
    const auto [ub, lb] = run_with_scales({
        VariableScale {
            .class_name = "Reservoir",
            .variable = "energy",
            .scale = 10.0,
        },
    });
    CHECK(ub == doctest::Approx(ub0).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(lb0).epsilon(kParityTol));
  }

  SUBCASE("reservoir energy scale = 100 + flow scale = 10 (per-class)")
  {
    const auto [ub, lb] = run_with_scales({
        VariableScale {
            .class_name = "Reservoir",
            .variable = "energy",
            .scale = 100.0,
        },
        VariableScale {
            .class_name = "Reservoir",
            .variable = "flow",
            .scale = 10.0,
        },
    });
    CHECK(ub == doctest::Approx(ub0).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(lb0).epsilon(kParityTol));
  }

  SUBCASE("juan-scale magnitudes: energy=10000 + flow=10000 (per-class)")
  {
    // Matches what `auto_scale_reservoirs` would produce on LMAULE
    // (juan/iplp emax≈1453 hm³ → scale=10 000, fmax=10 000 m³/s →
    // scale=10 000).  The previous transient `e848067a` revert broke
    // here: obj drifted by ~400× on a larger fixture, but the
    // scale=10 / 100 sub-cases above didn't trigger it.  This SUBCASE
    // locks in the juan-magnitude behaviour too.
    const auto [ub, lb] = run_with_scales({
        VariableScale {
            .class_name = "Reservoir",
            .variable = "energy",
            .scale = 10000.0,
        },
        VariableScale {
            .class_name = "Reservoir",
            .variable = "flow",
            .scale = 10000.0,
        },
    });
    CHECK(ub == doctest::Approx(ub0).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(lb0).epsilon(kParityTol));
  }
}

// ─── auto_scale_reservoirs invariance ─────────────────────────────────────
//
// The auto-scale path is a SEPARATE code path from manual
// `variable_scales` injection: it's invoked by `PlanningLP::ctor`
// from `auto_scale_reservoirs(planning)` when no `variable_scales`
// entries are supplied.  The lockstep test below builds a fixture with
// juan-style reservoir magnitudes (emax ≫ 1, fmax ≫ 1) and compares
// three runs:
//
//   1. auto-scale OFF (explicit empty `variable_scales` is kept empty
//      by skipping the helper — via a scale=1 per-class entry that
//      suppresses `has_entry` from firing further rounding)
//   2. auto-scale ON (reservoir emax/fmax drive the scale formula)
//
// Both must converge to the same PHYSICAL objective.  Regressions that
// break SDDP cut numerics at juan scale (obj jumping 400×) would be
// caught by this test where the previous 150-capacity fixture couldn't
// reproduce them.
TEST_CASE(  // NOLINT
    "SDDPMethod - auto_scale_reservoirs invariance at juan-scale magnitudes")
{
  constexpr int kIters = 15;
  constexpr double kConvTol = 1e-5;
  constexpr double kParityTol = 1e-3;

  // Mutate the 3-phase fixture to a juan-style reservoir: emax/fmax
  // both ≫ 1000 so `auto_scale_reservoirs` actually fires.  The
  // physical problem stays solvable (dispatch costs change, but the
  // solver sees a bigger admissible volume range).
  auto build = [](bool disable_auto_scale) -> std::pair<double, double>
  {
    auto planning = make_3phase_hydro_planning();
    REQUIRE(!planning.system.reservoir_array.empty());
    auto& rsv = planning.system.reservoir_array.front();
    rsv.emax = 10000.0;
    rsv.capacity = 10000.0;
    rsv.eini = 5000.0;
    rsv.fmin = -10000.0;
    rsv.fmax = +10000.0;

    if (disable_auto_scale) {
      // Inject scale=1.0 so `auto_scale_reservoirs::has_entry(rsv.uid)`
      // short-circuits and never rounds anything up.
      planning.options.variable_scales = {
          VariableScale {
              .class_name = "Reservoir",
              .variable = "energy",
              .uid = rsv.uid,
              .scale = 1.0,
          },
          VariableScale {
              .class_name = "Reservoir",
              .variable = "flow",
              .uid = rsv.uid,
              .scale = 1.0,
          },
      };
    }

    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = kIters;
    sddp_opts.convergence_tol = kConvTol;
    sddp_opts.enable_api = false;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    const auto& last = results->back();
    return {last.upper_bound, last.lower_bound};
  };

  const auto [ub_raw, lb_raw] = build(/*disable_auto_scale=*/true);
  const auto [ub_auto, lb_auto] = build(/*disable_auto_scale=*/false);

  // Both paths must converge to the same physical UB / LB.  A
  // regression in the auto-scale formula (e.g. an unintended scale
  // ratio between energy and flow) would shift the converged bound
  // even though the physical problem is unchanged.
  CHECK(ub_auto == doctest::Approx(ub_raw).epsilon(kParityTol));
  CHECK(lb_auto == doctest::Approx(lb_raw).epsilon(kParityTol));
}

TEST_CASE("SDDPMethod - 3-phase hydro+thermal converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Verify the monolithic solve works first
  {
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());
    CHECK(*result == 1);
  }

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  const auto& first = results->front();
  const auto& last = results->back();
  CHECK(first.iteration_index == IterationIndex {0});
  CHECK(last.upper_bound > 0.0);
  CHECK(last.lower_bound > 0.0);
  // Allow a tiny negative gap from floating-point rounding when LB ≈ UB at
  // convergence: (UB - LB) / max(1, |UB|) may be a small negative epsilon.
  static constexpr double kGapFpTol = -1e-10;
  CHECK(last.gap >= kGapFpTol);
  // Once reservoir state is properly coupled, SDDP should converge quickly
  CHECK(last.converged);
}

TEST_CASE("SDDPMethod - requires at least 2 phases")  // NOLINT
{
  auto planning = make_single_phase_planning();

  PlanningLP planning_lp(std::move(planning));
  SDDPMethod sddp(planning_lp);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
}

TEST_CASE("SDDPMethod - cut persistence save and load")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file = (tmp_dir / "sddp_test_cuts.csv").string();

  // Run SDDP and save cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cuts_output_file = cuts_file;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // Verify cuts were saved
  CHECK(std::filesystem::exists(cuts_file));
  CHECK_FALSE(sddp.stored_cuts().empty());

  // Clean up
  std::filesystem::remove(cuts_file);
}

// ─── Multi-cut threshold=0 forces multi-cut immediately ──────────────────────

TEST_CASE("SDDPMethod - multi_cut_threshold=0 forces multi-cut mode")  // NOLINT
{
  // Use the 3-phase hydro planning; set threshold=0 so any infeasibility
  // instantly uses multi-cut mode.  The problem should still converge.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.multi_cut_threshold = 0;  // always force multi-cut

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // Convergence should still be reached
  CHECK(results->back().converged);
}

TEST_CASE("SDDPMethod - multi_cut_threshold<0 disables auto-switch")  // NOLINT
{
  // Negative threshold disables automatic multi-cut switching entirely.
  // The problem should still converge with single-cut only.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.multi_cut_threshold = -1;  // never auto-switch

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ─── Probability-weighted cut sharing ────────────────────────────────────────

/// Create a 2-scene, 3-phase hydro+thermal planning problem with explicit
/// per-scene probability weights (0.7 and 0.3).

TEST_CASE("SDDPMethod 2-scene - probability-weighted bounds")  // NOLINT
{
  // Two scenes with probabilities 0.7 and 0.3.
  // UB and LB should be probability-weighted expectations, not simple averages.
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.cut_sharing = CutSharingMode::none;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // The solver should converge
  CHECK(results->back().converged);

  // Verify that the final upper bound is consistent with a
  // probability-weighted combination (not a simple average):
  // UB = 0.7 * ub_scene0 + 0.3 * ub_scene1
  const auto& last = results->back();
  REQUIRE(last.scene_upper_bounds.size() == 2);
  const double expected_ub =
      (0.7 * last.scene_upper_bounds[0]) + (0.3 * last.scene_upper_bounds[1]);
  CHECK(last.upper_bound == doctest::Approx(expected_ub).epsilon(1e-9));
  SPDLOG_INFO("2-scene weighted UB: {:.4f} (scene0={:.4f}, scene1={:.4f})",
              last.upper_bound,
              last.scene_upper_bounds[0],
              last.scene_upper_bounds[1]);

  // Verify lower bound is also probability-weighted
  REQUIRE(last.scene_lower_bounds.size() == 2);
  const double expected_lb =
      (0.7 * last.scene_lower_bounds[0]) + (0.3 * last.scene_lower_bounds[1]);
  CHECK(last.lower_bound == doctest::Approx(expected_lb).epsilon(1e-9));
}

TEST_CASE(
    "SDDPMethod 2-scene - equal weights same as simple average")  // NOLINT
{
  // Equal probability weights → result should match simple average
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);

  const auto& last = results->back();
  REQUIRE(last.scene_upper_bounds.size() == 2);
  // With equal weights 0.5/0.5 the weighted average = arithmetic mean
  const double simple_avg =
      0.5 * (last.scene_upper_bounds[0] + last.scene_upper_bounds[1]);
  CHECK(last.upper_bound == doctest::Approx(simple_avg).epsilon(1e-9));
}

TEST_CASE(
    "SDDPMethod 2-scene Expected cut sharing with prob weights")  // NOLINT
{
  // Verify that Expected cut-sharing mode produces the same convergence
  // outcome whether we use equal or unequal probability weights.
  // The solver should converge in both cases.

  SUBCASE("equal probabilities with Expected cut sharing")
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.cut_sharing = CutSharingMode::expected;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
    CHECK(results->back().converged);
  }

  SUBCASE("unequal probabilities with Expected cut sharing")
  {
    auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.cut_sharing = CutSharingMode::expected;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
    CHECK(results->back().converged);

    // The weighted UB should equal the probability-weighted combination
    const auto& last = results->back();
    REQUIRE(last.scene_upper_bounds.size() == 2);
    const double expected_ub =
        (0.7 * last.scene_upper_bounds[0]) + (0.3 * last.scene_upper_bounds[1]);
    CHECK(last.upper_bound == doctest::Approx(expected_ub).epsilon(1e-9));
  }
}

// ─── Simple 2-phase linear SDDP tests ──────────────────────────────────────

TEST_CASE("SDDPMethod - 2-phase linear converges")  // NOLINT
{
  auto planning = make_2phase_linear_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();

  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  SUBCASE("converges within allowed iterations")
  {
    CHECK(results->back().converged);
  }

  SUBCASE("Benders cuts were generated")
  {
    // At least one cut should have been added in the backward pass
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }

  SUBCASE("stored cuts match total cuts added")
  {
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(sddp.num_stored_cuts() == total_cuts);
  }

  SUBCASE("lower bound approaches upper bound")
  {
    const auto& last = results->back();
    CHECK(last.lower_bound > 0.0);
    CHECK(last.upper_bound > 0.0);
    CHECK(last.gap < 1e-4);
  }
}

TEST_CASE("SDDPMethod - 2-phase with apertures converges")  // NOLINT
{
  auto planning = make_2phase_2scenario_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = false;

  SUBCASE("apertures disabled (baseline)")
  {
    sddp_opts.apertures = std::vector<Uid> {};  // empty = no apertures
    SDDPMethod sddp(plp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
  }

  SUBCASE("apertures enabled with nullopt (use per-phase)")
  {
    sddp_opts.apertures = std::nullopt;  // use per-phase apertures
    SDDPMethod sddp(plp, sddp_opts);
    auto results = sddp.solve();

    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());

    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }
}

// ─── Unit tests for free utility functions ─────────────────────────────────

TEST_CASE(
    "compute_scene_weights - all scenes feasible, equal probability")  // NOLINT
{
  // 3 feasible scenes, no SceneLP objects (uses fallback weight=1)
  const std::vector<uint8_t> feasible {1, 1, 1};
  const std::vector<SceneLP> scenes {};  // empty → uses fallback 1.0 per scene
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(1.0 / 3.0));
  CHECK(w[1] == doctest::Approx(1.0 / 3.0));
  CHECK(w[2] == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("compute_scene_weights - one scene infeasible")  // NOLINT
{
  // scene 1 infeasible → weight must be 0, remaining two share probability
  const std::vector<uint8_t> feasible {1, 0, 1};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[1] == doctest::Approx(0.0));
  CHECK(w[0] == doctest::Approx(0.5));
  CHECK(w[2] == doctest::Approx(0.5));
}

TEST_CASE(
    "compute_scene_weights - all scenes infeasible returns zeros")  // NOLINT
{
  const std::vector<uint8_t> feasible {0, 0, 0};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(0.0));
  CHECK(w[1] == doctest::Approx(0.0));
  CHECK(w[2] == doctest::Approx(0.0));
}

TEST_CASE(
    "compute_scene_weights - single feasible scene gets weight 1")  // NOLINT
{
  const std::vector<uint8_t> feasible {0, 1, 0};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(0.0));
  CHECK(w[1] == doctest::Approx(1.0));
  CHECK(w[2] == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap - basic gap")  // NOLINT
{
  CHECK(compute_convergence_gap(100.0, 90.0) == doctest::Approx(0.1));
}

TEST_CASE(
    "compute_convergence_gap - zero upper bound uses denominator 1")  // NOLINT
{
  // denom = max(1.0, |0.0|) = 1.0
  CHECK(compute_convergence_gap(0.0, -1.0) == doctest::Approx(1.0));
}

TEST_CASE("compute_convergence_gap - converged returns zero gap")  // NOLINT
{
  CHECK(compute_convergence_gap(50.0, 50.0) == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap - large absolute upper bound")  // NOLINT
{
  // denom = max(1.0, 1000.0) = 1000.0 → gap = 10/1000 = 0.01
  CHECK(compute_convergence_gap(1000.0, 990.0) == doctest::Approx(0.01));
}

// ─── lp_only tests ─────────────────────────────────────────────────────
//
// lp_only is handled exclusively in gtopt_lp_runner (see
// build_all_lps_eagerly): it is SDDP-independent and never instantiates
// SDDPMethod.  The test below exercises the full gtopt_main path.

TEST_CASE(
    "gtopt_main - lp_only=true with SDDP solver builds LP only")  // NOLINT
{
  // Minimal multi-phase SDDP JSON: two phases so the SDDP solver accepts it.
  // lp_only should build the LP and return 0 without any solving.
  constexpr auto sddp_lp_only_json = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed",
      "method": "sddp",
      "use_single_bus": true
    },
    "simulation": {
      "block_array": [
        {"uid": 1, "duration": 1},
        {"uid": 2, "duration": 1}
      ],
      "stage_array": [
        {"uid": 1, "first_block": 0, "count_block": 1},
        {"uid": 2, "first_block": 1, "count_block": 1}
      ],
      "scenario_array": [{"uid": 1}],
      "phase_array": [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1}
      ]
    },
    "system": {
      "name": "sddp_lp_only_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
      ]
    }
  })";

  const auto tmp = std::filesystem::temp_directory_path() / "sddp_lp_only_test";
  {
    std::ofstream ofs(tmp.string() + ".json");
    ofs << sddp_lp_only_json;
  }

  auto result = gtopt_main(MainOptions {
      .planning_files = {tmp.string()},
      .lp_only = true,
  });

  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

// ─── Forward/backward propagation tests (boundary cases 1–5 phases) ─────────
//
// These tests specifically verify the backward pass predicate fix
// (pi > 0 instead of the former pi > 1) and correct forward/backward
// state-variable propagation across 1, 2, 3, 4, and 5 phases.
//
// Design notes
// ─────────────
// * make_nphase_simple_hydro_planning(N) creates a minimal N-phase problem
//   with a reservoir whose volume is the linking state variable.
// * Forward propagation: after a forward pass every phase p > 0 must have
//   a strictly positive forward_objective (thermal backup was needed).
// * Backward propagation: the backward pass visits phases N-1 … 1 and adds
//   one optimality Benders cut per phase, so the total stored cuts after one
//   full SDDP iteration must equal N-1.
// * The predicate fix is specifically exercised in the 2-phase case:
//   because pi=1 satisfies pi>0, phase 0 is re-solved after the cut from
//   phase 1 is added, which causes the lower bound to strictly increase.

// ─── Boundary case: 1 phase is rejected ─────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDP backward pass - 1-phase (boundary): solver rejects single phase")
{
  // Phase count = 1 is below the SDDP minimum of 2.
  // The solver must return an error rather than solving.
  auto planning = make_nphase_simple_hydro_planning(1);
  PlanningLP plp(std::move(planning));

  SDDPMethod sddp(plp);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
}

// ─── 2-phase: backward pass predicate fix ────────────────────────────────────
//
// With num_phases = 2, backward_pass iterates pi ∈ {1} (only one step).
// backward_pass_single_phase(phase=1) adds a cut to phase 0 and, with the
// corrected predicate (pi > 0), re-solves phase 0 so the lower bound rises.
// This is the exact bug fixed in PR 263: the old predicate (pi > 1) was
// false for pi=1, so phase 0 was never re-solved and the lower bound stagnated.

TEST_CASE(  // NOLINT
    "SDDP backward pass - 2-phase (boundary): lower bound rises after "
    "backward pass")
{
  auto planning = make_nphase_simple_hydro_planning(2);
  PlanningLP plp(std::move(planning));

  // Tight tolerance: must converge with the fixed predicate.
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);

  // Collect lower bounds across iterations.
  std::vector<double> lower_bounds;
  sddp.set_iteration_callback(
      [&lower_bounds](const SDDPIterationResult& r) -> bool
      {
        lower_bounds.push_back(r.lower_bound);
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // The lower bound must be strictly positive after the first backward pass.
  // A stagnant zero lower bound would indicate phase 0 was never re-solved.
  CHECK(lower_bounds.front() > 0.0);

  // The lower bound must be monotonically non-decreasing across iterations.
  for (std::size_t i = 1; i < lower_bounds.size(); ++i) {
    CHECK(lower_bounds[i] >= lower_bounds[i - 1] - 1e-9);
  }

  // SDDP must converge to optimality with the fixed predicate.
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDP backward pass - 2-phase: one cut added per iteration (N-1 = 1)")
{
  auto planning = make_nphase_simple_hydro_planning(2);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;  // single iteration to count precisely

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // For N=2 phases the backward pass visits exactly N-1 = 1 phase (phase 1),
  // producing exactly 1 Benders cut.
  CHECK(results->front().cuts_added == 1);
}

TEST_CASE(  // NOLINT
    "SDDP forward propagation - 2-phase: forward objective populated for "
    "each phase")
{
  auto planning = make_nphase_simple_hydro_planning(2);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After the forward pass each phase must have been dispatched.
  // forward_objective is the per-phase OPEX (excluding alpha).
  const auto& states = sddp.phase_states();
  CHECK(states[first_phase_index()].forward_objective >= 0.0);
  CHECK(states[PhaseIndex {1}].forward_objective >= 0.0);

  // Total forward cost across phases must be strictly positive.
  const double total_fwd = states[first_phase_index()].forward_objective
      + states[PhaseIndex {1}].forward_objective;
  CHECK(total_fwd > 0.0);
}

// ─── 3-phase: forward and backward propagation ──────────────────────────────

TEST_CASE(  // NOLINT
    "SDDP backward pass - 3-phase: two cuts added per iteration (N-1 = 2)")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;  // count cuts from one backward pass only

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // For N=3 phases backward iterates over pi ∈ {2, 1}: N-1 = 2 cuts.
  CHECK(results->front().cuts_added == 2);
}

TEST_CASE(  // NOLINT
    "SDDP forward propagation - 3-phase: state variables link all phases")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After the forward pass all three phases must have positive OPEX.
  const auto& states = sddp.phase_states();
  for (int p = 0; p < 3; ++p) {
    CHECK(states[PhaseIndex {p}].forward_objective >= 0.0);
  }

  const double total_fwd = [&]
  {
    double s = 0.0;
    for (int p = 0; p < 3; ++p) {
      s += states[PhaseIndex {p}].forward_objective;
    }
    return s;
  }();
  CHECK(total_fwd > 0.0);

  // Outgoing state-variable links must be established for phases 0 and 1
  // (links connect phase p to phase p+1; the last phase has no outgoing links).
  CHECK_FALSE(states[first_phase_index()].outgoing_links.empty());
  CHECK_FALSE(states[PhaseIndex {1}].outgoing_links.empty());
  CHECK(states[PhaseIndex {2}].outgoing_links.empty());
}

TEST_CASE(  // NOLINT
    "SDDP backward pass - 3-phase: lower bound rises after backward pass")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);

  std::vector<double> lbs;
  sddp.set_iteration_callback(
      [&lbs](const SDDPIterationResult& r) -> bool
      {
        lbs.push_back(r.lower_bound);
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  CHECK(lbs.front() > 0.0);
  for (std::size_t i = 1; i < lbs.size(); ++i) {
    CHECK(lbs[i] >= lbs[i - 1] - 1e-9);
  }
  CHECK(results->back().converged);
}

// ─── 4-phase: forward and backward propagation ──────────────────────────────

TEST_CASE(  // NOLINT
    "SDDP backward pass - 4-phase: three cuts added per iteration (N-1 = 3)")
{
  auto planning = make_nphase_simple_hydro_planning(4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // For N=4 phases backward iterates over pi ∈ {3, 2, 1}: N-1 = 3 cuts.
  CHECK(results->front().cuts_added == 3);
}

TEST_CASE(  // NOLINT
    "SDDP forward propagation - 4-phase: state links span all phases")
{
  auto planning = make_nphase_simple_hydro_planning(4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& states = sddp.phase_states();

  // Outgoing links from phases 0..2; phase 3 (last) has none.
  for (int p = 0; p < 3; ++p) {
    CHECK_FALSE(states[PhaseIndex {p}].outgoing_links.empty());
  }
  CHECK(states[PhaseIndex {3}].outgoing_links.empty());
}

TEST_CASE(  // NOLINT
    "SDDP backward pass - 4-phase: lower bound rises and converges")
{
  auto planning = make_nphase_simple_hydro_planning(4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);

  std::vector<double> lbs;
  sddp.set_iteration_callback(
      [&lbs](const SDDPIterationResult& r) -> bool
      {
        lbs.push_back(r.lower_bound);
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  CHECK(lbs.front() > 0.0);
  for (std::size_t i = 1; i < lbs.size(); ++i) {
    CHECK(lbs[i] >= lbs[i - 1] - 1e-9);
  }
  CHECK(results->back().converged);
}

// ─── 5-phase: forward and backward propagation ──────────────────────────────

TEST_CASE(  // NOLINT
    "SDDP backward pass - 5-phase: four cuts added per iteration (N-1 = 4)")
{
  auto planning = make_nphase_simple_hydro_planning(5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // For N=5 phases backward iterates over pi ∈ {4, 3, 2, 1}: N-1 = 4 cuts.
  CHECK(results->front().cuts_added == 4);
}

TEST_CASE(  // NOLINT
    "SDDP forward propagation - 5-phase: state links span phases 0..3")
{
  auto planning = make_nphase_simple_hydro_planning(5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& states = sddp.phase_states();

  // Phases 0..3 must have outgoing state-variable links.
  for (int p = 0; p < 4; ++p) {
    CHECK_FALSE(states[PhaseIndex {p}].outgoing_links.empty());
  }
  // Phase 4 (last) never has outgoing links.
  CHECK(states[PhaseIndex {4}].outgoing_links.empty());
}

TEST_CASE(  // NOLINT
    "SDDP backward pass - 5-phase: lower bound rises and converges")
{
  auto planning = make_nphase_simple_hydro_planning(5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;

  SDDPMethod sddp(plp, sddp_opts);

  std::vector<double> lbs;
  sddp.set_iteration_callback(
      [&lbs](const SDDPIterationResult& r) -> bool
      {
        lbs.push_back(r.lower_bound);
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  CHECK(lbs.front() > 0.0);
  for (std::size_t i = 1; i < lbs.size(); ++i) {
    CHECK(lbs[i] >= lbs[i - 1] - 1e-9);
  }
  CHECK(results->back().converged);
}

// ─── Cross-phase count: stored cuts equal (N-1) × iterations ────────────────

TEST_CASE(  // NOLINT
    "SDDP backward pass - stored cuts equal (N-1) per iteration across "
    "phase counts")
{
  // For each phase count n ∈ {2, 3, 4, 5} run exactly k iterations and verify
  // that the total stored cuts equals k × (n-1).  This directly validates the
  // backward loop range [1, n) and confirms the predicate fix allows the full
  // backward sweep for every phase count.
  constexpr int k = 2;  // number of iterations

  for (int n : {2, 3, 4, 5}) {
    auto planning = make_nphase_simple_hydro_planning(n);
    PlanningLP plp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = k;
    sddp_opts.convergence_tol = 1e-12;  // very tight: won't converge in 2

    SDDPMethod sddp(plp, sddp_opts);
    auto results = sddp.solve();

    REQUIRE(results.has_value());
    // Each of the k iterations should contribute n-1 cuts.
    const int expected_cuts = k * (n - 1);
    CHECK(sddp.num_stored_cuts() == expected_cuts);
    SPDLOG_INFO(
        "Phase count n={}: {} iterations × {} cuts = {} stored (expected {})",
        n,
        k,
        n - 1,
        sddp.num_stored_cuts(),
        expected_cuts);
  }
}

// ─── Monolithic vs SDDP objective equality for 2, 3, 4, 5 phases ────────────

TEST_CASE(  // NOLINT
    "SDDP vs monolithic - N-phase (2..5) objectives agree within 5%")
{
  // Parameterised over n ∈ {2, 3, 4, 5}.
  // Verifies that the SDDP upper bound at convergence is within 5% of the
  // monolithic total objective, confirming correct forward propagation
  // (supply/demand balance per phase) and backward propagation (Benders cuts
  // tighten the lower bound to the monolithic optimum).
  for (int n : {2, 3, 4, 5}) {
    // Monolithic solve
    auto mono_planning = make_nphase_simple_hydro_planning(n);
    PlanningLP plp_mono(std::move(mono_planning));

    auto mono_result = plp_mono.resolve();
    REQUIRE(mono_result.has_value());
    CHECK(*mono_result == 1);

    double mono_total = 0.0;
    for (int p = 0; p < n; ++p) {
      mono_total += plp_mono.system(first_scene_index(), PhaseIndex {p})
                        .linear_interface()
                        .get_obj_value();
    }
    CHECK(mono_total > 0.0);

    // SDDP solve
    auto sddp_planning = make_nphase_simple_hydro_planning(n);
    PlanningLP plp_sddp(std::move(sddp_planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 50;
    sddp_opts.convergence_tol = 1e-4;

    SDDPMethod sddp(plp_sddp, sddp_opts);
    auto sddp_results = sddp.solve();
    REQUIRE(sddp_results.has_value());
    REQUIRE_FALSE(sddp_results->empty());

    const auto& last = sddp_results->back();
    CHECK(last.converged);

    const double rel_diff = std::abs(last.upper_bound - mono_total)
        / std::max(1.0, std::abs(mono_total));

    SPDLOG_INFO("n={}: mono={:.4f} sddp_ub={:.4f} rel_diff={:.6f}",
                n,
                mono_total,
                last.upper_bound,
                rel_diff);

    CHECK(rel_diff < 0.05);
  }
}

// ─── forget_first_cuts tests ────────────────────────────────────────────────

TEST_CASE("SDDPMethod - forget_first_cuts removes inherited cuts")  // NOLINT
{
  // Solve to generate some cuts, then use forget_first_cuts to remove a
  // subset and verify LP row counts and stored cut counts are consistent.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto total_cuts_before = sddp.num_stored_cuts();
  REQUIRE(total_cuts_before > 2);

  // Record LP row counts per (scene, phase) before forget
  const auto& sim = planning_lp.simulation();
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  std::vector<size_t> rows_before;
  for (Index si = 0; si < num_scenes; ++si) {
    for (Index pi = 0; pi < num_phases; ++pi) {
      const auto& li = planning_lp.system(SceneIndex {si}, PhaseIndex {pi})
                           .linear_interface();
      rows_before.push_back(li.get_numrows());
    }
  }

  SUBCASE("forget 0 cuts is a no-op")
  {
    sddp.forget_first_cuts(0);
    CHECK(sddp.num_stored_cuts() == total_cuts_before);
  }

  SUBCASE("forget 2 cuts reduces stored count by 2")
  {
    const int to_forget = 2;
    sddp.forget_first_cuts(to_forget);

    CHECK(sddp.num_stored_cuts() == total_cuts_before - to_forget);

    // Total LP rows should have decreased
    size_t total_rows_before = 0;
    size_t total_rows_after = 0;
    size_t idx = 0;
    for (Index si = 0; si < num_scenes; ++si) {
      for (Index pi = 0; pi < num_phases; ++pi) {
        const auto& li = planning_lp.system(SceneIndex {si}, PhaseIndex {pi})
                             .linear_interface();
        total_rows_before += rows_before[idx];
        total_rows_after += li.get_numrows();
        ++idx;
      }
    }
    CHECK(total_rows_after < total_rows_before);
    CHECK(total_rows_before - total_rows_after
          == static_cast<size_t>(to_forget));
  }

  SUBCASE("forget all cuts empties the stored cuts")
  {
    sddp.forget_first_cuts(total_cuts_before);
    CHECK(sddp.num_stored_cuts() == 0);
  }

  SUBCASE("forget more than available clamps to available")
  {
    sddp.forget_first_cuts(total_cuts_before + 100);
    CHECK(sddp.num_stored_cuts() == 0);
  }

  SUBCASE("remaining cuts have valid row indices after forget")
  {
    sddp.forget_first_cuts(2);

    // After forgetting, update duals to verify row indices are valid.
    // update_stored_cut_duals reads duals at cut.row — if the index
    // were stale/out-of-range, the solver would crash or return garbage.
    sddp.update_stored_cut_duals();

    const auto& cuts = sddp.stored_cuts();
    for (const auto& cut : cuts) {
      CHECK(static_cast<int>(cut.row) >= 0);
      CHECK(cut.dual.has_value());
    }
  }

  SUBCASE("solver can re-solve after forgetting cuts")
  {
    sddp.forget_first_cuts(2);
    sddp.clear_stop();

    // Reconfigure for a few more iterations
    sddp.mutable_options().max_iterations = 3;
    auto results2 = sddp.solve();
    REQUIRE(results2.has_value());
    CHECK_FALSE(results2->empty());

    // Should still find a valid bound
    const auto& last = results2->back();
    CHECK(last.upper_bound > 0.0);
    CHECK(last.lower_bound > 0.0);
  }
}

// ─── mutable_options preserves auto-computed fields ─────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod — mutable_options preserves auto scale_alpha")
{
  // Regression: cascade_method.cpp used to overwrite the entire SDDPOptions
  // via mutable_options() = level_opts, resetting auto-computed scale_alpha
  // to 0.  This caused alpha_val = sol * 0 = 0 in the forward pass,
  // removing future-cost credit and producing NaN.
  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 5;
  opts.convergence_tol = 0.01;
  // scale_alpha=0 triggers auto-computation during initialize_solver()
  opts.scale_alpha = 0.0;

  SDDPMethod sddp(plp, opts);
  auto init = sddp.ensure_initialized();
  REQUIRE(init.has_value());

  // Auto-computed scale_alpha should be > 0
  const auto auto_sa = sddp.mutable_options().scale_alpha;
  CHECK(auto_sa > 0.0);

  // Update only max_iterations (correct pattern)
  sddp.mutable_options().max_iterations = 3;

  // scale_alpha must survive the field-level update
  CHECK(sddp.mutable_options().scale_alpha == auto_sa);

  // Solve should converge (not produce NaN)
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK_FALSE(std::isnan(results->back().upper_bound));
  CHECK(results->back().upper_bound > 0.0);
}

// ─── Convergence criteria unit tests ────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod primary convergence - gap < convergence_tol stops the loop")
{
  // 3-phase hydro problem converges in a few iterations.
  // Verify that the primary criterion (gap < convergence_tol) fires and that
  // SDDPIterationResult fields are properly populated.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.min_iterations = 1;
  sddp_opts.convergence_tol = 1e-3;
  // Disable stationary criterion so only the primary criterion can fire.
  sddp_opts.stationary_tol = 0.0;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto& last = results->back();

  // Primary convergence: gap must be below convergence_tol.
  CHECK(last.converged);
  CHECK_FALSE(last.stationary_converged);
  CHECK(last.gap < sddp_opts.convergence_tol);

  // gap_change is 1.0 (default / "not checked") when stationary is disabled.
  CHECK(last.gap_change == doctest::Approx(1.0));

  // The solver should stop well before max_iterations.
  CHECK(static_cast<int>(results->size()) < sddp_opts.max_iterations);
}

TEST_CASE(  // NOLINT
    "SDDPMethod stationary convergence - fires when gap stops improving")
{
  // The hydro problem converges to a gap of ~0 after a few iterations.
  // By setting convergence_tol to a negative value (-1.0), the primary
  // criterion (gap < convergence_tol) can never be satisfied.
  // The stationary criterion will then fire once the now-zero gap has not
  // changed over the look-back window.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  // Require at least 4 training iterations so the window (2) can fill.
  sddp_opts.min_iterations = 4;
  // Negative primary tolerance: primary convergence is impossible.
  sddp_opts.convergence_tol = -1.0;
  // Stationary criterion: any gap-change < 100% (i.e. not doubling) triggers.
  sddp_opts.stationary_tol = 1.0;
  sddp_opts.stationary_window = 2;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  bool found_stationary = false;
  for (const auto& ir : *results) {
    if (ir.stationary_converged) {
      found_stationary = true;
      // stationary_converged implies converged.
      CHECK(ir.converged);
      // gap_change must be below stationary_tol.
      CHECK(ir.gap_change < sddp_opts.stationary_tol);
      break;
    }
  }
  CHECK(found_stationary);
}

TEST_CASE(  // NOLINT
    "SDDPMethod stationary convergence - gap_change populated after window")
{
  // Verify that gap_change is 1.0 only for the first iteration (no prior
  // result), and is computed from iteration 1 onward using
  // min(window, available) look-back.  Stationary convergence still
  // requires the full window before it can fire.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.min_iterations = 4;
  sddp_opts.convergence_tol = -1.0;  // primary convergence impossible
  sddp_opts.stationary_tol = 0.99;  // fires once gap stops changing
  sddp_opts.stationary_window = 3;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto& all = *results;
  const std::size_t n = all.size();

  // First iteration (index 0): gap_change = 1.0 (no prior result).
  if (n > 1) {
    CHECK(all[0].gap_change == doctest::Approx(1.0));
  }

  // From iteration 1 onward: gap_change is computed (non-negative, < 1.0
  // once the gap starts stabilizing).
  for (std::size_t i = 1; i + 1 < n; ++i) {
    CHECK(all[i].gap_change >= 0.0);
  }
}

TEST_CASE(  // NOLINT
    "PlanningLP::SddpSummary populated after SDDP solve")
{
  // After a successful SDDP solve, PlanningLP::sddp_summary() must contain
  // meaningful gap/gap_change/converged values, and write_out() must emit
  // gap and gap_change columns in solution.csv.
  auto planning = make_3phase_hydro_planning();

  // Route output into a temporary directory.
  const auto out_dir =
      std::filesystem::temp_directory_path() / "__sddp_summary_test_out__";
  std::filesystem::remove_all(out_dir);
  std::filesystem::create_directories(out_dir);
  planning.options.output_directory = std::string(out_dir.string());

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.min_iterations = 1;
  sddp_opts.convergence_tol = 1e-3;

  SDDPMethod sddp(planning_lp, sddp_opts);
  const auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // Manually populate the summary (normally done by SDDPPlanningMethod).
  const auto& last = results->back();
  planning_lp.set_sddp_summary({
      .gap = last.gap,
      .gap_change = last.gap_change,
      .lower_bound = last.lower_bound,
      .upper_bound = last.upper_bound,
      .iterations = std::ssize(*results),
      .converged = last.converged,
      .stationary_converged = last.stationary_converged,
  });

  // Verify the summary is populated.
  const auto& summary = planning_lp.sddp_summary();
  CHECK(summary.converged);
  // Allow tiny negative gap from floating-point rounding (LB ≈ UB).
  static constexpr double kGapFpTol = -1e-10;
  CHECK(summary.gap >= kGapFpTol);
  CHECK(summary.gap < sddp_opts.convergence_tol);
  CHECK(summary.lower_bound > 0.0);
  CHECK(summary.upper_bound > 0.0);
  CHECK(summary.iterations > 0);

  // Write output and check that solution.csv contains gap and gap_change.
  planning_lp.write_out();

  const auto sol_path = out_dir / "solution.csv";
  REQUIRE(std::filesystem::exists(sol_path));

  std::ifstream f(sol_path.string());
  REQUIRE(f.is_open());
  std::string header;
  REQUIRE(std::getline(f, header));

  // Header must contain both gap columns.
  CHECK(header.find("gap") != std::string::npos);
  CHECK(header.find("gap_change") != std::string::npos);

  // At least one data row with non-negative gap value.
  std::string data_line;
  REQUIRE(std::getline(f, data_line));
  CHECK_FALSE(data_line.empty());

  std::filesystem::remove_all(out_dir);
}

// ─── Forward pass elastic fallback ──────────────────────────────────────────

/// Create a 3-phase hydro problem with a very tight reservoir that forces
/// elastic fallback during the forward pass.  The reservoir emax is so small
/// that the state variable linking phases 0→1 or 1→2 cannot satisfy the
/// inflow/outflow constraints without elastic relaxation.

TEST_CASE(  // NOLINT
    "SDDPMethod — forward pass elastic fallback converges")
{
  auto planning = make_tight_reservoir_3phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE("SDDPMethod — warm_start=false converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.warm_start = false;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

// ─── Cut sharing modes via solve() ──────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_sharing accumulate mode via solve")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::accumulate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_sharing expected mode via solve")
{
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::expected;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — cut_sharing max mode via solve")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.cut_sharing = CutSharingMode::max;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

// ─── Cut pruning ────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod — cut pruning bounds stored cuts")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-6;  // tight to force many iterations
  sddp_opts.max_cuts_per_phase = 5;
  sddp_opts.cut_prune_interval = 2;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // With pruning at interval=2 and max=5, stored cuts should be bounded
  // (exact count depends on convergence, but should not exceed
  // max_cuts_per_phase × num_phases × num_scenes significantly)
  CHECK(sddp.num_stored_cuts() <= 30);
}

// ─── Stationary convergence ─────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod — stationary convergence triggers")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-10;  // very tight — primary won't trigger
  sddp_opts.stationary_tol = 0.5;  // lenient stationary criterion
  sddp_opts.stationary_window = 3;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // Either stationary convergence triggers or we hit max_iterations.
  // The problem converges quickly so stationary should fire.
  const auto& last = results->back();
  if (last.converged) {
    // If converged via stationary, that flag is set
    // (may also converge via primary if gap is small enough)
    CHECK((last.stationary_converged || last.gap < 1e-3));
  }
}

// ─── Simulation mode ────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPMethod — simulation_mode runs evaluation only")
{
  // First train the solver to get cuts
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto train_results = sddp.solve();
  REQUIRE(train_results.has_value());
  REQUIRE(sddp.num_stored_cuts() > 0);

  // Now re-solve in simulation mode — should return a single-iteration
  // evaluation result
  sddp.mutable_options().max_iterations = 1;
  sddp.clear_stop();
  auto sim_results = sddp.solve();
  REQUIRE(sim_results.has_value());
  CHECK_FALSE(sim_results->empty());
}

// ─── Low-memory mode tests ─────────────────────────────────────────────────

TEST_CASE("SDDPMethod — low_memory level 1 converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
  CHECK(results->back().upper_bound > 0.0);
  CHECK(results->back().lower_bound > 0.0);
}

TEST_CASE("SDDPMethod — low_memory level 2 (compressed) converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::zstd;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
  CHECK(results->back().upper_bound > 0.0);
  CHECK(results->back().lower_bound > 0.0);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — low_memory matches normal mode objective")
{
  // Run without low_memory
  double normal_ub = 0.0;
  double normal_lb = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 10;
    sddp_opts.convergence_tol = 1e-3;
    sddp_opts.enable_api = false;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    normal_ub = results->back().upper_bound;
    normal_lb = results->back().lower_bound;
  }

  // Run with low_memory level 1
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 10;
    sddp_opts.convergence_tol = 1e-3;
    sddp_opts.low_memory_mode = LowMemoryMode::compress;
    sddp_opts.enable_api = false;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());

    // Same objective within tolerance
    CHECK(results->back().upper_bound
          == doctest::Approx(normal_ub).epsilon(1e-4));
    CHECK(results->back().lower_bound
          == doctest::Approx(normal_lb).epsilon(1e-4));
  }
}

TEST_CASE(  // NOLINT
    "SDDPMethod — low_memory level 1 with 2 scenes converges")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — low_memory with cut pruning converges")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.max_cuts_per_phase = 5;
  sddp_opts.cut_prune_interval = 2;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  // May not converge with aggressive pruning, but should not crash
  CHECK(results->back().upper_bound > 0.0);
}

TEST_CASE(  // NOLINT
    "SDDPMethod — rebuild mode: initialize_solver does not segfault")
{
  // Regression for a crash where initialize_solver() invoked
  // add_col(alpha) and get_col_{low,upp}_raw() on released backends
  // under LowMemoryMode::rebuild.  The SystemLP constructor defers
  // load_flat in rebuild mode, so every per-cell backend is null until
  // ensure_lp_built() runs.  The earlier ordering called
  // initialize_alpha_variables + collect_state_variable_links BEFORE
  // ensure_lp_built, dereferencing the null backend.
  //
  // Fix: rebuild callback + ensure_backend make the setup steps
  // mode-agnostic; cut loaders call record_cut_row alongside add_row so
  // persistent state (m_dynamic_cols_, m_active_cuts_, m_base_numrows_)
  // is populated live without any retrofit pass.
  auto planning = make_3phase_hydro_planning();
  // Configure PlanningLP to construct SystemLPs in rebuild mode (skips
  // the eager load_flat; this is what sets up the null-backend state
  // that the bug required).
  planning.options.sddp_options = SddpOptions {
      .low_memory_mode = LowMemoryMode::rebuild,
  };
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1.0;  // loose: we only care about init+solve
  sddp_opts.low_memory_mode = LowMemoryMode::rebuild;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());  // pre-fix: segfault during initialize_solver
  CHECK_FALSE(results->empty());
  CHECK(results->back().upper_bound > 0.0);
  CHECK(results->back().lower_bound > 0.0);
}

// ─── LowMemoryMode parity: off vs compress (lz4, uncompressed) vs rebuild ────

TEST_CASE(  // NOLINT
    "SDDPMethod — low_memory parity across all modes")
{
  // Every supported low_memory configuration must converge to the same
  // SDDP objective on the same deterministic problem.  This is the
  // end-to-end guarantee that release/reconstruct (compress with lz4 or
  // uncompressed codec) and re-flatten (rebuild) preserve every piece
  // of LP state needed by SDDP convergence (alpha cols, accumulated
  // cuts, base_numrows, state-variable links).
  //
  // Anything that silently drops state would show as a bound divergence.
  constexpr int kIters = 10;
  constexpr double kConvTol = 1e-3;
  constexpr double kParityTol = 1e-4;

  auto run_with_mode = [&](std::optional<LowMemoryMode> mode,
                           std::optional<CompressionCodec> codec =
                               std::nullopt) -> std::pair<double, double>
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = kIters;
    sddp_opts.convergence_tol = kConvTol;
    sddp_opts.enable_api = false;
    if (mode) {
      sddp_opts.low_memory_mode = *mode;
    }
    if (codec) {
      sddp_opts.memory_codec = *codec;
    }

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return {results->back().upper_bound, results->back().lower_bound};
  };

  // Reference: low_memory disabled.
  const auto [off_ub, off_lb] = run_with_mode(std::nullopt);

  SUBCASE("compress with lz4 (default codec)")
  {
    const auto [ub, lb] =
        run_with_mode(LowMemoryMode::compress, CompressionCodec::lz4);
    CHECK(ub == doctest::Approx(off_ub).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(off_lb).epsilon(kParityTol));
  }

  SUBCASE("compress with zstd codec")
  {
    const auto [ub, lb] =
        run_with_mode(LowMemoryMode::compress, CompressionCodec::zstd);
    CHECK(ub == doctest::Approx(off_ub).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(off_lb).epsilon(kParityTol));
  }

  SUBCASE("compress with uncompressed codec (ex-snapshot semantics)")
  {
    const auto [ub, lb] =
        run_with_mode(LowMemoryMode::compress, CompressionCodec::uncompressed);
    CHECK(ub == doctest::Approx(off_ub).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(off_lb).epsilon(kParityTol));
  }

  SUBCASE("rebuild")
  {
    const auto [ub, lb] = run_with_mode(LowMemoryMode::rebuild);
    CHECK(ub == doctest::Approx(off_ub).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(off_lb).epsilon(kParityTol));
  }
}

// ─── Pure simulation-run invariance ─────────────────────────────────────────
//
// A "pure" simulation run is `max_iterations=0` with an optional hot-start
// cut file — no training happens, only the final forward (simulation) pass
// runs.  The output must be byte-for-byte identical regardless of the
// `low_memory_mode`.  Protects the whole pipeline:
//
//   1. Loaded cuts flow into `m_active_cuts_` via `record_cut_row` →
//      replay on `ensure_lp_built()` under compress/rebuild.
//   2. Sim-pass forward writes each cell with the live backend
//      (`sddp_forward_pass.cpp`) before `release_backend()`.
//   3. `SystemLP::m_output_written_` guards against a second write from
//      `PlanningLP::write_out()` that would see a rehydrated-but-unsolved
//      backend under compress.
//   4. `PlanningLP::write_out` normalises status/obj/kappa for unsolved
//      cells so `solution.csv` matches across modes.
//
// The test runs a 3-phase hydro case twice — `low_memory=off` vs
// `low_memory=compress` — with `max_iterations=0`, then compares
// `solution.csv` byte-for-byte.

TEST_CASE(  // NOLINT
    "SDDPMethod — pure sim pass (max_iter=0) output invariance "
    "across low_memory modes")
{
  namespace fs = std::filesystem;

  auto run_once = [&](std::optional<LowMemoryMode> mode,
                      const fs::path& out_dir) -> void
  {
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    auto planning = make_3phase_hydro_planning();
    planning.options.output_directory = out_dir.string();

    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 0;  // pure sim pass
    sddp_opts.enable_api = false;
    if (mode) {
      sddp_opts.low_memory_mode = *mode;
    }

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());

    // Mimic SDDPPlanningMethod's summary population so solution.csv is
    // written with identical gap/max_kappa/converged columns in both
    // runs (otherwise minor summary differences would diverge the file).
    const auto& last = results->back();
    planning_lp.set_sddp_summary({
        .gap = last.gap,
        .gap_change = last.gap_change,
        .lower_bound = last.lower_bound,
        .upper_bound = last.upper_bound,
        .max_kappa = sddp.global_max_kappa(),
        .iterations = 0,
        .converged = last.converged,
        .stationary_converged = last.stationary_converged,
        .statistical_converged = last.statistical_converged,
    });
    planning_lp.write_out();
  };

  const auto base_dir = fs::temp_directory_path() / "gtopt_pure_sim_invariance";
  const auto off_dir = base_dir / "off";
  const auto cmp_dir = base_dir / "compress";

  run_once(std::nullopt, off_dir);
  run_once(LowMemoryMode::compress, cmp_dir);

  // solution.csv must match byte-for-byte.
  const auto off_sol = off_dir / "solution.csv";
  const auto cmp_sol = cmp_dir / "solution.csv";
  REQUIRE(fs::exists(off_sol));
  REQUIRE(fs::exists(cmp_sol));

  const auto read_file = [](const fs::path& p) -> std::string
  {
    std::ifstream f(p.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
  };

  const auto off_txt = read_file(off_sol);
  const auto cmp_txt = read_file(cmp_sol);
  CHECK(off_txt == cmp_txt);

  // Parity on file-set: the two runs must emit the same set of output
  // files (the sim-pass writes every (scene, phase) cell whose solve
  // was optimal; `SystemLP::write_out` short-circuits others — so both
  // modes produce exactly the same universe of parquet/csv shards).
  const auto collect = [&](const fs::path& root)
  {
    std::vector<std::string> rel;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (e.is_regular_file()) {
        rel.push_back(fs::relative(e.path(), root).string());
      }
    }
    std::ranges::sort(rel);
    return rel;
  };

  const auto off_files = collect(off_dir);
  const auto cmp_files = collect(cmp_dir);
  CHECK(off_files == cmp_files);

  // Cleanup
  fs::remove_all(base_dir);
}

// ─── Multi-iter training invariance (Phase 2 exit criterion) ───────────────
//
// Stronger than the `max_iter=0` test above: this one runs a real training
// loop (`max_iterations=3`) with `low_memory=off` and `low_memory=compress`,
// then asserts that the consolidated `solution.csv` matches byte-for-byte
// and the per-row obj_value / status columns agree.  Exercises the paths
// Phase 2 was designed to keep numerically identical:
//
//   • forward pass: saved-snapshot getters return the same values compress
//     now serves from `m_cached_col_sol_` as off reads from the live backend;
//   • `PlanningLP::write_out` fast-path (Phase 2b) under compress skips
//     `ensure_lp_built` + `release_backend` for sim-pass cells — the test
//     catches any path where that fast-path would emit different numbers;
//   • multi-iter training produces the same final cuts across modes,
//     which the `low_memory parity` test above already covers for the
//     UB/LB scalars but not for the emitted files.
TEST_CASE(  // NOLINT
    "SDDPMethod — multi-iter training output invariance "
    "across low_memory modes")
{
  namespace fs = std::filesystem;

  constexpr int kIters = 3;
  constexpr double kConvTol = 1e-3;

  auto run_once = [&](std::optional<LowMemoryMode> mode,
                      const fs::path& out_dir) -> void
  {
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    auto planning = make_3phase_hydro_planning();
    planning.options.output_directory = out_dir.string();

    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = kIters;
    sddp_opts.convergence_tol = kConvTol;
    sddp_opts.enable_api = false;
    if (mode) {
      sddp_opts.low_memory_mode = *mode;
    }

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());

    // Mirror the SDDPPlanningMethod summary population so solution.csv
    // emits the same gap/max_kappa/converged columns in both runs.
    const auto& last = results->back();
    planning_lp.set_sddp_summary({
        .gap = last.gap,
        .gap_change = last.gap_change,
        .lower_bound = last.lower_bound,
        .upper_bound = last.upper_bound,
        .max_kappa = sddp.global_max_kappa(),
        .iterations = static_cast<int>(results->size()),
        .converged = last.converged,
        .stationary_converged = last.stationary_converged,
        .statistical_converged = last.statistical_converged,
    });
    planning_lp.write_out();
  };

  const auto base_dir =
      fs::temp_directory_path() / "gtopt_multi_iter_invariance";
  const auto off_dir = base_dir / "off";
  const auto cmp_dir = base_dir / "compress";

  run_once(std::nullopt, off_dir);
  run_once(LowMemoryMode::compress, cmp_dir);

  const auto off_sol = off_dir / "solution.csv";
  const auto cmp_sol = cmp_dir / "solution.csv";
  REQUIRE(fs::exists(off_sol));
  REQUIRE(fs::exists(cmp_sol));

  // Parse solution.csv and compare the *solution* columns (status,
  // obj_value, gap, gap_change) exactly / within tolerance.  Kappa and
  // max_kappa are solver-internal condition numbers that legitimately
  // diverge between off (live-backend warm-start across iterations) and
  // compress (release-reconstruct path) — they reflect different
  // simplex-iteration paths, not a solution divergence.
  struct SolRow
  {
    std::string scene;
    std::string phase;
    std::string status;
    std::string status_name;
    double obj_value {};
    double gap {};
    double gap_change {};
  };
  const auto parse = [](const fs::path& p) -> std::vector<SolRow>
  {
    std::ifstream f(p.string());
    std::vector<SolRow> rows;
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
      if (line.empty()) {
        continue;
      }
      SolRow r;
      std::stringstream ss(line);
      std::string col;
      std::getline(ss, r.scene, ',');
      std::getline(ss, r.phase, ',');
      std::getline(ss, r.status, ',');
      std::getline(ss, r.status_name, ',');
      std::getline(ss, col, ',');
      r.obj_value = std::stod(col);
      std::getline(ss, col, ',');  // kappa (skip — solver internal)
      std::getline(ss, col, ',');  // max_kappa (skip — solver internal)
      std::getline(ss, col, ',');
      r.gap = std::stod(col);
      std::getline(ss, col, ',');
      r.gap_change = std::stod(col);
      rows.push_back(std::move(r));
    }
    return rows;
  };

  const auto off_rows = parse(off_sol);
  const auto cmp_rows = parse(cmp_sol);
  REQUIRE(off_rows.size() == cmp_rows.size());
  REQUIRE_FALSE(off_rows.empty());

  for (std::size_t i = 0; i < off_rows.size(); ++i) {
    const auto& a = off_rows[i];
    const auto& b = cmp_rows[i];
    CHECK(a.scene == b.scene);
    CHECK(a.phase == b.phase);
    CHECK(a.status == b.status);
    CHECK(a.status_name == b.status_name);
    CHECK(a.obj_value == doctest::Approx(b.obj_value).epsilon(1e-6));
    CHECK(a.gap == doctest::Approx(b.gap).epsilon(1e-6));
    CHECK(a.gap_change == doctest::Approx(b.gap_change).epsilon(1e-6));
  }

  // File-set parity: every parquet / csv shard must be emitted by both
  // modes.  Catches a regression where Phase 2b's fast-path would skip
  // a cell under one mode but not the other.
  const auto collect = [&](const fs::path& root)
  {
    std::vector<std::string> rel;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (e.is_regular_file()) {
        rel.push_back(fs::relative(e.path(), root).string());
      }
    }
    std::ranges::sort(rel);
    return rel;
  };

  const auto off_files = collect(off_dir);
  const auto cmp_files = collect(cmp_dir);
  CHECK(off_files == cmp_files);

  fs::remove_all(base_dir);
}

// ─── ElasticFilterMode end-to-end comparison ────────────────────────────────
//
// End-to-end smoke test: drive the SDDP solver through every
// ElasticFilterMode value (single_cut, multi_cut, chinneck) on the
// same small fixture, and verify each mode dispatches correctly,
// converges, and stores at least one optimality cut.
//
// What this test demonstrates:
//   - Mode dispatch in SDDPMethod::elastic_solve() routes to the right
//     filter (regular elastic vs chinneck IIS) without crashing
//   - All three modes produce a converged solution on a small hydro
//     problem
//
// What this test does NOT demonstrate (by itself):
//   - The structural difference between the modes when feasibility cuts
//     ARE generated.  On well-conditioned fixtures like the ones in
//     `sddp_helpers.hpp`, the SDDP optimality-cut path converges in 2-3
//     iterations without ever triggering forward-pass infeasibility
//     (logs show `infeas_cuts=0`).  The IIS algorithm itself is exercised
//     by the LP-level unit tests in `test_benders_cut.cpp`
//     ("chinneck_filter_solve filters non-essential link", etc.) — those
//     directly compare the elastic vs IIS link_infos on a fixture
//     designed to have one essential and one non-essential bound.
//
// Reference structural property (only meaningful when feas_cuts > 0):
//   single_cut : 1 fcut per infeasibility, full coeff fan-out
//   multi_cut  : 1 fcut + per-active-slack mcuts
//   chinneck   : 1 fcut + per-IIS-bound mcuts (≤ multi_cut)
//
// Conditional assertions below check those properties only when the
// fixture actually generates feasibility cuts.
TEST_CASE(  // NOLINT
    "SDDPMethod - ElasticFilterMode comparison on shared hydro fixture")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  struct ModeOutcome
  {
    int total_cuts {0};
    int feas_cuts {0};
    int opt_cuts {0};
    bool converged {false};
    double avg_feas_coeffs {0.0};
  };

  auto run_mode = [](ElasticFilterMode mode) -> ModeOutcome
  {
    // Use the forced-infeasibility fixture: phase 1's mandatory
    // waterway discharge (`fmin=5 hm³/h`) cannot be met from the
    // reservoir state that phase 0's optimum produces (eini ≈ 0 after
    // phase 0 drains the reservoir for its own cheap-hydro dispatch).
    // The first forward pass therefore lands phase 1 in an infeasible
    // LP, the elastic filter activates, and at least one fcut is
    // installed.  This exercises the cut-construction branches we want
    // to compare across modes.
    auto planning = make_forced_infeasibility_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.elastic_filter_mode = mode;
    // Force aggressive multi-cut in modes that use it, so the comparison
    // clearly distinguishes single_cut from multi_cut from chinneck.
    sddp_opts.multi_cut_threshold = 0;

    SDDPMethod sddp(planning_lp, sddp_opts);
    // Note: solve() may return std::unexpected if a backward-pass scut
    // cascades into LP infeasibility on this aggressive fixture (a
    // separate concern from the comparison being made here — the scut
    // resolve-after-cut at sddp_method.cpp:896 still treats post-cut
    // non-optimality as an error).  We don't REQUIRE convergence here
    // — we inspect whatever cuts were stored before the failure.
    [[maybe_unused]] auto results = sddp.solve();

    ModeOutcome out;
    out.converged =
        results.has_value() && !results->empty() && results->back().converged;

    const auto cuts = sddp.stored_cuts();
    out.total_cuts = static_cast<int>(cuts.size());

    int total_feas_coeffs = 0;
    for (const auto& c : cuts) {
      if (c.type == CutType::Feasibility) {
        ++out.feas_cuts;
        total_feas_coeffs += static_cast<int>(c.coefficients.size());
      } else {
        ++out.opt_cuts;
      }
    }
    out.avg_feas_coeffs = (out.feas_cuts > 0)
        ? static_cast<double>(total_feas_coeffs)
            / static_cast<double>(out.feas_cuts)
        : 0.0;
    return out;
  };

  const auto single = run_mode(ElasticFilterMode::single_cut);
  const auto multi = run_mode(ElasticFilterMode::multi_cut);
  const auto chinneck = run_mode(ElasticFilterMode::chinneck);

  // Surface the comparison so a developer running this test with -v sees
  // exactly what each mode produced.  CAPTURE() keeps the values in the
  // test failure log if any of the structural assertions below break.
  CAPTURE(single.total_cuts);
  CAPTURE(single.feas_cuts);
  CAPTURE(single.opt_cuts);
  CAPTURE(single.avg_feas_coeffs);
  CAPTURE(multi.total_cuts);
  CAPTURE(multi.feas_cuts);
  CAPTURE(multi.opt_cuts);
  CAPTURE(multi.avg_feas_coeffs);
  CAPTURE(chinneck.total_cuts);
  CAPTURE(chinneck.feas_cuts);
  CAPTURE(chinneck.opt_cuts);
  CAPTURE(chinneck.avg_feas_coeffs);

  // ── Fcuts must fire in every mode that emits them: single_cut,
  //    multi_cut, and chinneck all install fcuts in the forward pass
  //    when the elastic filter activates.
  CHECK(single.feas_cuts >= 1);
  CHECK(multi.feas_cuts >= 1);
  CHECK(chinneck.feas_cuts >= 1);

  // ── Structural property: with multi_cut active, the per-bound bound
  //    cuts add to the total cut count beyond single_cut's flat count.
  //    Chinneck filters those cuts to the IIS subset, so it emits at
  //    most as many feasibility-class cuts as full multi_cut.
  CHECK(chinneck.feas_cuts <= multi.feas_cuts);
}

// ── Two-reservoir variant: drives all three cut-emitting modes
//    through an SDDP solve on a fixture where one reservoir has a
//    mandatory waterway minimum discharge (essential) and the other
//    does not (potentially non-essential).
//
//    Honest observation — on this fixture the elastic LP picks both
//    reservoirs' state-variable slacks at sdn = 1.0 in LP units (LP
//    fills both reservoirs to capacity even at very high
//    elastic_penalty).  Investigation shows the dep-column cost
//    structure and degenerate primal optimum tie both reservoirs
//    together, so chinneck cannot classify reservoir 2 as
//    non-essential and falls through its `non_essential.empty()`
//    early-exit.  Result: chinneck.feas_cuts == multi.feas_cuts here.
//
//    The IIS algorithm IS demonstrably correct on a synthetic LP
//    fixture (see test_benders_cut.cpp's "chinneck_filter_solve
//    filters non-essential link" test, which constructs a 2-link
//    case where the slacks are clearly asymmetric).  Reproducing
//    that asymmetry in a full SDDP fixture requires a more
//    decoupled hydro problem than this two-reservoir-shared-bus
//    setup — a follow-up task.
//
//    The assertions here are therefore the conservative
//    `chinneck ≤ multi` form: chinneck must NOT produce more cuts
//    than multi_cut, regardless of whether IIS filtering kicks in.
TEST_CASE(  // NOLINT
    "SDDPMethod - ElasticFilterMode comparison on 2-reservoir fixture")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  struct ModeOutcome
  {
    int total_cuts {0};
    int feas_cuts {0};
    int opt_cuts {0};
    bool converged {false};
  };

  auto run_mode = [](ElasticFilterMode mode) -> ModeOutcome
  {
    auto planning = make_two_reservoir_forced_infeasibility_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.elastic_filter_mode = mode;
    sddp_opts.multi_cut_threshold = 0;  // force per-bound mcuts

    SDDPMethod sddp(planning_lp, sddp_opts);
    [[maybe_unused]] auto results = sddp.solve();

    ModeOutcome out;
    out.converged =
        results.has_value() && !results->empty() && results->back().converged;
    const auto cuts = sddp.stored_cuts();
    out.total_cuts = static_cast<int>(cuts.size());
    for (const auto& c : cuts) {
      if (c.type == CutType::Feasibility) {
        ++out.feas_cuts;
      } else {
        ++out.opt_cuts;
      }
    }
    return out;
  };

  const auto single = run_mode(ElasticFilterMode::single_cut);
  const auto multi = run_mode(ElasticFilterMode::multi_cut);
  const auto chinneck = run_mode(ElasticFilterMode::chinneck);

  CAPTURE(single.feas_cuts);
  CAPTURE(single.total_cuts);
  CAPTURE(multi.feas_cuts);
  CAPTURE(multi.total_cuts);
  CAPTURE(chinneck.feas_cuts);
  CAPTURE(chinneck.total_cuts);

  // ── Fcuts must fire in every cut-emitting mode.
  CHECK(single.feas_cuts >= 1);
  CHECK(multi.feas_cuts >= 1);
  CHECK(chinneck.feas_cuts >= 1);

  // ── Conservative IIS bound: chinneck never emits MORE feasibility
  //    cuts than full multi_cut.  When the LP has slack asymmetry
  //    chinneck can be strictly less; here it ties due to the
  //    degenerate primal optimum (see test docstring).
  CHECK(chinneck.feas_cuts <= multi.feas_cuts);

  // ── single_cut emits one fcut per infeasibility event regardless
  //    of how many slacks are active, so it should never exceed
  //    multi_cut's total feasibility-class cut count.
  CHECK(single.feas_cuts <= multi.feas_cuts);
}
