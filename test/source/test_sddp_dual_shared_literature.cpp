// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_dual_shared_literature.cpp
 * @brief     Literature validation of the dual-shared aperture cut modes
 *            (`aperture_solve_mode = dual_shared` / `screened`) against
 *            the Infanger–Morton cut-sharing methodology.
 * @date      2026-07-08
 *
 * ## Provenance
 *
 * - G. Infanger, D. P. Morton, "Cut sharing for multistage stochastic
 *   linear programs with interstage dependency", *Mathematical
 *   Programming* 75 (1996) 241–256, DOI 10.1007/BF02592154.  The journal
 *   article is paywalled; the recovered public source used here is the
 *   technical-report-level exposition in:
 * - D. P. Morton, "Algorithmic Advances in Stochastic Programming",
 *   Stanford Systems Optimization Laboratory report SOL 93-6 (July
 *   1993), full text public via OSTI (osti.gov/biblio/10186619).
 *   Section 2.2 states the *dual sharing formula* — optimal duals of one
 *   solved same-stage subproblem are dual-feasible for every sibling
 *   subproblem, so weak duality yields a valid cut without re-solving —
 *   and §3.5 extends it to interstage-dependent RHS processes.
 *
 * The paper's own test instances (PG&E Mokelumne / Yuba-Bear South
 * Feather multireservoir basins, SOL 93-6 Table 2.1) were never
 * published as data — only dimensions and iteration counts survive.
 * The fixtures below are therefore **reconstructions after the paper's
 * description** (multistage hydro-thermal, stochastic natural inflows
 * entering as simple-bound data, finite bounds everywhere per the §2.2
 * caveat), NOT the original instances.  See
 * `docs/examples/sddp-dual-shared.md` for the full mapping and the
 * honest-gaps list.
 *
 * ## What is asserted
 *
 * gtopt's `dual_shared` mode solves ONE representative aperture per
 * (scene, phase) cell in the backward pass and synthesizes every other
 * aperture's cut from the shared vertex dual with the Lemma AP2
 * bound-delta intercept correction (`dual_shared_bound_correction`,
 * theorem doc §6).  No per-mode LP-solve counter is exported, so the
 * "one LP per cell" half of the claim is asserted via the mode's
 * documented semantics (sddp_enums.hpp) plus the *observable*
 * consequences the paper reports for shared vs exact cuts:
 *
 *  1. identical converged bounds (cold ≡ dual_shared ≡ screened), and
 *  2. convergence-equivalence — same iterations-to-tolerance.
 *
 * On these fixtures the per-aperture value functions are LINEAR in the
 * inflow over the whole reachable state box (designed so no aperture
 * saturates demand or capacity), hence the Lemma AP2 correction
 * `Σ rc·Δbound` reproduces the exact aperture intercept and the
 * synthesized cut EQUALS the exact cut — the sharpest form of the
 * paper's exactness claim, asserted cut-for-cut in the degenerate
 * identical-aperture case and as tight bound parity in the
 * heterogeneous case.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/sddp_cut_store.hpp>

#include "sddp_helpers.hpp"

