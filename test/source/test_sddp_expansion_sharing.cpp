// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_expansion_sharing.cpp
 * @brief     Capacity expansion × SDDP × cut sharing — the multi-scene
 *            expansion case (hedging vs wait-and-see) plus the first
 *            production-fixture exercise of `extract_capacity_cuts`.
 * @date      2026-07-08
 *
 * Fixture: 2 scenes × 3 phases on the shared hydro fixture, with a
 * peaker-class expansion candidate (c0 convention: base capacity 0 +
 * `expcap`/`expmod`/`annual_capcost`) whose build is economic ONLY
 * under the dry hydrology:
 *
 *   * demand 75 MW, hydro 60 MW @ $5, thermal 30 MW @ $50, fail $1000;
 *   * candidate: up to 1 × 50 MW module, gcost $60 (ABOVE thermal — it
 *     runs only against the $1000 fail cost, never displaces thermal),
 *     annual_capcost 8760 $/MW-yr = 1 $/MW-h, buildable at stage 0 only
 *     (expmod schedule {1, 0, 0});
 *   * phase 0 is scene-IDENTICAL (eini = 200 = emax, inflow 8): supply
 *     comfortably exceeds 75 → no shortage, so the phase-0 build
 *     decision is driven purely by the FUTURE term;
 *   * phases 1-2 diverge: wet inflow 60 (hydro 60 + thermal 30 > 75 →
 *     no shortage → candidate idle → the build is a pure carrying-cost
 *     loss), dry inflow 2 (stored 200 + 16 dam³ over 8 h → hydro
 *     ≈ 27 MW avg; 27 + 30 ≪ 75 → ≈ 18 MW shortfall at $1000/MWh; each
 *     candidate MW repays its 12 $/MW carrying cost ~600×).  The dry
 *     wait-and-see optimum builds ≈ the shortfall (continuous module),
 *     the wet one builds exactly 0.
 *
 * Under `cut_sharing = none` each scene prices its own persistent
 * future (wait-and-see): dry builds 50 MW, wet builds 0 — the phase-0
 * capacity decisions DIFFER.  Under `multicut` both scenes share the
 * cut family, so their (identical-data) phase-0 LPs see the same
 * probability-weighted future and build the same hedged capacity.
 *
 * Companion user doc: docs/examples/sddp-integer-cuts.md (integer
 * module subcase); design: §7 of
 * docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md.
 *
 * FINDING (2026-07-08, uncovered by this fixture; FIXED 2026-07-09):
 * per-scene SDDP LPs over-weighted expansion CAPEX by 1/p_s —
 * `capacity_object_lp.cpp` priced `capacost` at probability 1.0
 * (correct only when all scenarios share one LP).  Decisions were
 * unaffected; objectives shifted by exactly one duplicated carrying
 * charge.  FIX: `capacity_object_lp.cpp` now folds the carrying-cost
 * objective coefficient by the owning scene's total probability mass
 * (`SceneLP::probability_factor()` = p_s per scene; = 1.0 in the
 * monolithic single-scene LP, so monolithic behaviour is unchanged).
 * Tests 4-5 now assert the theorem-mandated Benders ≡ SDDP objective
 * equalities strictly (the WARNs became CHECKs, the over-count pins
 * were removed).
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/generator_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_capacity_cuts.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"
#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Geometry ────────────────────────────────────────────────────────────────
constexpr int kExNumPhases = 3;
constexpr int kExBlocksPerStage = 4;
constexpr double kExDemand = 75.0;
constexpr double kExHydroCap = 60.0;
constexpr double kExThermalCap = 30.0;
constexpr double kExEini = 200.0;  // full reservoir → comfortable phase 0
constexpr double kExPhase0Inflow = 8.0;  // scene-identical phase 0
constexpr double kExWetInflow = 60.0;  // phases 1-2, wet scene
constexpr double kExDryInflow = 2.0;  // phases 1-2, dry scene
constexpr double kExExpCap = 50.0;  // MW per module
constexpr Uid kExCandidateUid {3};

[[nodiscard]] double ex_tol(double v_abs)
{
  constexpr double kSolverFeasTol = 1.0e-6;
  constexpr double kMipGapAllowance = 2.0e-6;
  return (kSolverFeasTol + kMipGapAllowance) * std::max(1.0, v_abs);
}

// ─── Fixture ─────────────────────────────────────────────────────────────────

