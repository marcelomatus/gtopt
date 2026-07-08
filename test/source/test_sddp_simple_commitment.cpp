// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_simple_commitment.cpp
 * @brief     SimpleCommitment × SDDP use cases (pmin-per-block scope) plus
 *            the user-visible SDDiP framework surface (integer_cuts):
 *            aperture inertness + planning-level option round-trip.
 * @date      2026-07-08
 *
 * Scope: the unit on/off binary with `pmin·u ≤ g ≤ pmax·u` per block —
 * NO min-up/down time, no ramping (see §5.16 of
 * docs/formulation/mathematical-formulation.md).  Companion user doc:
 * docs/examples/sddp-simple-commitment.md.
 *
 * Discriminating pmin geometry (shared with
 * test_sddp_strengthened_cuts.cpp's witness geometry): demand 60 MW,
 * scenario inflows ≥ 22 dam³/h on the 2-scene 3-phase hydro fixture.
 * Per-stage water (4 h × inflow ≥ 88 dam³) always lets hydro exceed
 * 20 MW, so the thermal residual 60 − h < 40 = pmin at EVERY reservoir
 * state.  The MIP dispatch is pinned to (h = 20, t = 40) — thermal ON
 * at exactly pmin — while the LP relaxation commits fractionally
 * (u = t/200) and serves the residual with t ∈ (0, pmin), which is
 * integer-INFEASIBLE.  That off-or-min gap is what every test below
 * discriminates on.
 *
 * MIP-dependent tests pin `solver_test::first_mip_solver()` (ambient
 * GTOPT_SOLVER=clp would refuse integer columns at `load_flat`) and
 * skip cleanly when no exact MIP backend is loaded, wrapped in
 * `run_or_skip_license` per the unit-commitment test conventions.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/generator_lp.hpp>
#include <gtopt/json/json_options.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"
#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Geometry (see the file header for the derivation) ──────────────────────
constexpr int kScNumPhases = 3;
constexpr int kScBlocksPerStage = 4;
constexpr double kScEmax = 200.0;
constexpr double kScEini = 100.0;
constexpr double kScPmin = 40.0;  // dispatch_pmin — the off-or-min bar
constexpr double kScPmax = 200.0;  // thermal capacity
constexpr double kScDemand = 60.0;  // witness demand (residual < pmin)
constexpr double kScWetInflow = 28.0;
constexpr double kScDryInflow = 22.0;

// Solver feasibility noise + MIP-gap allowance, mirroring the oracle
// tolerances of test_sddp_strengthened_cuts.cpp.
[[nodiscard]] double sc_tol(double v_abs)
{
  constexpr double kSolverFeasTol = 1.0e-6;
  constexpr double kMipGapAllowance = 2.0e-6;
  return (kSolverFeasTol + kMipGapAllowance) * std::max(1.0, v_abs);
}

// ─── Fixture ─────────────────────────────────────────────────────────────────

/// The 2-scene 3-phase hydro fixture plus a SimpleCommitment on the
/// thermal unit (`relax = true` builds the LP-relaxation twin).
[[nodiscard]] Planning sc_make_planning(double p0,
                                        double p1,
                                        const std::string& solver,
                                        double demand = kScDemand,
                                        bool relax = false,
                                        double pmin = kScPmin)
{
  auto planning = make_2scene_3phase_hydro_planning(p0, p1);
  planning.system.demand_array[0].capacity = demand;
  planning.system.simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc_thermal",
          .generator = Uid {2},
          .dispatch_pmin = pmin,
          .relax = relax,
      },
  };
  if (!solver.empty()) {
    planning.options.lp_matrix_options.solver_name = solver;
  }
  planning.options.solver_options.mip_gap = 1e-9;
  planning.options.solver_options.mip_gap_abs = 1e-7;
  return planning;
}

