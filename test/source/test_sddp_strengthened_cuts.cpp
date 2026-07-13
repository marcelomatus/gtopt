// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_strengthened_cuts.cpp
 * @brief     Strengthened Benders cuts (integer_cuts_mode = strengthened):
 *            unit-level Lagrangian-intercept checks plus the extensive-form
 *            MIP oracle (deliverable 2 of the SDDiP campaign).
 * @date      2026-07-08
 *
 * Three certification tiers, mirroring `test_sddp_cut_oracle.cpp`:
 *
 *  (b) **Unit tier** — `build_strengthened_benders_cut` on a hand-built
 *      5-column MIP with a known LP-relaxation cut (b_LP = 3, slope
 *      λ = −3) and a known Lagrangian intercept (m* = 5): asserts the
 *      exact tightening ordering b_LP ≤ m* ≤ V_MIP (Theorem SB1 /
 *      Corollary SB2 in
 *      docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md).
 *
 *  (a) **Oracle tier** — the 2-scene 3-phase hydro fixture with a
 *      BINARY thermal commitment (dispatch_pmin = 40 MW), solved with
 *      `cut_sharing = none` + `integer_cuts = strengthened`.  The truth
 *      is the persistent-scene tail solved monolithically as ONE MIP
 *      (integers kept — the extensive form), swept across the reservoir
 *      state box.  Every stored optimality cut must underestimate the
 *      MIP tail (strict, theorem N1 composed with SB1), and at least
 *      one cut must exceed the LP-RELAXATION tail somewhere — the
 *      witness that the intercept genuinely tightened beyond anything
 *      an LP-relaxation cut can certify.
 *
 *  (c) **Off ≡ legacy** — on a pure-LP fixture the option is inert:
 *      a `strengthened` run produces bit-equal cuts and LB trajectory
 *      to a `none` run (the gate `has_integer_cols()` never fires).
 *
 * MIP-dependent tests pin `solver_test::first_mip_solver()` into the
 * fixture (ambient GTOPT_SOLVER=clp would refuse integer columns) and
 * skip cleanly when no exact MIP backend is loaded, wrapped in
 * `run_or_skip_license` per the unit-commitment test conventions.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"
#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Geometry shared with test_sddp_cut_oracle.cpp ──────────────────────────
constexpr int kNumPhases = 3;
constexpr int kBlocksPerStage = 4;
constexpr double kEmax = 200.0;  // reservoir state box is [0, emax]
constexpr double kEini = 100.0;
constexpr double kBaseInflow = 8.0;
constexpr double kWetInflow = 11.0;
constexpr double kDryInflow = 5.0;

// Binary thermal commitment (dispatch_pmin, relax = false).  Two
// geometries are used below:
//
//  * **Witness geometry** (oracle test): demand 60, inflow ≥ 22 dam³/h.
//    Per-stage water (4 h × inflow ≥ 88 dam³) always lets hydro exceed
//    20 MW, so the thermal residual 60 − h < 40 = pmin at EVERY
//    reservoir state — the MIP dispatch is pinned to (h = 20, t = 40)
//    while the LP relaxation commits fractionally (t = 60 − h,
//    u = t/200).  The integrality gap is state-uniform, so the
//    Lagrangian intercept strictly exceeds the LP intercept and the
//    strengthened cut beats the LP-relaxation tail (the SB2 witness).
//
//  * **Convergence geometry** (identical-scenes test): the base
//    fixture (demand 80, inflow 8).  There the visited states sit in
//    the region where pmin is slack (residual 80 − h ≥ 40), V_MIP
//    coincides with V_LP along the optimal path, and the strengthened
//    cuts are exact supports — LB/UB close onto the extensive MIP
//    optimum.
constexpr double kDispatchPmin = 40.0;
constexpr double kWitnessDemand = 60.0;
constexpr double kWitnessWetInflow = 28.0;
constexpr double kWitnessDryInflow = 22.0;