/// Per-scenario inflow schedule: identical phase 0, divergent phases
/// 1-2 (the hedging-vs-wait-and-see driver).
void ex_set_scenario_inflows(Planning& planning, double q_wet, double q_dry)
{
  std::vector<std::vector<std::vector<double>>> sched;
  sched.reserve(2);
  for (const double q : {q_wet, q_dry}) {
    std::vector<std::vector<double>> per_stage;
    per_stage.reserve(static_cast<std::size_t>(kExNumPhases));
    for (int st = 0; st < kExNumPhases; ++st) {
      const double value = (st == 0) ? kExPhase0Inflow : q;
      per_stage.push_back(std::vector<double>(
          static_cast<std::size_t>(kExBlocksPerStage), value));
    }
    sched.push_back(std::move(per_stage));
  }
  planning.system.flow_array[0].discharge = STBRealFieldSched {sched};
}

/// 2-scene 3-phase expansion fixture (see the file header).  With
/// `integer_module = true` the candidate's expmod column is integer —
/// the MIP-gated subcase.
[[nodiscard]] Planning ex_make_planning(double q_wet,
                                        double q_dry,
                                        bool integer_module = false,
                                        const std::string& solver = {})
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.system.demand_array[0].capacity = kExDemand;
  planning.system.generator_array[0].capacity = kExHydroCap;  // hydro
  planning.system.generator_array[1].capacity = kExThermalCap;  // thermal
  planning.system.reservoir_array[0].eini = kExEini;

  // Candidate peaker: buildable at stage 0 only (expmod schedule),
  // priced above thermal so it runs only against the fail cost.
  // `capmax` must be pinned explicitly: its per-stage default is
  // `capacity + expcap·expmod`, which collapses to 0 on stages 1-2
  // (expmod 0 there) and would destroy the built capacity.
  planning.system.generator_array.push_back(Generator {
      .uid = kExCandidateUid,
      .name = "candidate_gen",
      .bus = Uid {1},
      .gcost = 60.0,
      .capacity = 0.0,
      .expcap = kExExpCap,
      .expmod = std::vector<double> {1.0, 0.0, 0.0},
      .capmax = kExExpCap,
      .annual_capcost = 8760.0,  // 1 $/MW-h
      .integer_expmod = integer_module,
  });

  ex_set_scenario_inflows(planning, q_wet, q_dry);
  if (!solver.empty()) {
    planning.options.lp_matrix_options.solver_name = solver;
    planning.options.solver_options.mip_gap = 1e-9;
    planning.options.solver_options.mip_gap_abs = 1e-7;
  }
  return planning;
}

/// Single-scenario extensive-form Planning: the SAME system with all
/// 3 stages in ONE phase (the monolithic/Benders-equivalence truth for
/// the identical-scene gates).  @p inflow applies to stages 1-2;
/// stage 0 keeps the fixture's phase-0 inflow.
[[nodiscard]] Planning ex_make_extensive_planning(double inflow,
                                                  bool integer_module,
                                                  const std::string& solver)
{
  auto block_array = make_uniform_blocks(
      static_cast<std::size_t>(kExNumPhases * kExBlocksPerStage), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(kExNumPhases),
                          static_cast<std::size_t>(kExBlocksPerStage));

  Array<Phase> phase_array = {
      Phase {
          .uid = Uid {1},
          .first_stage = 0,
          .count_stage = static_cast<Size>(kExNumPhases),
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 1.0,
              },
          },
      .phase_array = std::move(phase_array),
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = kExHydroCap,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = kExThermalCap,
      },
      {
          .uid = kExCandidateUid,
          .name = "candidate_gen",
          .bus = Uid {1},
          .gcost = 60.0,
          .capacity = 0.0,
          .expcap = kExExpCap,
          .expmod = std::vector<double> {1.0, 0.0, 0.0},
          .capmax = kExExpCap,  // see ex_make_planning: keep stages 1-2
          .annual_capcost = 8760.0,
          .integer_expmod = integer_module,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = kExDemand,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = kExEini,
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
      },
  };
  // (scenario × stage × block) inflow: phase-0 value at stage 0,
  // @p inflow on stages 1-2 — mirroring the SDDP fixture's schedule.
  constexpr auto n_blocks = static_cast<std::size_t>(kExBlocksPerStage);
  const std::vector<std::vector<std::vector<double>>> inflow_sched = {
      {
          std::vector<double>(n_blocks, kExPhase0Inflow),
          std::vector<double>(n_blocks, inflow),
          std::vector<double>(n_blocks, inflow),
      },
  };
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = STBRealFieldSched {inflow_sched},
      },
  };
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  if (!solver.empty()) {
    options.lp_matrix_options.solver_name = solver;
    options.solver_options.mip_gap = 1e-9;
    options.solver_options.mip_gap_abs = 1e-7;
  }

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_expansion_extensive",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Monolithic objective of the extensive form (LP or MIP).
[[nodiscard]] double ex_extensive_value(double inflow,
                                        bool integer_module,
                                        const std::string& solver)
{
  auto planning = ex_make_extensive_planning(inflow, integer_module, solver);
  PlanningLP plp(std::move(planning));
  auto status = plp.resolve();
  REQUIRE(status.has_value());
  REQUIRE(*status == 1);  // optimal
  return plp.system(first_scene_index(), PhaseIndex {0})
      .linear_interface()
      .get_obj_value();
}