/// Give each scenario its own constant inflow (heterogeneous scenes).
void sc_set_scenario_inflows(Planning& planning, double q0, double q1)
{
  std::vector<std::vector<std::vector<double>>> sched;
  sched.reserve(2);
  for (const double q : {q0, q1}) {
    std::vector<std::vector<double>> per_stage;
    per_stage.reserve(static_cast<std::size_t>(kScNumPhases));
    for (int st = 0; st < kScNumPhases; ++st) {
      per_stage.push_back(
          std::vector<double>(static_cast<std::size_t>(kScBlocksPerStage), q));
    }
    sched.push_back(std::move(per_stage));
  }
  planning.system.flow_array[0].discharge = STBRealFieldSched {sched};
}

/// Single-scenario extensive-form tail Planning: the last @p num_stages
/// stages merged into ONE phase, same system as the SDDP fixture
/// (including the commitment binary unless @p relax).  Mirrors
/// `make_tail_planning` of test_sddp_strengthened_cuts.cpp — kept
/// file-local per the parallel-agent file-coordination constraint.
[[nodiscard]] Planning sc_make_tail_planning(int num_stages,
                                             double inflow,
                                             double eini,
                                             const std::string& solver,
                                             double demand,
                                             bool relax)
{
  auto block_array = make_uniform_blocks(
      static_cast<std::size_t>(num_stages * kScBlocksPerStage), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages),
                          static_cast<std::size_t>(kScBlocksPerStage));

  Array<Phase> phase_array = {
      Phase {
          .uid = Uid {1},
          .first_stage = 0,
          .count_stage = static_cast<Size>(num_stages),
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
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = kScPmax,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = demand,
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
          .capacity = kScEmax,
          .emin = 0.0,
          .emax = kScEmax,
          .eini = eini,
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
      },
  };
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = inflow,
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
  const Array<SimpleCommitment> simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc_thermal",
          .generator = Uid {2},
          .dispatch_pmin = kScPmin,
          .relax = relax,
      },
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  options.lp_matrix_options.solver_name = solver;
  options.solver_options.mip_gap = 1e-9;
  options.solver_options.mip_gap_abs = 1e-7;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_simple_commitment_tail",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .simple_commitment_array = simple_commitment_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Solve the tail extensive form (MIP when `relax == false`) and return
/// its physical objective folded by @p prob externally (the lone
/// scenario's probability_factor is normalized away at LP build time —
/// the convention pinned by the oracle self-check in
/// test_sddp_cut_oracle.cpp).
[[nodiscard]] double sc_tail_value(int num_stages,
                                   double prob,
                                   double inflow,
                                   double eini,
                                   const std::string& solver,
                                   double demand,
                                   bool relax)
{
  auto planning =
      sc_make_tail_planning(num_stages, inflow, eini, solver, demand, relax);
  PlanningLP plp(std::move(planning));
  auto status = plp.resolve();
  REQUIRE(status.has_value());
  REQUIRE(*status == 1);  // optimal
  return prob
      * plp.system(first_scene_index(), PhaseIndex {0})
            .linear_interface()
            .get_obj_value();
}

// ─── SDDP driver ─────────────────────────────────────────────────────────────

[[nodiscard]] SDDPOptions sc_sddp_opts(IntegerCutsMode integer_cuts,
                                       CutSharingMode cut_sharing,
                                       int max_iterations,
                                       double convergence_tol = 1.0e-9)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = convergence_tol;
  opts.stationary_tol = 0.0;  // no early stationary exit
  opts.cut_sharing = cut_sharing;
  opts.integer_cuts = integer_cuts;
  opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  opts.enable_api = false;
  return opts;
}

// ─── Dispatch collection ─────────────────────────────────────────────────────

/// One per-block sample of the committed thermal unit: its generation
/// and its commitment status u, read from the solved (scene, phase) LP.
struct ScDispatchSample
{
  std::size_t scene_pos {};
  int phase_pos {};
  double gen {};
  double status {};
};