// ─── Tolerances ──────────────────────────────────────────────────────────────
//
// Solver feasibility noise on both the cut solves and the oracle tail
// (1e-6 relative), plus the strengthening-MIP gap residual (the
// implementation pins mip_gap = 1e-9 / mip_gap_abs = 1e-6 on the
// Lagrangian solve; a positive gap over-tightens by up to gap·|m*| —
// investigation doc §6.3) and the oracle tail's own mip_gap.
[[nodiscard]] double mip_oracle_tol(double v_abs)
{
  constexpr double kSolverFeasTol = 1.0e-6;
  constexpr double kMipGapAllowance = 2.0e-6;
  return (kSolverFeasTol + kMipGapAllowance) * std::max(1.0, v_abs);
}

constexpr std::size_t kGridN = 9;  // 0, 25, …, 200

[[nodiscard]] std::array<double, kGridN> state_grid()
{
  std::array<double, kGridN> grid {};
  for (std::size_t i = 0; i < kGridN; ++i) {
    grid[i] = kEmax * static_cast<double>(i) / static_cast<double>(kGridN - 1);
  }
  return grid;
}

// ─── Fixtures ────────────────────────────────────────────────────────────────

/// The 2-scene 3-phase hydro fixture plus a binary commitment on the
/// thermal unit; `relax = true` builds the pure-LP twin used by the
/// LP-relaxation tail witness.  A MIP-capable solver must be pinned —
/// LP-only backends refuse integer columns at `load_flat`.
[[nodiscard]] Planning make_commitment_planning(double p0,
                                                double p1,
                                                const std::string& solver,
                                                double demand = 80.0,
                                                bool relax = false)
{
  auto planning = make_2scene_3phase_hydro_planning(p0, p1);
  planning.system.demand_array[0].capacity = demand;
  planning.system.simple_commitment_array = {
      {
          .uid = Uid {1},
          .name = "sc_thermal",
          .generator = Uid {2},
          .dispatch_pmin = kDispatchPmin,
          .relax = relax,
      },
  };
  planning.options.lp_matrix_options.solver_name = solver;
  // Tight MIP gap so forward/backward MIP objectives are near-exact.
  planning.options.solver_options.mip_gap = 1e-9;
  planning.options.solver_options.mip_gap_abs = 1e-7;
  return planning;
}

/// Give each scenario its own constant inflow (heterogeneous scenes).
void set_scenario_inflows(Planning& planning, double q0, double q1)
{
  std::vector<std::vector<std::vector<double>>> sched;
  sched.reserve(2);
  for (const double q : {q0, q1}) {
    std::vector<std::vector<double>> per_stage;
    per_stage.reserve(static_cast<std::size_t>(kNumPhases));
    for (int st = 0; st < kNumPhases; ++st) {
      per_stage.push_back(
          std::vector<double>(static_cast<std::size_t>(kBlocksPerStage), q));
    }
    sched.push_back(std::move(per_stage));
  }
  planning.system.flow_array[0].discharge = STBRealFieldSched {sched};
}

/// Single-scenario tail Planning (the last @p num_stages stages merged
/// into one phase — the extensive form), with the SAME binary thermal
/// commitment as the SDDP fixture; `relax = true` yields the
/// LP-relaxation twin.  Mirrors `oracle_make_tail_planning` in
/// test_sddp_cut_oracle.cpp (unfolded — p_s applied externally).
[[nodiscard]] Planning make_tail_planning(int num_stages,
                                          double inflow,
                                          double eini,
                                          const std::string& solver,
                                          double demand,
                                          bool relax)
{
  auto block_array = make_uniform_blocks(
      static_cast<std::size_t>(num_stages * kBlocksPerStage), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages),
                          static_cast<std::size_t>(kBlocksPerStage));

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
          .capacity = 200.0,
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
          .capacity = kEmax,
          .emin = 0.0,
          .emax = kEmax,
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
          .dispatch_pmin = kDispatchPmin,
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
              .name = "sddp_strengthened_tail",
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
/// its physical objective folded by @p prob externally (same convention
/// as `oracle_tail_value` — the lone scenario's probability_factor is
/// normalized away at LP build time).
[[nodiscard]] double tail_value(int num_stages,
                                double prob,
                                double inflow,
                                double eini,
                                const std::string& solver,
                                double demand,
                                bool relax)
{
  auto planning =
      make_tail_planning(num_stages, inflow, eini, solver, demand, relax);
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

[[nodiscard]] SDDPOptions sddp_opts(IntegerCutsMode integer_cuts,
                                    int max_iterations,
                                    double convergence_tol = 1.0e-9)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = convergence_tol;
  opts.stationary_tol = 0.0;  // no early stationary exit — audit more cuts
  opts.cut_sharing = CutSharingMode::none;
  opts.integer_cuts = integer_cuts;
  opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  opts.enable_api = false;
  return opts;
}