namespace dual_shared_lit
{
namespace
{

/// Parameters of the reconstructed Infanger–Morton style hydro-thermal
/// instance.  One bus, one reservoir (the recourse-function dimension),
/// cheap hydro vs expensive thermal, stochastic natural inflow — the
/// structure of SOL 93-6 §1.3.1 / §2.3 at unit-test scale.
struct FixtureSpec
{
  int num_phases {3};
  double demand_mw {45.0};
  double eini {100.0};
  /// One inflow (dam³/h) per scenario; entry 0 is the forward/base path.
  std::vector<double> aperture_inflows {};
  /// Per-scenario probability factors (normalised by the solver).
  std::vector<double> aperture_probs {};
};

constexpr int kBlocksPerPhase = 4;
constexpr double kHydroCost = 5.0;  // $/MWh
constexpr double kThermalCost = 50.0;  // $/MWh

/// Build the reconstructed instance: N scenarios (scenario 1 = forward
/// path), one aperture per scenario, every phase referencing the full
/// aperture set.  Stochastic inflows enter as per-scenario Flow
/// discharge schedules — exactly the bound-parameterized data the
/// aperture machinery rewrites (`update_aperture` flow-column bounds),
/// matching the paper's premise that randomness lives in RHS/simple
/// bounds only.  All bounds are finite (SOL 93-6 §2.2: "the only arcs
/// which can have nonzero shared μ's have natural finite upper
/// bounds"), so the dual-shared correction never hits the sentinel
/// fallback.
[[nodiscard]] auto make_planning(const FixtureSpec& spec) -> Planning
{
  REQUIRE(spec.aperture_inflows.size() == spec.aperture_probs.size());
  const auto n_phases = static_cast<std::size_t>(spec.num_phases);
  const auto n_scen = spec.aperture_inflows.size();

  auto block_array = make_uniform_blocks(n_phases * kBlocksPerPhase, 1.0);
  auto stage_array = make_uniform_stages(n_phases, kBlocksPerPhase);
  auto phase_array = make_single_stage_phases(n_phases);

  Array<Scenario> scenario_array;
  Array<Aperture> aperture_array;
  Array<Uid> aperture_uids;
  for (const auto i : std::views::iota(std::size_t {0}, n_scen)) {
    const Uid uid {static_cast<int>(i) + 1};
    // Scenario probability_factor folds RAW into the scene-LP objective
    // (`ScenarioLP::probability_factor()` — no cross-scenario
    // normalisation).  Keep it 1.0 so bounds are physical dollars;
    // scenarios 2..N exist only as aperture data sources, and the
    // aperture mixture weights are carried by Aperture::probability_factor
    // below (which IS normalised across active apertures).
    scenario_array.push_back(Scenario {
        .uid = uid,
        .probability_factor = 1.0,
    });
    aperture_array.push_back(Aperture {
        .uid = uid,
        .source_scenario = uid,
        .probability_factor = spec.aperture_probs[i],
    });
    aperture_uids.push_back(uid);
  }
  for (auto& phase : phase_array) {
    phase.apertures = aperture_uids;
  }

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = std::move(scenario_array),
      .phase_array = std::move(phase_array),
  };
  simulation.scene_array = {
      Scene {
          .uid = Uid {1},
          .name = "forward",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
  };
  simulation.aperture_array = std::move(aperture_array);

  // Per-scenario constant inflow schedule (scenario × stage × block).
  using Vec3D = std::vector<std::vector<std::vector<double>>>;
  Vec3D discharge;
  discharge.reserve(n_scen);
  for (const double q : spec.aperture_inflows) {
    discharge.emplace_back(
        n_phases,
        std::vector<double>(static_cast<std::size_t>(kBlocksPerPhase), q));
  }

  System system = {
      .name = "im_dual_shared_lit",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = spec.demand_mw,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "hydro_gen",
                  .bus = Uid {1},
                  .gcost = kHydroCost,
                  .capacity = 50.0,
              },
              {
                  .uid = Uid {2},
                  .name = "thermal_gen",
                  .bus = Uid {1},
                  .gcost = kThermalCost,
                  .capacity = 200.0,
              },
          },
      .junction_array =
          {
              {
                  .uid = Uid {1},
                  .name = "j_up",
              },
              {
                  .uid = Uid {2},
                  .name = "j_down",
                  .drain = true,
              },
          },
      .waterway_array =
          {
              {
                  .uid = Uid {1},
                  .name = "ww1",
                  .junction_a = Uid {1},
                  .junction_b = Uid {2},
                  .fmin = 0.0,
                  .fmax = 50.0,
              },
          },
      .flow_array =
          {
              {
                  .uid = Uid {1},
                  .name = "inflow",
                  .direction = 1,
                  .junction = Uid {1},
                  .discharge = STBRealFieldSched {discharge},
              },
          },
      .reservoir_array =
          {
              {
                  .uid = Uid {1},
                  .name = "rsv1",
                  .junction = Uid {1},
                  .capacity = 200.0,
                  .emin = 0.0,
                  .emax = 200.0,
                  .eini = spec.eini,
                  .fmin = -1000.0,
                  .fmax = +1000.0,
                  .flow_conversion_rate = 1.0,
              },
          },
      .turbine_array =
          {
              {
                  .uid = Uid {1},
                  .name = "tur1",
                  .waterway = Uid {1},
                  .generator = Uid {1},
                  .production_factor = 1.0,
              },
          },
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Outcome of one SDDP run under a given aperture solve mode.
struct RunOutcome
{
  double lower_bound {};
  double upper_bound {};
  int iterations {};
  /// Per-iteration (lower_bound, upper_bound) trajectory.
  std::vector<std::pair<double, double>> bounds {};
  std::vector<StoredCut> cuts {};
};

/// First iteration (1-based) whose relative bound gap
/// |UB − LB| / max(1, |UB|) is within @p tol, or -1 if none.
/// Bound-based convergence measured directly from the trajectory —
/// deliberately independent of the solver's stopping machinery: since
/// the 2026-05-14 rewrite `SDDPIterationResult::converged` is driven
/// by UB *stationarity* (`stationary_tol`), not the signed gap (see
/// sddp_method_iteration.cpp "Convergence semantics"), and these runs
/// disable the stationary check for determinism.
[[nodiscard]] auto iterations_to_gap(
    const std::vector<std::pair<double, double>>& bounds, double tol) -> int
{
  for (const auto i : std::views::iota(std::size_t {0}, bounds.size())) {
    const auto& [lb, ub] = bounds[i];
    if (std::abs(ub - lb) <= tol * std::max(1.0, std::abs(ub))) {
      return static_cast<int>(i) + 1;
    }
  }
  return -1;
}

/// Run SDDP on the fixture under @p mode with a FIXED iteration budget
/// (min == max, tolerances zeroed, no early exit) so per-mode outcomes
/// are compared at the SAME iteration count.  Convergence itself is
/// asserted from the returned bound trajectory via `iterations_to_gap`.
[[nodiscard]] auto run_sddp(const FixtureSpec& spec,
                            ApertureSolveMode mode,
                            int iterations,
                            const std::string& solver) -> RunOutcome
{
  auto planning = make_planning(spec);
  if (!solver.empty()) {
    planning.options.lp_matrix_options.solver_name = solver;
  }
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = iterations;
  opts.min_iterations = iterations;
  opts.convergence_tol = 0.0;
  opts.stationary_tol = 0.0;  // no stationary early exit
  opts.cut_sharing = CutSharingMode::none;
  opts.apertures = std::nullopt;  // use the per-phase aperture_array
  opts.aperture_solve_mode = mode;
  opts.aperture_screen_count = 1;
  opts.enable_api = false;

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  const auto& last = results->back();
  RunOutcome out {
      .lower_bound = last.lower_bound,
      .upper_bound = last.upper_bound,
      .iterations = static_cast<int>(results->size()),
      .cuts = sddp.stored_cuts(),
  };
  out.bounds.reserve(results->size());
  for (const auto& ir : *results) {
    out.bounds.emplace_back(ir.lower_bound, ir.upper_bound);
  }
  return out;
}

/// The three solve modes under comparison, with labels for SUBCASEs.
[[nodiscard]] auto modes_under_test()
{
  return std::array<std::pair<ApertureSolveMode, const char*>, 3> {
      {
          {ApertureSolveMode::cold, "cold"},
          {ApertureSolveMode::dual_shared, "dual_shared"},
          {ApertureSolveMode::screened, "screened"},
      },
  };
}

}  // namespace
}  // namespace dual_shared_lit