// ─── SDDP driver ─────────────────────────────────────────────────────────────

[[nodiscard]] SDDPOptions ex_sddp_opts(CutSharingMode cut_sharing,
                                       int max_iterations,
                                       double convergence_tol = 1.0e-9)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = convergence_tol;
  opts.stationary_tol = 0.0;
  opts.cut_sharing = cut_sharing;
  opts.integer_cuts = IntegerCutsMode::none;
  opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  opts.enable_api = false;
  return opts;
}

/// Candidate `capainst` at the FIRST stage of phase 0 of scene 0 —
/// works for both the SDDP fixture and the single-phase extensive twin.
[[nodiscard]] double ex_first_stage_capacity(PlanningLP& plp)
{
  auto& sim = plp.simulation();
  auto& sys = plp.systems()[SceneIndex {0}][PhaseIndex {0}];
  sys.rebuild_collections_if_needed();
  const auto& gen_lps = sys.template elements<GeneratorLP>();
  const auto it = std::ranges::find_if(
      gen_lps, [](const auto& g) { return g.uid() == kExCandidateUid; });
  REQUIRE(it != gen_lps.end());
  const auto& stage0 = sim.phases()[PhaseIndex {0}].stages().front();
  const auto col = it->capacity_col_at(stage0);
  REQUIRE(col.has_value());
  return sys.linear_interface().get_col_sol()[*col];
}