/// A stored optimality cut reduced to oracle coordinates (same
/// classification as test_sddp_cut_oracle.cpp: α columns carry class
/// "Sddp"; the only physical state is the reservoir energy — the
/// commitment binaries are block-local, never states).
struct CutView
{
  std::size_t scene_pos {};
  int phase_pos {};
  double rhs {};
  double state_coeff {};
};

[[nodiscard]] CutView classify_cut(const SimulationLP& sim, const StoredCut& sc)
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

  CutView view {
      .scene_pos = static_cast<std::size_t>(*scene),
      .phase_pos = static_cast<int>(static_cast<std::size_t>(*phase)),
  };
  view.rhs = sc.rhs;

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

/// Cut bound at state x:  α ≥ rhs − coeff·x  (row is α + coeff·x ≥ rhs).
[[nodiscard]] double cut_value_at(const CutView& view, double x)
{
  return view.rhs - view.state_coeff * x;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// (b) Unit tier: hand-checkable Lagrangian intercept.
//
// Target subproblem (state z pinned at v̂ = 0.4, box [0, 0.5]):
//
//     min  0·h + 1·p + 10·q + 4·w
//     s.t. h + p + q = 1                (demand)
//          h − z ≤ 0                    (state cap)
//          p − 2w ≤ 0                   (commitment cap)
//          h ∈ [0, 1], p ∈ [0, 2], q ∈ [0, 0.3], w ∈ {0, 1}
//
// LP relaxation at v̂: (h, p, w, q) = (0.4, 0.6, 0.3, 0) → z*_LP = 1.8,
// λ = rc(z) = −3 (V_LP(x) = 3 − 3x), so b_LP = 1.8 + 3·0.4 = 3.0.
// Lagrangian MIP (z ∈ [0, 0.5] charged +3, w binary): w = 0 is
// infeasible (h + q ≤ 0.8 < 1), w = 1 → p = 1, z = 0 → m* = 5.0.
// Truth: V_MIP(x) = 5 − x on the box, strictly above the LP support
// 3 − 3x everywhere — strict tightening with equality of the
// strengthened cut 5 − 3x at x = 0.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "build_strengthened_benders_cut — hand-checkable Lagrangian intercept "
    "(b_LP=3, m*=5)")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping strengthened-cut unit");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        LinearProblem lp("strengthened_unit");
        const auto z = lp.add_col(SparseCol {
            .lowb = 0.4,
            .uppb = 0.4,
        });
        const auto h = lp.add_col(SparseCol {
            .lowb = 0.0,
            .uppb = 1.0,
        });
        const auto p = lp.add_col(SparseCol {
            .lowb = 0.0,
            .uppb = 2.0,
            .cost = 1.0,
        });
        const auto q = lp.add_col(SparseCol {
            .lowb = 0.0,
            .uppb = 0.3,
            .cost = 10.0,
        });
        const auto w = lp.add_col(SparseCol {
            .lowb = 0.0,
            .uppb = 1.0,
            .cost = 4.0,
            .is_integer = true,
        });

        auto demand = SparseRow {.lowb = 1.0, .uppb = 1.0};
        demand[h] = 1.0;
        demand[p] = 1.0;
        demand[q] = 1.0;
        std::ignore = lp.add_row(std::move(demand));

        auto state_cap = SparseRow {
            .lowb = -LinearProblem::DblMax,
            .uppb = 0.0,
        };
        state_cap[h] = 1.0;
        state_cap[z] = -1.0;
        std::ignore = lp.add_row(std::move(state_cap));

        auto commit_cap = SparseRow {
            .lowb = -LinearProblem::DblMax,
            .uppb = 0.0,
        };
        commit_cap[p] = 1.0;
        commit_cap[w] = -2.0;
        std::ignore = lp.add_row(std::move(commit_cap));

        LinearInterface li(mip_solver, lp.flatten({}));
        REQUIRE(li.has_integer_cols());

        // Source-phase identities: arbitrary distinct indices — the cut
        // row is inspected directly, never installed.
        const ColIndex alpha_col {50};
        const ColIndex source_col {51};

        const StateVariable::LPKey key {
            .scene_index = SceneIndex {0},
            .phase_index = PhaseIndex {0},
        };
        StateVariable svar(key, source_col, /*scost=*/0.0, 1.0, LpContext {});
        svar.set_col_sol(0.4);  // v̂ (var_scale = 1 → physical 0.4)

        std::vector<StateVarLink> links = {
            {
                .source_col = source_col,
                .dependent_col = z,
                .trial_value = 0.4,
                .source_low = 0.0,
                .source_upp = 0.5,
                .var_scale = 1.0,
                .state_var = &svar,
            },
        };

        SolverOptions lp_opts;
        SolverOptions mip_opts;
        mip_opts.mip_gap = 1e-9;
        mip_opts.mip_gap_abs = 1e-9;

        const auto result = build_strengthened_benders_cut(
            alpha_col, links, li, /*cut_coeff_eps=*/1e-9, lp_opts, mip_opts);

        REQUIRE(result.has_value());
        // LP-relaxation baseline: b_LP = 3, slope −λ = +3.
        CHECK(result->lp_rhs == doctest::Approx(3.0).epsilon(1e-6));
        CHECK(result->row.cmap.at(alpha_col) == doctest::Approx(1.0));
        CHECK(result->row.cmap.at(source_col)
              == doctest::Approx(3.0).epsilon(1e-6));
        // Strengthened intercept: m* = 5, strictly tighter (SB2).
        CHECK(result->tightened);
        CHECK(result->mip_rhs == doctest::Approx(5.0).epsilon(1e-6));
        CHECK(result->row.lowb == doctest::Approx(5.0).epsilon(1e-6));
        CHECK(result->row.lowb >= result->lp_rhs);

        // Validity sweep (Theorem SB1): V_MIP(x) = 5 − x on the box;
        // the cut asserts α ≥ 5 − 3x ≤ V_MIP(x) for all x ∈ [0, 0.5].
        for (const double x : {0.0, 0.1, 0.25, 0.4, 0.5}) {
          const double cut_val =
              result->row.lowb - result->row.cmap.at(source_col) * x;
          const double v_mip = 5.0 - x;
          CHECK(cut_val <= v_mip + 1e-6);
        }
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — strengthened-cut unit skipped");
  }
}