// ═════════════════════════════════════════════════════════════════════════
// 1. Heterogeneous apertures — cold ≡ dual_shared ≡ screened bounds.
//
// Reconstruction after SOL 93-6 §2.3: stochastic natural inflow as the
// only random element, dry/base/wet realizations with non-uniform
// probabilities.  Because every aperture stays strictly inside the
// linear region of the stage value function (no demand saturation, no
// capacity/box kink at any reachable state — see the doc for the
// arithmetic), the Lemma AP2 synthesized intercept equals the exact
// aperture intercept, so all three modes must produce the SAME bound
// trajectory — compared here at a fixed iteration budget.
// ═════════════════════════════════════════════════════════════════════════
TEST_CASE(  // NOLINT
    "SDDP dual_shared_literature — Infanger–Morton reconstruction: "
    "cold/dual_shared/screened bound parity on heterogeneous apertures")
{
  const auto solver = pick_non_mindopt_solver();
  if (solver.empty()) {
    MESSAGE("Skipping — only the MindOpt backend is available.");
    return;
  }

  const dual_shared_lit::FixtureSpec spec {
      .num_phases = 3,
      .demand_mw = 45.0,
      .eini = 100.0,
      .aperture_inflows = {5.0, 2.0, 9.0},  // base, dry, wet
      .aperture_probs = {0.5, 0.2, 0.3},
  };
  constexpr int kIters = 12;

  const auto cold =
      dual_shared_lit::run_sddp(spec, ApertureSolveMode::cold, kIters, solver);
  const auto shared = dual_shared_lit::run_sddp(
      spec, ApertureSolveMode::dual_shared, kIters, solver);
  const auto screened = dual_shared_lit::run_sddp(
      spec, ApertureSolveMode::screened, kIters, solver);

  CAPTURE(cold.lower_bound);
  CAPTURE(cold.upper_bound);
  CAPTURE(shared.lower_bound);
  CAPTURE(shared.upper_bound);
  CAPTURE(screened.lower_bound);
  CAPTURE(screened.upper_bound);

  // The paper's headline claim: cuts synthesized from the shared dual
  // are valid AND (in the RHS/bound-only setting) as tight as exact
  // re-solves — identical bounds at the same iteration count.
  CHECK(shared.upper_bound
        == doctest::Approx(cold.upper_bound).epsilon(1.0e-6));
  CHECK(screened.upper_bound
        == doctest::Approx(cold.upper_bound).epsilon(1.0e-6));
  CHECK(shared.lower_bound
        == doctest::Approx(cold.lower_bound).epsilon(1.0e-6));
  CHECK(screened.lower_bound
        == doctest::Approx(cold.lower_bound).epsilon(1.0e-6));

  // Measure-change sanity (theorem doc §6, Theorem AP1): the aperture
  // mixture (mean inflow 5.6 > forward 5.0) prices a wetter tail, so
  // the LB settles BELOW the realized forward cost — a valid,
  // non-closing gap, identical across modes.
  const double tol = 1.0e-6 * std::max(1.0, std::abs(cold.upper_bound));
  CHECK(cold.lower_bound <= cold.upper_bound + tol);
  CHECK(shared.lower_bound <= shared.upper_bound + tol);
  CHECK(screened.lower_bound <= screened.upper_bound + tol);
}