/// Walk every (scene, phase, stage, block) of the solved planning and
/// return the thermal unit's (generation, status) samples.  The LPs
/// hold the last solve performed on each cell — always an
/// integer-feasible MIP solve when `relax = false` under a MIP backend.
[[nodiscard]] std::vector<ScDispatchSample> sc_collect_thermal(PlanningLP& plp)
{
  std::vector<ScDispatchSample> out;
  auto& sim = plp.simulation();
  for (const auto scene : iota_range<SceneIndex>(0, sim.scene_count())) {
    for (const auto phase : iota_range<PhaseIndex>(0, sim.phase_count())) {
      auto& sys = plp.systems()[scene][phase];
      sys.rebuild_collections_if_needed();

      const auto& gen_lps = sys.template elements<GeneratorLP>();
      const auto gen_it = std::ranges::find_if(
          gen_lps, [](const auto& g) { return g.uid() == Uid {2}; });
      REQUIRE(gen_it != gen_lps.end());

      const auto& sc_lps = sys.template elements<SimpleCommitmentLP>();
      REQUIRE(sc_lps.size() == 1);
      const auto& sc_lp = sc_lps.front();

      const auto& scenario = sim.scenarios()[static_cast<std::size_t>(
          sys.scene().first_scenario())];
      const auto sol = sys.linear_interface().get_col_sol();

      for (const auto& stage : sim.phases()[phase].stages()) {
        const auto& gcols = gen_it->generation_cols_at(scenario, stage);
        for (const auto& [buid, gcol] : gcols) {
          const auto ucol = sc_lp.lookup_status_col(scenario, stage, buid);
          REQUIRE(ucol.has_value());
          out.push_back(ScDispatchSample {
              .scene_pos = static_cast<std::size_t>(scene),
              .phase_pos = static_cast<int>(static_cast<std::size_t>(phase)),
              .gen = sol[gcol],
              .status = sol[*ucol],
          });
        }
      }
    }
  }
  return out;
}

/// Off-or-min invariant: u ∈ {0, 1} and g ∈ {0} ∪ [pmin, pmax] on every
/// sample; returns the number of ON samples so callers can assert the
/// check is non-vacuous.
int sc_check_off_or_min(const std::vector<ScDispatchSample>& samples)
{
  constexpr double kTol = 1e-3;
  int n_on = 0;
  for (const auto& s : samples) {
    INFO("scene=",
         s.scene_pos,
         " phase=",
         s.phase_pos,
         " gen=",
         s.gen,
         " u=",
         s.status);
    // Binary status.
    CHECK((s.status <= kTol || s.status >= 1.0 - kTol));
    if (s.status >= 0.5) {
      // ON branch: pmin ≤ g ≤ pmax — never in the (0, pmin) hole.
      CHECK(s.gen >= kScPmin - kTol);
      CHECK(s.gen <= kScPmax + kTol);
      ++n_on;
    } else {
      // OFF branch: g = 0.
      CHECK(s.gen <= kTol);
    }
  }
  return n_on;
}

// ─── Cut classification (physical row, column-agnostic) ─────────────────────

/// A stored optimality cut reduced to (rhs, reservoir coefficient) —
/// same classification as the oracle harness: α columns carry class
/// "Sddp"; the commitment binaries are block-local, never states, so
/// the only physical state is the reservoir energy.
struct ScCutView
{
  double rhs {};
  double state_coeff {};
};

[[nodiscard]] ScCutView sc_classify_cut(const SimulationLP& sim,
                                        const StoredCut& sc)
{
  std::optional<SceneIndex> scene;
  for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
    if (sim.uid_of(si) == sc.scene_uid) {
      scene = si;
      break;
    }
  }
  REQUIRE_MESSAGE(scene.has_value(), "cut scene_uid not found in simulation");

  std::optional<PhaseIndex> phase;
  for (const auto pi : iota_range<PhaseIndex>(0, sim.phase_count())) {
    if (sim.uid_of(pi) == sc.phase_uid) {
      phase = pi;
      break;
    }
  }
  REQUIRE_MESSAGE(phase.has_value(), "cut phase_uid not found in simulation");

  ScCutView view {.rhs = sc.rhs};
  const auto& svars = sim.state_variables(*scene, *phase);
  bool alpha_seen = false;
  std::size_t n_state_coeffs = 0;
  for (const auto& [col, coeff] : sc.coefficients) {
    bool matched = false;
    for (const auto& [key, svar] : svars) {
      if (svar.col() != col) {
        continue;
      }
      matched = true;
      if (key.class_name == "Sddp") {
        CHECK(coeff == doctest::Approx(1.0));
        alpha_seen = true;
      } else {
        view.state_coeff = coeff;
        ++n_state_coeffs;
      }
      break;
    }
    REQUIRE_MESSAGE(matched,
                    "cut coefficient column not present in the "
                    "state-variable registry");
  }
  REQUIRE_MESSAGE(alpha_seen, "optimality cut does not reference α");
  REQUIRE_MESSAGE(n_state_coeffs == 1,
                  "expected exactly one reservoir state coefficient");
  return view;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// 1. Element semantics under SDDP: forward dispatch is off-or-min at