TEST_CASE(  // NOLINT
    "build_strengthened_benders_cut — pure-LP target returns nullopt")
{
  // No integer columns → the builder must decline so the caller keeps
  // the legacy cut path (the SDDP-side gate checks has_integer_cols()
  // first; this pins the defensive in-function gate).
  LinearProblem lp("strengthened_lp_only");
  const auto z = lp.add_col(SparseCol {
      .lowb = 0.4,
      .uppb = 0.4,
  });
  const auto y = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 2.0,
      .cost = 1.0,
  });
  auto row = SparseRow {.lowb = 1.0};
  row[y] = 1.0;
  row[z] = 1.0;
  std::ignore = lp.add_row(std::move(row));

  LinearInterface li("", lp.flatten({}));
  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {51},
          .dependent_col = z,
          .trial_value = 0.4,
          .source_low = 0.0,
          .source_upp = 0.5,
      },
  };
  const auto result = build_strengthened_benders_cut(ColIndex {50},
                                                     links,
                                                     li,
                                                     /*cut_coeff_eps=*/0.0,
                                                     SolverOptions {},
                                                     SolverOptions {});
  CHECK_FALSE(result.has_value());
}

// ═════════════════════════════════════════════════════════════════════════
// (a) Oracle tier: strengthened cuts underestimate the extensive-form
// MIP tails at every swept state (cut_sharing = none — theorem N1
// composed with SB1), and at least one cut exceeds the LP-relaxation
// tail somewhere (the tightening witness).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP strengthened cuts — MIP tail oracle: cuts underestimate the "
    "per-scene MIP tails and beat the LP tails somewhere")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping MIP oracle");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        auto planning =
            make_commitment_planning(0.6, 0.4, mip_solver, kWitnessDemand);
        set_scenario_inflows(planning, kWitnessWetInflow, kWitnessDryInflow);
        PlanningLP plp(std::move(planning));

        auto opts = sddp_opts(IntegerCutsMode::strengthened,
                              /*max_iterations=*/5);
        SDDPMethod sddp(plp, opts);
        auto results = sddp.solve();
        REQUIRE(results.has_value());
        REQUIRE_FALSE(results->empty());

        struct SceneData
        {
          double prob {};
          double inflow {};
        };
        const std::array<SceneData, 2> scenes = {{
            {.prob = 0.6, .inflow = kWitnessWetInflow},
            {.prob = 0.4, .inflow = kWitnessDryInflow},
        }};

        const auto grid = state_grid();

        // Tail caches per (tail length, scene): MIP truth and the
        // LP-relaxation twin (for the tightening witness).
        std::map<std::pair<int, std::size_t>, std::array<double, kGridN>>
            mip_cache;
        std::map<std::pair<int, std::size_t>, std::array<double, kGridN>>
            lp_cache;
        const auto tails_for =
            [&](int tail_stages,
                std::size_t scene_pos,
                bool relax) -> const std::array<double, kGridN>&
        {
          auto& cache = relax ? lp_cache : mip_cache;
          const auto key = std::pair {tail_stages, scene_pos};
          auto it = cache.find(key);
          if (it == cache.end()) {
            std::array<double, kGridN> vals {};
            for (std::size_t i = 0; i < kGridN; ++i) {
              vals[i] = tail_value(tail_stages,
                                   scenes[scene_pos].prob,
                                   scenes[scene_pos].inflow,
                                   grid[i],
                                   mip_solver,
                                   kWitnessDemand,
                                   relax);
            }
            it = cache.emplace(key, vals).first;
          }
          return it->second;
        };

        int n_checked = 0;
        int n_lp_beaten = 0;  // grid points where a cut exceeds the LP tail
        for (const auto& sc : sddp.stored_cuts()) {
          if (sc.type != CutType::Optimality) {
            continue;
          }
          const auto view = classify_cut(plp.simulation(), sc);
          const int tail_stages = kNumPhases - 1 - view.phase_pos;
          REQUIRE(tail_stages >= 1);

          const auto& v_mip = tails_for(tail_stages, view.scene_pos, false);
          const auto& v_lp = tails_for(tail_stages, view.scene_pos, true);
          for (std::size_t i = 0; i < kGridN; ++i) {
            const double cut_val = cut_value_at(view, grid[i]);
            const double tol = mip_oracle_tol(std::abs(v_mip[i]));
            INFO("scene=",
                 view.scene_pos,
                 " phase=",
                 view.phase_pos,
                 " iter=",
                 static_cast<int>(sc.iteration_index),
                 " x=",
                 grid[i],
                 " cut=",
                 cut_val,
                 " V_mip=",
                 v_mip[i],
                 " V_lp=",
                 v_lp[i]);
            // (a) validity against the MIP truth — strict.
            CHECK(cut_val <= v_mip[i] + tol);
            // (b)-witness: count states where the cut is strictly above
            // the LP-relaxation tail (impossible for a valid LP cut).
            if (cut_val > v_lp[i] + tol) {
              ++n_lp_beaten;
            }
          }
          ++n_checked;
        }
        CAPTURE(n_checked);
        REQUIRE(n_checked >= 2);
        // Witness geometry: the state-uniform integrality gap makes
        // V_MIP sit strictly above V_LP across the whole box, so the
        // Lagrangian intercept must lift at least one cut above the
        // LP-relaxation tail (impossible for any valid LP cut — the
        // SB2 tightening witness at SDDP level).
        CHECK(n_lp_beaten >= 1);

        // LB property: never above the persistent extensive MIP optimum.
        double extensive_mip = 0.0;
        for (const auto& sd : scenes) {
          extensive_mip += tail_value(kNumPhases,
                                      sd.prob,
                                      sd.inflow,
                                      kEini,
                                      mip_solver,
                                      kWitnessDemand,
                                      /*relax=*/false);
        }
        const double lb_tol = 2.0 * mip_oracle_tol(std::abs(extensive_mip));
        for (const auto& ir : *results) {
          INFO("iter=",
               static_cast<int>(ir.iteration_index),
               " LB=",
               ir.lower_bound,
               " extensive_mip=",
               extensive_mip);
          CHECK(ir.lower_bound <= extensive_mip + lb_tol);
        }
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — MIP oracle skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// Convergence tier (demonstrative + valid brackets): identical scenes —
// the strengthened cuts support V_MIP exactly on this fixture (the
// integrality gap is state-independent), so LB/UB must bracket and
// approach the extensive MIP optimum.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP strengthened cuts — identical scenes converge toward the "
    "extensive MIP optimum")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping convergence tier");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        auto planning = make_commitment_planning(0.5, 0.5, mip_solver);
        PlanningLP plp(std::move(planning));

        auto opts = sddp_opts(IntegerCutsMode::strengthened,
                              /*max_iterations=*/25,
                              /*convergence_tol=*/1.0e-6);
        SDDPMethod sddp(plp, opts);
        auto results = sddp.solve();
        REQUIRE(results.has_value());
        REQUIRE_FALSE(results->empty());

        // Both scenes identical (p = 0.5 each): Σ_s V_s = V_full.
        const double v_full = tail_value(kNumPhases,
                                         1.0,
                                         kBaseInflow,
                                         kEini,
                                         mip_solver,
                                         /*demand=*/80.0,
                                         /*relax=*/false);
        REQUIRE(v_full > 0.0);

        const auto& last = results->back();
        const double tol = 2.0 * mip_oracle_tol(std::abs(v_full));
        // Valid brackets: LB below, UB above the extensive optimum.
        CHECK(last.lower_bound <= v_full + tol);
        CHECK(last.upper_bound >= v_full - tol);
        // Demonstrative closeness (the fixture's V_MIP = V_LP + const
        // makes strengthened cuts exact supports).
        CHECK(last.lower_bound == doctest::Approx(v_full).epsilon(2e-2));
        CHECK(last.upper_bound == doctest::Approx(v_full).epsilon(2e-2));
      });
  if (!ran) {
    MESSAGE("MIP solver license unavailable — convergence tier skipped");
  }
}