// ═════════════════════════════════════════════════════════════════════════
// 2. Analytic optimum pin + convergence-equivalence.
//
// Mean-matched apertures ({3, 5, 7} dam³/h, equal weights → q-mean 5.0
// = the forward inflow) on a value function linear in both state and
// inflow over the reachable region.  Then the aperture mixture equals
// the persistent process, LB and UB both converge to the analytic
// deterministic optimum:
//
//   total demand energy  D = 3 phases × 4 h × 45 MW = 540 MWh
//   total water          W = eini + 12 h × 5 = 160 dam³  (< D, scarce)
//   optimum = kThermalCost·D − (kThermalCost − kHydroCost)·W
//           = 50·540 − 45·160 = 19800   [physical $, scale_objective=1]
//
// (any dispatch that uses ALL the water is optimal — thermal price is
// flat — so the pin is insensitive to tie-breaking).  The paper's
// convergence-equivalence claim is asserted as: every mode's bound
// trajectory reaches the 1e-6 relative gap, at the same iteration
// index ± small slack, while dual_shared performs one aperture LP
// solve per (scene, phase) per backward pass by construction
// (ApertureSolveMode::dual_shared semantics — no solve counter is
// exported to assert on).  Convergence is measured directly from the
// (LB, UB) trajectory: the solver's own `converged` flag is a UB-
// stationarity signal, disabled here for run-to-run determinism.
// ═════════════════════════════════════════════════════════════════════════
TEST_CASE(  // NOLINT
    "SDDP dual_shared_literature — mean-matched apertures: converged "
    "LB == UB == analytic optimum for cold/dual_shared/screened")
{
  const auto solver = pick_non_mindopt_solver();
  if (solver.empty()) {
    MESSAGE("Skipping — only the MindOpt backend is available.");
    return;
  }

  const dual_shared_lit::FixtureSpec spec {
      .num_phases = 3,
      .demand_mw = 45.0,
      .eini = 100.0,
      .aperture_inflows = {5.0, 3.0, 7.0},  // q-mean == forward inflow
      .aperture_probs = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
  };
  constexpr double kAnalyticOptimum = 19800.0;
  constexpr double kGapTol = 1.0e-6;
  constexpr int kIters = 10;

  std::vector<int> iters_to_tol;
  for (const auto& [mode, label] : dual_shared_lit::modes_under_test()) {
    INFO("mode=", std::string {label});
    const auto out = dual_shared_lit::run_sddp(spec, mode, kIters, solver);
    CAPTURE(out.lower_bound);
    CAPTURE(out.upper_bound);

    // Published-numbers note: no optimal values survive for the
    // paper's PG&E instances; this pin is the ANALYTIC optimum of the
    // reconstruction (derivation above and in the doc).
    CHECK(out.upper_bound == doctest::Approx(kAnalyticOptimum).epsilon(1e-6));
    CHECK(out.lower_bound == doctest::Approx(kAnalyticOptimum).epsilon(1e-5));

    // Valid cuts ⇒ the bound gap closes (the point of the method).
    const int n_to_tol =
        dual_shared_lit::iterations_to_gap(out.bounds, kGapTol);
    CAPTURE(n_to_tol);
    CHECK(n_to_tol > 0);
    iters_to_tol.push_back(n_to_tol);
  }

  // Convergence-equivalence (shared vs exact cuts reach tolerance in
  // the same number of iterations, ± small slack) — the empirical
  // behaviour Infanger–Morton report for cut sharing on their
  // RHS-random instances.
  REQUIRE(iters_to_tol.size() == 3);
  CHECK(std::abs(iters_to_tol[1] - iters_to_tol[0]) <= 2);
  CHECK(std::abs(iters_to_tol[2] - iters_to_tol[0]) <= 2);
}