/// Phase-0 installed capacity (`capainst` at stage 0) of the candidate
/// unit, per scene, read from the solved planning.
[[nodiscard]] std::array<double, 2> ex_phase0_capacity(PlanningLP& plp)
{
  std::array<double, 2> caps {};
  auto& sim = plp.simulation();
  REQUIRE(sim.scene_count() >= 2);
  for (const auto scene : iota_range<SceneIndex>(0, sim.scene_count())) {
    auto& sys = plp.systems()[scene][PhaseIndex {0}];
    sys.rebuild_collections_if_needed();
    const auto& gen_lps = sys.template elements<GeneratorLP>();
    const auto it = std::ranges::find_if(
        gen_lps, [](const auto& g) { return g.uid() == kExCandidateUid; });
    REQUIRE(it != gen_lps.end());

    const auto& stage0 = sim.phases()[PhaseIndex {0}].stages().front();
    const auto col = it->capacity_col_at(stage0);
    REQUIRE(col.has_value());
    caps[static_cast<std::size_t>(scene)] =
        sys.linear_interface().get_col_sol()[*col];
  }
  return caps;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// 1+2. Mechanism: after a multicut SDDP run, stored optimality cuts
// carry capacity-state coefficients (∂V/∂capainst ≠ 0 on at least one
// cut), and `extract_capacity_cuts` surfaces them — its first exercise
// on a production-shaped (hydro + expansion, heterogeneous scenes)
// fixture.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP expansion sharing — multicut cuts carry capainst coefficients "
    "and extract_capacity_cuts surfaces them")
{
  auto planning = ex_make_planning(kExWetInflow, kExDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = ex_sddp_opts(CutSharingMode::multicut, /*max_iterations=*/5);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto cuts = sddp.stored_cuts();
  const auto& sim = plp.simulation();

  // Walk every stored optimality cut against the state-variable
  // registry: count coefficients landing on Generator/capainst columns.
  int n_capainst_nonzero = 0;
  int n_optimality = 0;
  for (const auto& sc : cuts) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    ++n_optimality;

    std::optional<SceneIndex> scene;
    for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
      if (sim.uid_of(si) == sc.scene_uid) {
        scene = si;
        break;
      }
    }
    std::optional<PhaseIndex> phase;
    for (const auto pi : iota_range<PhaseIndex>(0, sim.phase_count())) {
      if (sim.uid_of(pi) == sc.phase_uid) {
        phase = pi;
        break;
      }
    }
    REQUIRE((scene.has_value() && phase.has_value()));

    const auto& svars = sim.state_variables(*scene, *phase);
    for (const auto& [col, coeff] : sc.coefficients) {
      for (const auto& [key, svar] : svars) {
        if (svar.col() != col) {
          continue;
        }
        if (key.class_name == "Generator" && key.col_name == "capainst"
            && std::abs(coeff) > 1e-9)
        {
          ++n_capainst_nonzero;
        }
        break;
      }
    }
  }
  CAPTURE(n_optimality);
  REQUIRE(n_optimality >= 2);
  // The dry future prices capacity: at least one cut must carry a
  // non-zero ∂V/∂capainst subgradient.
  CHECK(n_capainst_nonzero >= 1);

  // Extraction API on the same run (first production-fixture use).
  const auto extracted = extract_capacity_cuts(sim, cuts);
  REQUIRE_FALSE(extracted.empty());
  bool any_candidate_capainst_nonzero = false;
  for (const auto& cut : extracted) {
    CHECK(std::isfinite(cut.rhs));
    for (const auto& c : cut.coefficients) {
      CHECK((c.col_name == "capainst" || c.col_name == "capacost"));
      if (c.col_name == "capainst" && c.uid == kExCandidateUid
          && std::abs(c.coeff) > 1e-9)
      {
        any_candidate_capainst_nonzero = true;
      }
    }
    // The reservoir energy coordinate is projected away and reported
    // (mixed-state caveat, design doc §7.3).
    CHECK(cut.dropped_state_coefficients >= 1);
  }
  CHECK(any_candidate_capainst_nonzero);
}

// ═════════════════════════════════════════════════════════════════════════
// 3. Hedging vs wait-and-see: under `none` each scene optimizes its own
// hydrology — dry builds the module, wet does not (phase-0 capacities
// DIFFER); under `multicut` the scene-identical phase-0 LPs share the
// cut family and commit a COMMON hedged build.  Bound ordering: the
// none-mode LB (wait-and-see) never exceeds the multicut converged UB
// (a feasible nonanticipative policy cost).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP expansion sharing — wait-and-see (none) splits the build, "
    "multicut hedges it; none LB ≤ multicut UB")
{
  constexpr int kIters = 12;

  // ── cut_sharing = none (wait-and-see) ────────────────────────────
  auto none_planning = ex_make_planning(kExWetInflow, kExDryInflow);
  PlanningLP none_plp(std::move(none_planning));
  auto none_opts = ex_sddp_opts(CutSharingMode::none, kIters);
  SDDPMethod none_sddp(none_plp, none_opts);
  auto none_results = none_sddp.solve();
  REQUIRE(none_results.has_value());
  REQUIRE_FALSE(none_results->empty());

  const auto none_caps = ex_phase0_capacity(none_plp);
  INFO("none-mode phase-0 capacity: wet=", none_caps[0], " dry=", none_caps[1]);
  // Scene 0 (wet future): the build is a pure loss → no capacity.
  CHECK(none_caps[0] <= 1e-3);
  // Scene 1 (dry future): the capacity repays its carrying cost ~600×
  // up to the ≈ 18 MW shortfall → a substantial build.
  CHECK(none_caps[1] >= 10.0);
  // The wait-and-see decisions genuinely DIFFER.
  CHECK(none_caps[1] - none_caps[0] >= 10.0);

  // ── cut_sharing = multicut (hedged) ──────────────────────────────
  auto mc_planning = ex_make_planning(kExWetInflow, kExDryInflow);
  PlanningLP mc_plp(std::move(mc_planning));
  auto mc_opts = ex_sddp_opts(CutSharingMode::multicut, kIters);
  SDDPMethod mc_sddp(mc_plp, mc_opts);
  auto mc_results = mc_sddp.solve();
  REQUIRE(mc_results.has_value());
  REQUIRE_FALSE(mc_results->empty());

  const auto mc_caps = ex_phase0_capacity(mc_plp);
  INFO(
      "multicut phase-0 capacity: scene0=", mc_caps[0], " scene1=", mc_caps[1]);
  // Identical phase-0 data + shared cut family ⇒ a COMMON first-phase
  // decision.
  CHECK(mc_caps[0] == doctest::Approx(mc_caps[1]).epsilon(1e-4));
  // The hedge must build: the dry branch's $1000/MWh exposure at
  // probability 0.5 dwarfs the 12 $/MW carrying cost.
  CHECK(mc_caps[0] >= 10.0);

  // ── Bound ordering ───────────────────────────────────────────────
  // Wait-and-see LB ≤ nonanticipative optimum ≤ any feasible
  // nonanticipative policy cost (the multicut forward UB).
  const double ws_lb = none_results->back().lower_bound;
  const double mc_ub = mc_results->back().upper_bound;
  INFO("wait-and-see LB=", ws_lb, " multicut UB=", mc_ub);
  CHECK(ws_lb <= mc_ub + 2.0 * ex_tol(std::abs(mc_ub)));
}