// ═════════════════════════════════════════════════════════════════════════
// (c) Off ≡ legacy: on a pure-LP fixture the option is inert — a
// strengthened run reproduces the none run bit-for-bit (cuts and LB).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP strengthened cuts — pure-LP fixture: strengthened ≡ none "
    "(identical cuts and LB trajectory)")
{
  using CutId = std::tuple<SceneUid, PhaseUid, int>;

  struct ModeRun
  {
    std::vector<SDDPIterationResult> results;
    std::map<CutId, std::pair<double, double>> cuts;  // rhs, state coeff
  };

  auto run_mode = [](IntegerCutsMode mode) -> ModeRun
  {
    auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
    PlanningLP plp(std::move(planning));

    auto opts = sddp_opts(mode, /*max_iterations=*/5);
    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    ModeRun run;
    run.results = *results;
    for (const auto& sc : sddp.stored_cuts()) {
      if (sc.type != CutType::Optimality) {
        continue;
      }
      const auto view = classify_cut(plp.simulation(), sc);
      const auto id = CutId {
          sc.scene_uid,
          sc.phase_uid,
          static_cast<int>(sc.iteration_index),
      };
      REQUIRE_MESSAGE(
          run.cuts.emplace(id, std::pair {view.rhs, view.state_coeff}).second,
          "duplicate optimality cut id in one run");
    }
    return run;
  };

  const auto none = run_mode(IntegerCutsMode::none);
  const auto strengthened = run_mode(IntegerCutsMode::strengthened);

  REQUIRE(none.results.size() == strengthened.results.size());
  for (std::size_t i = 0; i < none.results.size(); ++i) {
    INFO("iter=", i);
    CHECK(strengthened.results[i].lower_bound
          == doctest::Approx(none.results[i].lower_bound).epsilon(1e-9));
  }

  REQUIRE(none.cuts.size() == strengthened.cuts.size());
  REQUIRE(none.cuts.size() >= 2);
  for (const auto& [id, rc] : none.cuts) {
    const auto it = strengthened.cuts.find(id);
    REQUIRE_MESSAGE(it != strengthened.cuts.end(),
                    "cut id missing from the strengthened run");
    CHECK(it->second.first == doctest::Approx(rc.first).epsilon(1e-9));
    CHECK(it->second.second == doctest::Approx(rc.second).epsilon(1e-9));
  }
}