// ═════════════════════════════════════════════════════════════════════════
// 3. Exactness probe — identical apertures ⇒ synthesized cut EQUALS the
// exact cut, cut-for-cut.
//
// With all three apertures referencing the same inflow, every bound
// delta Δl = Δu = 0, so the Lemma AP2 correction is exactly zero and
// the synthesized cuts must coincide with cold's exact per-aperture
// cuts (which are themselves identical across the three clones).  This
// is the paper's "shared cut at the donor equals the exact cut"
// statement isolated from any mixture effect: same stored cut set —
// type, placement, RHS and coefficients — for cold, dual_shared and
// screened.
// ═════════════════════════════════════════════════════════════════════════
TEST_CASE(  // NOLINT
    "SDDP dual_shared_literature — identical apertures: dual_shared and "
    "screened store cold's exact cuts")
{
  const auto solver = pick_non_mindopt_solver();
  if (solver.empty()) {
    MESSAGE("Skipping — only the MindOpt backend is available.");
    return;
  }

  const dual_shared_lit::FixtureSpec spec {
      .num_phases = 3,
      .demand_mw = 45.0,
      .eini = 100.0,
      .aperture_inflows = {5.0, 5.0, 5.0},
      .aperture_probs = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
  };
  constexpr int kIters = 6;

  const auto cold =
      dual_shared_lit::run_sddp(spec, ApertureSolveMode::cold, kIters, solver);

  for (const auto mode :
       {ApertureSolveMode::dual_shared, ApertureSolveMode::screened})
  {
    const auto other = dual_shared_lit::run_sddp(spec, mode, kIters, solver);
    REQUIRE_FALSE(cold.cuts.empty());
    REQUIRE(other.cuts.size() == cold.cuts.size());
    for (const auto i : std::views::iota(std::size_t {0}, cold.cuts.size())) {
      const auto& a = cold.cuts[i];
      const auto& b = other.cuts[i];
      INFO("cut #", i);
      CHECK(a.type == b.type);
      CHECK(a.scene_uid == b.scene_uid);
      CHECK(a.phase_uid == b.phase_uid);
      CHECK(a.rhs == doctest::Approx(b.rhs).epsilon(1.0e-9));
      REQUIRE(a.coefficients.size() == b.coefficients.size());
      for (const auto j :
           std::views::iota(std::size_t {0}, a.coefficients.size()))
      {
        CHECK(a.coefficients[j].first == b.coefficients[j].first);
        CHECK(a.coefficients[j].second
              == doctest::Approx(b.coefficients[j].second).epsilon(1.0e-9));
      }
    }
    CHECK(other.upper_bound
          == doctest::Approx(cold.upper_bound).epsilon(1.0e-9));
    CHECK(other.lower_bound
          == doctest::Approx(cold.lower_bound).epsilon(1.0e-9));
  }
}