// ═════════════════════════════════════════════════════════════════════════
// 4. Identical-scene sanity (Benders ≡ SDDP for expansion) — and the
// per-scene CAPEX weighting FIX this fixture uncovered (2026-07-08, fixed
// 2026-07-09):
//
// `CapacityObjectBase::add_to_lp` now prices the `capacost` column with
// `CostHelper::stage_ecost(stage, 1.0, scene_prob)`, where `scene_prob`
// is the owning scene's total probability mass
// (`SceneLP::probability_factor()`).  Capacity columns are still built
// once (first scenario only), but the objective folds the SAME
// probability mass the dispatch/OPEX coefficients carry:
//   * Monolithic LP: the single synthetic scene holds every scenario →
//     `scene_prob` sums to 1.0 → identical to the previous `stage_ecost
//     (stage, 1.0)` behaviour (monolithic invariance).
//   * Per-scene SDDP LP: each LP carries ONLY its own scenario with the
//     GLOBALLY-normalized p_s (0.5 here), so BOTH dispatch and CAPEX
//     fold by 0.5.  Summed across scenes (`compute_iteration_bounds` is
//     a plain sum) the carrying charge reconstitutes exactly once.
//
// Verified decomposition on this fixture (probe run 2026-07-08): both
// the SDDP run and the single-phase extensive twin build the SAME
// K = 24.333 MW (expmod 0.48667).  Post-fix each scene-LP total is
// 0.5 × dispatch(36 760) + 0.5 × carrying(292) = 18 526 → summed over
// the two identical scenes LB = UB = 37 052 = v_full exactly (the
// carrying charge is now counted once, not twice).
//
// Both the DECISION equivalence (Benders ≡ SDDP builds) AND the
// objective equality (LB = UB = v_full, the theorem-mandated
// Benders ≡ SDDP identity) now hold and are asserted strictly.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP expansion sharing — identical scenes: SDDP reproduces the "
    "extensive build AND the extensive objective (Benders ≡ SDDP)")
{
  auto planning = ex_make_planning(kExDryInflow, kExDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = ex_sddp_opts(CutSharingMode::multicut,
                           /*max_iterations=*/25,
                           /*convergence_tol=*/1.0e-6);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // Extensive twin: value AND build decision.
  double v_full = 0.0;
  double ext_build = 0.0;
  {
    auto ext_planning = ex_make_extensive_planning(kExDryInflow,
                                                   /*integer_module=*/false,
                                                   /*solver=*/ {});
    PlanningLP ext_plp(std::move(ext_planning));
    auto ext_status = ext_plp.resolve();
    REQUIRE(ext_status.has_value());
    REQUIRE(*ext_status == 1);
    v_full = ext_plp.system(first_scene_index(), PhaseIndex {0})
                 .linear_interface()
                 .get_obj_value();
    ext_build = ex_first_stage_capacity(ext_plp);
  }
  REQUIRE(v_full > 0.0);

  // Decision equivalence: the SDDP build matches the extensive build,
  // identically in both scenes (Benders ≡ SDDP on the decision space).
  const auto caps = ex_phase0_capacity(plp);
  INFO("sddp builds: ", caps[0], "/", caps[1], " extensive: ", ext_build);
  CHECK(caps[0] == doctest::Approx(caps[1]).epsilon(1e-4));
  CHECK(caps[0] == doctest::Approx(ext_build).epsilon(1e-3));
  CHECK(caps[0] >= 10.0);

  const auto& last = results->back();
  const double tol = 2.0 * ex_tol(std::abs(v_full));
  INFO("LB=", last.lower_bound, " UB=", last.upper_bound, " v_full=", v_full);
  // Theorem-mandated equivalence — holds now that the per-scene CAPEX
  // weighting is fixed (folded by scene_prob = p_s).
  CHECK(last.lower_bound <= v_full + tol);
  // Converged (LB = UB) …
  CHECK(last.lower_bound == doctest::Approx(last.upper_bound).epsilon(1e-6));
  // … exactly onto the extensive optimum: the carrying charge is now
  // folded by p_s in each identical scene and the plain cross-scene sum
  // reconstitutes it once (no duplication).  Previously (defect) the
  // SDDP objective was v_full + one extra 12 h × K × 1 $/MW-h charge.
  CHECK(last.upper_bound == doctest::Approx(v_full).epsilon(1e-3));
  MESSAGE("identical-scene gate: v_full=", v_full, " sddp=", last.upper_bound);
}

// ═════════════════════════════════════════════════════════════════════════
// 5. Integer expansion module (MIP-gated): `integer_expmod = true` on
// the identical-scene fixture — the build is integral (0 or the full
// 50 MW module) and matches the extensive MIP, and the SDDP LB stays at
// or below the monolithic extensive MIP optimum (the theorem-mandated
// Benders ≡ SDDP gate).  With the per-scene CAPEX weighting fixed
// (folded by scene_prob = p_s) the previous +600 over-charge (the
// 50 MW module double-counted across two identical p_s = 0.5 scenes)
// is gone: each scene charges 0.5 × 600 and the plain cross-scene sum
// reconstitutes the single 600 carrying charge that the extensive MIP
// also pays.  Runs with `integer_cuts = strengthened` (the certified
// mode on integer-bearing cells, Theorem SB1).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP expansion sharing — integer module (MIP): integral build; "
    "LB ≤ the monolithic MIP optimum (Benders ≡ SDDP gate)")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping integer-module tier");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        auto planning = ex_make_planning(kExDryInflow,
                                         kExDryInflow,
                                         /*integer_module=*/true,
                                         mip_solver);
        PlanningLP plp(std::move(planning));

        auto opts = ex_sddp_opts(CutSharingMode::multicut,
                                 /*max_iterations=*/12,
                                 /*convergence_tol=*/1.0e-6);
        opts.integer_cuts = IntegerCutsMode::strengthened;
        SDDPMethod sddp(plp, opts);
        auto results = sddp.solve();
        REQUIRE(results.has_value());
        REQUIRE_FALSE(results->empty());

        const double v_mip = ex_extensive_value(kExDryInflow,
                                                /*integer_module=*/true,
                                                mip_solver);
        REQUIRE(v_mip > 0.0);

        // Integral build: capainst ∈ {0, 50} — and on this dry fixture
        // the module must be built (matching the extensive MIP).
        const auto caps = ex_phase0_capacity(plp);
        for (const auto cap : caps) {
          INFO("cap=", cap);
          const bool integral =
              std::abs(cap) < 1e-3 || std::abs(cap - kExExpCap) < 1e-3;
          CHECK(integral);
        }
        CHECK(caps[0] == doctest::Approx(kExExpCap).epsilon(1e-3));
        CHECK(caps[1] == doctest::Approx(kExExpCap).epsilon(1e-3));

        // LB validity: the theorem-mandated Benders ≡ SDDP gate
        // LB ≤ v_mip now holds directly, because the per-scene CAPEX
        // weighting is fixed (folded by scene_prob = p_s).  Previously
        // (defect) the built module was double-charged across the two
        // identical p_s = 0.5 scenes (+12 h × 50 MW × 1 $/MW-h = +600),
        // and this assertion had to be relaxed by that over-count.
        const double tol = 2.0 * ex_tol(std::abs(v_mip));
        for (const auto& ir : *results) {
          INFO("iter=",
               static_cast<int>(ir.iteration_index),
               " LB=",
               ir.lower_bound,
               " v_mip=",
               v_mip);
          CHECK(ir.lower_bound <= v_mip + tol);
        }
        MESSAGE("integer gate: v_mip=",
                v_mip,
                " final LB=",
                results->back().lower_bound);
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — integer-module tier skipped");
  }
}