// ═════════════════════════════════════════════════════════════════════════
// Option plumbing: enum names, JSON parse + round-trip, merge.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("IntegerCutsMode option plumbing")  // NOLINT
{
  SUBCASE("enum names")
  {
    CHECK(enum_from_name<IntegerCutsMode>("none").value_or(
              IntegerCutsMode::strengthened)
          == IntegerCutsMode::none);
    CHECK(enum_from_name<IntegerCutsMode>("strengthened")
              .value_or(IntegerCutsMode::none)
          == IntegerCutsMode::strengthened);
    CHECK_FALSE(enum_from_name<IntegerCutsMode>("bogus").has_value());
    CHECK(enum_name(IntegerCutsMode::none) == "none");
    CHECK(enum_name(IntegerCutsMode::strengthened) == "strengthened");
  }

  SUBCASE("SddpOptions default is unset")
  {
    const SddpOptions opts {};
    CHECK_FALSE(opts.integer_cuts_mode.has_value());
  }

  SUBCASE("JSON parse + round-trip")
  {
    const std::string_view js = R"({"integer_cuts_mode": "strengthened"})";
    const auto so = daw::json::from_json<SddpOptions>(js);
    CHECK((so.integer_cuts_mode
           && *so.integer_cuts_mode == IntegerCutsMode::strengthened));

    const std::string round = daw::json::to_json(so);
    const auto rt = daw::json::from_json<SddpOptions>(std::string_view {round});
    CHECK((rt.integer_cuts_mode
           && *rt.integer_cuts_mode == IntegerCutsMode::strengthened));
  }

  SUBCASE("JSON without the field stays unset")
  {
    const std::string_view js = R"({"max_iterations": 3})";
    const auto so = daw::json::from_json<SddpOptions>(js);
    CHECK_FALSE(so.integer_cuts_mode.has_value());
  }

  SUBCASE("merge: incoming wins")
  {
    SddpOptions base {};
    SddpOptions over {};
    over.integer_cuts_mode = IntegerCutsMode::strengthened;
    base.merge(std::move(over));
    CHECK((base.integer_cuts_mode
           && *base.integer_cuts_mode == IntegerCutsMode::strengthened));
  }

  SUBCASE("resolved SDDPOptions default is none")
  {
    const SDDPOptions opts {};
    CHECK(opts.integer_cuts == IntegerCutsMode::none);
  }
}