// every block, across iterations, and the LP-relaxation twin genuinely
// picks integer-infeasible dispatch in (0, pmin) — the discriminating
// geometry witness.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SimpleCommitment × SDDP — dispatch is 0 or in [pmin, pmax] across "
    "iterations; the LP twin dispatches inside (0, pmin)")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping commitment semantics");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        // Across iterations: independent 1-iteration and 4-iteration
        // runs (the LP state after solve() is the final iteration's) —
        // the off-or-min invariant must hold at both cut maturities.
        for (const int iters : {1, 4}) {
          CAPTURE(iters);
          auto planning = sc_make_planning(0.6, 0.4, mip_solver);
          sc_set_scenario_inflows(planning, kScWetInflow, kScDryInflow);
          PlanningLP plp(std::move(planning));

          auto opts =
              sc_sddp_opts(IntegerCutsMode::none, CutSharingMode::none, iters);
          SDDPMethod sddp(plp, opts);
          auto results = sddp.solve();
          REQUIRE(results.has_value());
          REQUIRE_FALSE(results->empty());

          const auto samples = sc_collect_thermal(plp);
          REQUIRE_FALSE(samples.empty());
          const int n_on = sc_check_off_or_min(samples);
          // Witness geometry: the residual 60 − h < pmin forces the
          // MIP to commit at exactly pmin in EVERY block (OFF would
          // strand 10 MW at the $1000 fail cost).
          CHECK(n_on == static_cast<int>(samples.size()));
          for (const auto& s : samples) {
            CHECK(s.gen == doctest::Approx(kScPmin).epsilon(1e-3));
          }
        }

        // The LP-relaxation twin (relax = true) on the same geometry:
        // fractional u lets thermal serve the residual with
        // g ∈ (0, pmin) — the integer-infeasible dispatch that proves
        // the geometry discriminates.
        {
          auto planning = sc_make_planning(0.6,
                                           0.4,
                                           mip_solver,
                                           kScDemand,
                                           /*relax=*/true);
          sc_set_scenario_inflows(planning, kScWetInflow, kScDryInflow);
          PlanningLP plp(std::move(planning));

          auto opts =
              sc_sddp_opts(IntegerCutsMode::none, CutSharingMode::none, 4);
          SDDPMethod sddp(plp, opts);
          auto results = sddp.solve();
          REQUIRE(results.has_value());

          const auto samples = sc_collect_thermal(plp);
          REQUIRE_FALSE(samples.empty());
          constexpr double kTol = 1e-3;
          const auto in_hole = std::ranges::count_if(
              samples,
              [&](const ScDispatchSample& s)
              { return s.gen > kTol && s.gen < kScPmin - kTol; });
          INFO("LP-twin samples inside (0, pmin): ", in_hole);
          CHECK(in_hole >= 1);
        }
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — commitment semantics skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// 2. SDDP validity: LB ≤ extensive-form MIP optimum (brute-force
// monolithic MIP tail oracle) under both integer-cut modes, and the
// strengthened LB is never below the none-mode LB at convergence.
//
// Why the none-mode LB stays valid HERE: the pure-Benders backward
// path with `integer_cuts = none` reads reduced costs off a MIP —
// an unsound path in general (investigation doc §1: flat cut at the
// MIP incumbent).  On this geometry V_MIP is CONSTANT in the reservoir
// state (the dispatch is pinned to (h, t) = (20, 40) at every level of
// the box), so the flat cut at the MIP incumbent is not just valid but
// EXACT — by accident of the geometry, not by theorem.  The certified
// mode is `strengthened` (Theorem SB1); the none-mode check pins the
// empirical behaviour and the MESSAGE documents the looseness numbers
// quoted in docs/examples/sddp-simple-commitment.md.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SimpleCommitment × SDDP — LB ≤ extensive MIP optimum; strengthened "
    "LB ≥ none LB at convergence")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping LB validity oracle");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        struct SceneData
        {
          double prob {};
          double inflow {};
        };
        const std::array<SceneData, 2> scenes = {{
            {.prob = 0.6, .inflow = kScWetInflow},
            {.prob = 0.4, .inflow = kScDryInflow},
        }};

        // Extensive-form optima (persistent scenes, full horizon).
        double v_mip = 0.0;
        double v_lp = 0.0;
        for (const auto& sd : scenes) {
          v_mip += sc_tail_value(kScNumPhases,
                                 sd.prob,
                                 sd.inflow,
                                 kScEini,
                                 mip_solver,
                                 kScDemand,
                                 /*relax=*/false);
          v_lp += sc_tail_value(kScNumPhases,
                                sd.prob,
                                sd.inflow,
                                kScEini,
                                mip_solver,
                                kScDemand,
                                /*relax=*/true);
        }
        REQUIRE(v_mip > 0.0);
        // The witness geometry has a genuine integrality gap.
        REQUIRE(v_mip > v_lp + sc_tol(v_mip));

        constexpr int kIters = 5;
        auto run_mode =
            [&](IntegerCutsMode mode) -> std::vector<SDDPIterationResult>
        {
          auto planning = sc_make_planning(0.6, 0.4, mip_solver);
          sc_set_scenario_inflows(planning, kScWetInflow, kScDryInflow);
          PlanningLP plp(std::move(planning));
          auto opts = sc_sddp_opts(mode, CutSharingMode::none, kIters);
          SDDPMethod sddp(plp, opts);
          auto results = sddp.solve();
          REQUIRE(results.has_value());
          REQUIRE_FALSE(results->empty());
          return *results;
        };

        const auto none = run_mode(IntegerCutsMode::none);
        const auto strengthened = run_mode(IntegerCutsMode::strengthened);

        const double lb_tol = 2.0 * sc_tol(std::abs(v_mip));
        for (const auto& ir : none) {
          INFO("none iter=",
               static_cast<int>(ir.iteration_index),
               " LB=",
               ir.lower_bound,
               " v_mip=",
               v_mip);
          CHECK(ir.lower_bound <= v_mip + lb_tol);
        }
        for (const auto& ir : strengthened) {
          INFO("strengthened iter=",
               static_cast<int>(ir.iteration_index),
               " LB=",
               ir.lower_bound,
               " v_mip=",
               v_mip);
          CHECK(ir.lower_bound <= v_mip + lb_tol);
        }

        // Tightening.  Corollary SB2 orders the strengthened intercept
        // against the LP-RELAXATION cut, not against the legacy
        // mirror-based cut the none mode emits — and on this fixture
        // V_MIP is state-CONSTANT (the dispatch is pinned at every
        // reservoir level), so the legacy flat cut at the MIP
        // incumbent is accidentally EXACT and the none-mode LB hits
        // the optimum from iteration 1 (observed: none = 25200 at
        // i1 while strengthened = 23256, still converging along its
        // sloped LP cuts).  The theorem-backed run-level statement is
        // therefore at CONVERGENCE: the strengthened LB ends no lower
        // than the none LB, and both end at the MIP optimum here.
        // The per-iteration ordering stays as a WARN-level probe.
        REQUIRE(none.size() == strengthened.size());
        for (std::size_t i = 0; i < none.size(); ++i) {
          INFO("iter=",
               i,
               " none=",
               none[i].lower_bound,
               " strengthened=",
               strengthened[i].lower_bound);
          WARN(strengthened[i].lower_bound >= none[i].lower_bound - lb_tol);
        }
        CHECK(strengthened.back().lower_bound
              >= none.back().lower_bound - lb_tol);
        // On this state-uniform-gap geometry both modes certify the
        // full MIP optimum at convergence (looseness 0 — the flat
        // legacy cut and the max(b_LP, m*) strengthened intercept are
        // both exact supports of the constant V_MIP).
        CHECK(strengthened.back().lower_bound
              == doctest::Approx(v_mip).epsilon(1e-4));
        CHECK(none.back().lower_bound == doctest::Approx(v_mip).epsilon(1e-4));

        // Document the looseness (user doc numbers): none-mode cuts
        // support the convexified tails at best, so its LB gap to the
        // MIP optimum includes the integrality gap v_mip − v_lp.
        MESSAGE("extensive MIP optimum      = ", v_mip);
        MESSAGE("extensive LP relaxation    = ", v_lp);
        MESSAGE("integrality gap            = ", v_mip - v_lp);
        MESSAGE("final LB (integer_cuts=none)         = ",
                none.back().lower_bound,
                "  (looseness ",
                v_mip - none.back().lower_bound,
                ")");
        MESSAGE("final LB (integer_cuts=strengthened) = ",
                strengthened.back().lower_bound,
                "  (looseness ",
                v_mip - strengthened.back().lower_bound,
                ")");
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — LB validity oracle skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// 3. Commitment × cut_sharing: none and multicut on the same fixture.
// Bounds sanity follows the existing conventions
// (test_sddp_bounds_sanity.cpp / ledger F9): strict CHECK for
// cut_sharing=none and for multicut on IDENTICAL scenes; WARN-only for
// multicut on heterogeneous scenes (resampled-LB vs persistent-UB
// process mismatch, Corollary M2).  The off-or-min invariant is strict
// everywhere — cut sharing must never break integer feasibility.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SimpleCommitment × cut_sharing — off-or-min holds under none and "
    "multicut; bounds sanity per convention")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping cut-sharing tier");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        struct ModeCase
        {
          const char* label;
          CutSharingMode mode;
          double wet;
          double dry;
          bool strict_bounds;
        };
        const std::array<ModeCase, 3> cases = {{
            {"none heterogeneous",
             CutSharingMode::none,
             kScWetInflow,
             kScDryInflow,
             true},
            {"multicut heterogeneous (WARN bounds)",
             CutSharingMode::multicut,
             kScWetInflow,
             kScDryInflow,
             false},
            {"multicut identical scenes",
             CutSharingMode::multicut,
             kScDryInflow,
             kScDryInflow,
             true},
        }};

        for (const auto& mc : cases) {
          SUBCASE(mc.label)
          {
            auto planning = sc_make_planning(0.5, 0.5, mip_solver);
            sc_set_scenario_inflows(planning, mc.wet, mc.dry);
            PlanningLP plp(std::move(planning));

            // Strengthened cuts: the certified mode on integer cells
            // (Theorem SB1), so LB ≤ optimum ≤ UB is assertable.
            auto opts = sc_sddp_opts(
                IntegerCutsMode::strengthened, mc.mode, /*max_iterations=*/4);
            SDDPMethod sddp(plp, opts);
            auto results = sddp.solve();
            REQUIRE(results.has_value());
            REQUIRE_FALSE(results->empty());

            for (const auto& ir : *results) {
              const double tol = 2.0
                  * sc_tol(std::max(std::abs(ir.upper_bound),
                                    std::abs(ir.lower_bound)));
              INFO("iter=",
                   static_cast<int>(ir.iteration_index),
                   " LB=",
                   ir.lower_bound,
                   " UB=",
                   ir.upper_bound);
              if (mc.strict_bounds) {
                CHECK(ir.lower_bound <= ir.upper_bound + tol);
              } else {
                WARN(ir.lower_bound <= ir.upper_bound + tol);
              }
            }

            // Integer feasibility of the commitment decisions is
            // strict under EVERY sharing mode.
            const auto samples = sc_collect_thermal(plp);
            REQUIRE_FALSE(samples.empty());
            const int n_on = sc_check_off_or_min(samples);
            CHECK(n_on >= 1);
          }
        }
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — cut-sharing tier skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// 4. Pure-LP degeneracy: dispatch_pmin = 0 with relax = true makes the
// SimpleCommitment element inert — the SDDP run must reproduce the
// no-commitment twin exactly (LB/UB trajectories and physical cuts).
// Feature-gate parity; runs under any LP backend (no MIP gating).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SimpleCommitment × SDDP — pmin=0 relaxed commitment ≡ no-commitment "
    "twin (LB/UB and cuts)")
{
  using CutId = std::tuple<SceneUid, PhaseUid, int>;

  struct TwinRun
  {
    std::vector<SDDPIterationResult> results;
    std::map<CutId, ScCutView> cuts;
  };

  auto run_twin = [](bool with_commitment) -> TwinRun
  {
    auto planning = with_commitment
        ? sc_make_planning(0.6,
                           0.4,
                           /*solver=*/"",
                           /*demand=*/80.0,
                           /*relax=*/true,
                           /*pmin=*/0.0)
        : make_2scene_3phase_hydro_planning(0.6, 0.4);
    PlanningLP plp(std::move(planning));

    auto opts = sc_sddp_opts(
        IntegerCutsMode::none, CutSharingMode::none, /*max_iterations=*/5);
    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    TwinRun run;
    run.results = *results;
    for (const auto& sc : sddp.stored_cuts()) {
      if (sc.type != CutType::Optimality) {
        continue;
      }
      // Classify against the run's own registry: the commitment twin
      // carries extra u columns, so raw column indices differ between
      // the twins — the physical (rhs, reservoir coeff) view is the
      // column-agnostic comparison key.
      const auto view = sc_classify_cut(plp.simulation(), sc);
      const auto id = CutId {
          sc.scene_uid,
          sc.phase_uid,
          static_cast<int>(sc.iteration_index),
      };
      REQUIRE_MESSAGE(run.cuts.emplace(id, view).second,
                      "duplicate optimality cut id in one run");
    }
    return run;
  };

  const auto twin = run_twin(/*with_commitment=*/true);
  const auto bare = run_twin(/*with_commitment=*/false);

  REQUIRE(twin.results.size() == bare.results.size());
  for (std::size_t i = 0; i < twin.results.size(); ++i) {
    INFO("iter=", i);
    CHECK(twin.results[i].lower_bound
          == doctest::Approx(bare.results[i].lower_bound).epsilon(1e-9));
    CHECK(twin.results[i].upper_bound
          == doctest::Approx(bare.results[i].upper_bound).epsilon(1e-9));
  }

  REQUIRE(twin.cuts.size() == bare.cuts.size());
  REQUIRE(twin.cuts.size() >= 2);
  for (const auto& [id, view] : bare.cuts) {
    const auto it = twin.cuts.find(id);
    REQUIRE_MESSAGE(it != twin.cuts.end(),
                    "cut id missing from the commitment twin");
    CHECK(it->second.rhs == doctest::Approx(view.rhs).epsilon(1e-9));
    CHECK(it->second.state_coeff
          == doctest::Approx(view.state_coeff).epsilon(1e-9));
  }
}