// ═════════════════════════════════════════════════════════════════════════
// T5 (coverage-audit G5): strengthened cuts through the Parquet codec.
// The strengthened intercept `max(b_LP, m*)` is the whole point of the
// mode — any precision loss in the cut file silently degrades a
// hot-start back toward the LP-relaxation intercept.  Save a trained
// strengthened cut set, reload into a fresh identical planning via the
// canonical hot-start path (`SDDPMethod::load_cuts`), and assert every
// cut's rhs and coefficient multiset survive bit-tight (Parquet stores
// raw float64 — exact by construction; this guards the writer/loader
// against a narrowing cast).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP strengthened cuts — Parquet round-trip preserves the tightened "
    "intercept")
{
  const auto mip_solver = solver_test::first_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("no exact MIP solver loaded — skipping strengthened round-trip");
    return;
  }

  const bool ran = solver_test::run_or_skip_license(
      [&]
      {
        const auto cuts_file =
            (std::filesystem::temp_directory_path()
             / "gtopt_test_strengthened_cut_roundtrip.parquet")
                .string();

        auto planning =
            make_commitment_planning(0.6, 0.4, mip_solver, kWitnessDemand);
        set_scenario_inflows(planning, kWitnessWetInflow, kWitnessDryInflow);
        PlanningLP plp(std::move(planning));
        SDDPMethod sddp(
            plp, sddp_opts(IntegerCutsMode::strengthened, /*max_iter=*/3));
        auto results = sddp.solve();
        REQUIRE(results.has_value());
        const auto orig = sddp.stored_cuts();
        REQUIRE_FALSE(orig.empty());
        REQUIRE(save_cuts_parquet(orig, plp, cuts_file).has_value());

        // Match on the (type, scene, phase, iteration) 4-tuple — NOT
        // the full `CutKey`: the loader deliberately re-encodes the
        // `extra` discriminator as the origin-encoded broadcast
        // context (`ctx_extra = extra·N + origin`,
        // `sddp_cut_parquet.cpp`).  Unique per cut on this fixture.
        using RoundTripKey = std::tuple<CutType, SceneUid, PhaseUid, Uid>;
        const auto rt_key = [](const StoredCut& sc) -> RoundTripKey
        {
          return {sc.type,
                  sc.scene_uid,
                  sc.phase_uid,
                  static_cast<Uid>(gtopt::uid_of(sc.iteration_index))};
        };
        std::map<RoundTripKey, const StoredCut*> orig_by_key;
        for (const auto& sc : orig) {
          const auto [it_ins, inserted] = orig_by_key.emplace(rt_key(sc), &sc);
          REQUIRE_MESSAGE(inserted, "4-tuple key not unique in saved set");
        }

        auto planning2 =
            make_commitment_planning(0.6, 0.4, mip_solver, kWitnessDemand);
        set_scenario_inflows(planning2, kWitnessWetInflow, kWitnessDryInflow);
        PlanningLP plp2(std::move(planning2));
        SDDPMethod sddp2(
            plp2, sddp_opts(IntegerCutsMode::strengthened, /*max_iter=*/1));
        REQUIRE(sddp2.ensure_initialized().has_value());
        const auto loaded = sddp2.load_cuts(cuts_file);
        REQUIRE(loaded.has_value());

        const auto rt = sddp2.stored_cuts();
        REQUIRE(rt.size() == orig.size());
        std::size_t matched = 0;
        for (const auto& sc : rt) {
          const auto it = orig_by_key.find(rt_key(sc));
          REQUIRE_MESSAGE(it != orig_by_key.end(),
                          "reloaded cut key not present in the saved set");
          const auto& ov = *it->second;
          // The strengthened intercept must survive bit-tight — this is
          // exactly the value `max(b_LP, m*)` tightened.  Coefficients
          // are compared as SPARSE maps (absent ⇒ 0.0): the loader's
          // noise filter legitimately drops exactly-zero coefficients
          // (e.g. a 0 state slope on a flat fixture), which is
          // value-identical, not precision loss.  Identical planning
          // construction → identical column order, so ColIndex keys
          // are comparable across the two runs.
          CHECK(sc.rhs == doctest::Approx(ov.rhs).epsilon(1.0e-15));
          const auto coeff_of = [](const StoredCut& cut, ColIndex col) -> double
          {
            for (const auto& [c, v] : cut.coefficients) {
              if (c == col) {
                return v;
              }
            }
            return 0.0;
          };
          std::set<ColIndex> cols;
          for (const auto& [c, v] : sc.coefficients) {
            cols.insert(c);
          }
          for (const auto& [c, v] : ov.coefficients) {
            cols.insert(c);
          }
          for (const auto& col : cols) {
            INFO("col ", static_cast<int>(col));
            CHECK(coeff_of(sc, col)
                  == doctest::Approx(coeff_of(ov, col)).epsilon(1.0e-15));
          }
          ++matched;
        }
        CHECK(matched == orig.size());

        std::filesystem::remove(cuts_file);
      });
  if (!ran) {
    MESSAGE(
        "MIP solver license unavailable — strengthened round-trip "
        "skipped");
  }
}