// ═════════════════════════════════════════════════════════════════════════
// 5. SDDiP user surface (a): `integer_cuts = strengthened` is INERT when
// the aperture backward pass is enabled — the strengthened path is
// wired into the pure-Benders backward pass only (investigation doc
// §6.4); aperture clones relax integrality before duals are read
// (ledger F5), so both modes must produce identical cuts and bounds.
// This pins the documented behaviour so any future aperture-path
// extension shows up as a deliberate test change.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDiP surface — integer_cuts=strengthened is inert under the "
    "aperture backward pass (identical cuts and bounds vs none)")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping aperture inertness");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        auto run_mode = [&](IntegerCutsMode mode)
            -> std::pair<std::vector<SDDPIterationResult>,
                         std::vector<StoredCut>>
        {
          auto planning = sc_make_planning(0.6, 0.4, mip_solver);
          sc_set_scenario_inflows(planning, kScWetInflow, kScDryInflow);
          PlanningLP plp(std::move(planning));

          auto opts = sc_sddp_opts(mode,
                                   CutSharingMode::none,
                                   /*max_iterations=*/4);
          // Synthetic apertures, one per scenario (q = 1/N) — the
          // aperture backward pass replaces the pure-Benders one.
          opts.apertures = std::vector<Uid> {Uid {1}, Uid {2}};
          SDDPMethod sddp(plp, opts);
          auto results = sddp.solve();
          REQUIRE(results.has_value());
          REQUIRE_FALSE(results->empty());
          return {*results, sddp.stored_cuts()};
        };

        const auto [none_results, none_cuts] = run_mode(IntegerCutsMode::none);
        const auto [str_results, str_cuts] =
            run_mode(IntegerCutsMode::strengthened);

        REQUIRE(none_results.size() == str_results.size());
        for (std::size_t i = 0; i < none_results.size(); ++i) {
          INFO("iter=", i);
          CHECK(str_results[i].lower_bound
                == doctest::Approx(none_results[i].lower_bound).epsilon(1e-9));
          CHECK(str_results[i].upper_bound
                == doctest::Approx(none_results[i].upper_bound).epsilon(1e-9));
        }

        REQUIRE_FALSE(none_cuts.empty());
        REQUIRE(none_cuts.size() == str_cuts.size());
        for (std::size_t i = 0; i < none_cuts.size(); ++i) {
          const auto& a = none_cuts[i];
          const auto& b = str_cuts[i];
          INFO("cut #", i);
          CHECK(a.type == b.type);
          CHECK(a.scene_uid == b.scene_uid);
          CHECK(a.phase_uid == b.phase_uid);
          CHECK(a.rhs == doctest::Approx(b.rhs).epsilon(1e-9));
          REQUIRE(a.coefficients.size() == b.coefficients.size());
          for (std::size_t j = 0; j < a.coefficients.size(); ++j) {
            CHECK(a.coefficients[j].first == b.coefficients[j].first);
            CHECK(a.coefficients[j].second
                  == doctest::Approx(b.coefficients[j].second).epsilon(1e-9));
          }
        }
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — aperture inertness skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// 6. SDDiP user surface (b): planning-level option round-trip — the
// full PlanningOptions JSON (not just SddpOptions) carries
// `sddp_options.integer_cuts_mode` through parse → serialize → parse,
// and PlanningOptionsLP resolves it into the enum the method consumes.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "SDDiP surface — integer_cuts_mode round-trips through "
    "PlanningOptions JSON and resolves via PlanningOptionsLP")  // NOLINT
{
  SUBCASE("parse + round-trip at planning level")
  {
    const std::string_view js =
        R"({"sddp_options": {"integer_cuts_mode": "strengthened"}})";
    const auto opts = daw::json::from_json<PlanningOptions>(js);
    CHECK((opts.sddp_options.integer_cuts_mode
           && *opts.sddp_options.integer_cuts_mode
               == IntegerCutsMode::strengthened));

    const std::string round = daw::json::to_json(opts);
    CHECK(round.find(R"("integer_cuts_mode":"strengthened")")
          != std::string::npos);
    const auto rt =
        daw::json::from_json<PlanningOptions>(std::string_view {round});
    CHECK((rt.sddp_options.integer_cuts_mode
           && *rt.sddp_options.integer_cuts_mode
               == IntegerCutsMode::strengthened));
  }

  SUBCASE("absent field stays unset and resolves to the none default")
  {
    const std::string_view js = R"({"sddp_options": {"max_iterations": 3}})";
    const auto opts = daw::json::from_json<PlanningOptions>(js);
    CHECK_FALSE(opts.sddp_options.integer_cuts_mode.has_value());

    const PlanningOptionsLP lp_opts(opts);
    CHECK(lp_opts.sddp_integer_cuts_mode_enum() == IntegerCutsMode::none);
  }

  SUBCASE("PlanningOptionsLP resolves the set value")
  {
    PlanningOptions opts;
    opts.sddp_options.integer_cuts_mode = IntegerCutsMode::strengthened;
    const PlanningOptionsLP lp_opts(opts);
    CHECK(lp_opts.sddp_integer_cuts_mode_enum()
          == IntegerCutsMode::strengthened);
  }

  SUBCASE("bogus name is rejected at ingestion")
  {
    const std::string_view js =
        R"({"sddp_options": {"integer_cuts_mode": "lagrangian"}})";
    CHECK_THROWS((void)daw::json::from_json<PlanningOptions>(js));
  }
}
