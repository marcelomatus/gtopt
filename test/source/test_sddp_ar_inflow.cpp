// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_ar_inflow.cpp
 * @brief     Opt-in AR(1) inflow states: parity, structure, and the
 *            explicit-tail exact oracle.
 * @date      2026-07-08
 *
 * Covers `docs/formulation/sddp-ar-inflows.md`:
 *
 *   * **Opt-out parity** — absent `inflow_model`, no `Flow` state
 *     variable is registered and no AR structure exists (the LP is the
 *     historical one; the full suite pins the rest).
 *   * **phi = 0 parity** — with the model present but memoryless, the
 *     SDDP run converges to the same objective as the model-free run
 *     (the AR row `q = mu` is value-identical to the bound pin).
 *   * **Exact oracle** — v1 realizes the schedule (`eps = 0`), so the
 *     AR run's optimum equals the explicit deterministic optimum with
 *     precomputed inflows.
 *   * **2-D cut validity** — every optimality cut now carries an
 *     inflow coefficient; it must underestimate the explicit AR tail
 *       V_tail(e, q) = tail LP with eini = e and stage-k inflow
 *                      mu + phi^k (q − mu)
 *     over the (reservoir energy, lagged inflow) grid — Theorems O1/O2
 *     of `docs/formulation/sddp-cut-validity.md` applied to the
 *     enlarged state.
 *   * **Aperture parity** — with synthetic apertures the AR path
 *     rewrites the AR-row RHS instead of the column bounds; at phi = 0
 *     both formulations converge to the same objective.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_flow.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#include "log_capture.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Fixture geometry (mirrors make_2scene_3phase_hydro_planning) ──────────
constexpr int kArNumPhases = 3;
constexpr int kArBlocksPerStage = 4;
constexpr double kArEmax = 200.0;
constexpr double kArEini = 100.0;
constexpr double kArWetInflow = 11.0;
constexpr double kArDryInflow = 5.0;
constexpr double kArPhi = 0.6;

// ε-validity tolerance, same shape as the base oracle harness
// (`test_sddp_cut_oracle.cpp`): solver feasibility noise dominates.
[[nodiscard]] double ar_tol(double v_abs)
{
  return 1.0e-8 * kArEmax + 1.0e-6 * std::max(1.0, v_abs);
}

/// Per-scenario constant inflows (wet / dry persistent sample paths).
void ar_set_scenario_inflows(Planning& planning, double q0, double q1)
{
  std::vector<std::vector<std::vector<double>>> sched;
  sched.reserve(2);
  for (const double q : {q0, q1}) {
    std::vector<std::vector<double>> per_stage;
    per_stage.reserve(static_cast<std::size_t>(kArNumPhases));
    for (int st = 0; st < kArNumPhases; ++st) {
      per_stage.push_back(
          std::vector<double>(static_cast<std::size_t>(kArBlocksPerStage), q));
    }
    sched.push_back(std::move(per_stage));
  }
  planning.system.flow_array[0].discharge = STBRealFieldSched {sched};
}

void ar_attach_inflow_model(Planning& planning, double phi)
{
  planning.system.flow_array[0].inflow_model = InflowModel {
      .type = Name {"ar1"},
      .phi = phi,
  };
}

/// Single-scenario tail extensive form with EXPLICIT per-stage inflows
/// (the deterministic equivalent of the AR tail probed at a lagged
/// inflow).  Mirrors the base oracle's tail maker; the lone scenario's
/// probability is normalized away at build time, so the p_s folding is
/// applied externally by the caller.
[[nodiscard]] Planning ar_make_tail_planning(
    const std::vector<double>& stage_inflows, double eini)
{
  const auto num_stages = stage_inflows.size();
  auto block_array = make_uniform_blocks(num_stages * kArBlocksPerStage, 1.0);
  auto stage_array = make_uniform_stages(
      num_stages, static_cast<std::size_t>(kArBlocksPerStage));

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

  std::vector<std::vector<std::vector<double>>> sched(1);
  sched[0].reserve(num_stages);
  for (const double q : stage_inflows) {
    sched[0].push_back(
        std::vector<double>(static_cast<std::size_t>(kArBlocksPerStage), q));
  }

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
          .capacity = 80.0,
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
          .capacity = kArEmax,
          .emin = 0.0,
          .emax = kArEmax,
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
          .discharge = STBRealFieldSched {sched},
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

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_ar_inflow_tail",
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

/// Explicit deterministic tail optimum with the AR-shifted inflows
/// `q_k = mu + phi^k (lag − mu)`, folded by @p prob externally.
[[nodiscard]] double ar_tail_value(int tail_stages,
                                   double prob,
                                   double mu,
                                   double phi,
                                   double lag,
                                   double eini)
{
  std::vector<double> inflows;
  inflows.reserve(static_cast<std::size_t>(tail_stages));
  double shift = lag - mu;
  for ([[maybe_unused]] const auto k : iota_range<int>(0, tail_stages)) {
    shift *= phi;
    inflows.push_back(mu + shift);
  }
  auto planning = ar_make_tail_planning(inflows, eini);
  PlanningLP plp(std::move(planning));
  auto status = plp.resolve();
  REQUIRE(status.has_value());
  REQUIRE(*status == 1);
  return prob
      * plp.system(first_scene_index(), PhaseIndex {0})
            .linear_interface()
            .get_obj_value();
}

[[nodiscard]] SDDPOptions ar_sddp_opts(int max_iterations,
                                       double convergence_tol = 1.0e-9,
                                       bool synthetic_apertures = false)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = convergence_tol;
  opts.stationary_tol = 0.0;
  opts.cut_sharing = CutSharingMode::none;
  if (synthetic_apertures) {
    // Cross-scenario synthetic apertures: a NON-EMPTY UID request with
    // no `simulation.aperture_array` makes `resolve_effective_apertures`
    // build synthetic apertures from the first N scenarios.  (A
    // `std::nullopt` request with empty defs silently FALLS BACK to the
    // pure-Benders backward pass — no apertures at all.)
    opts.apertures = std::vector<Uid> {Uid {1}, Uid {2}};
  } else {
    opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  }
  opts.enable_api = false;
  return opts;
}

/// A stored optimality cut in oracle coordinates: `α + c_e·e + c_q·q ≥
/// rhs`, classified through the state-variable registry.
struct ArCutView
{
  std::size_t scene_pos {};
  int phase_pos {};
  double rhs {};
  double energy_coeff {};
  double inflow_coeff {};
  bool has_inflow_coeff {false};
};

[[nodiscard]] ArCutView ar_classify_cut(const SimulationLP& sim,
                                        const StoredCut& sc)
{
  std::optional<SceneIndex> scene;
  for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
    if (sim.uid_of(si) == sc.scene_uid) {
      scene = si;
      break;
    }
  }
  REQUIRE_MESSAGE(scene.has_value(), "cut scene_uid not found");

  std::optional<PhaseIndex> phase;
  for (const auto pi : iota_range<PhaseIndex>(0, sim.phase_count())) {
    if (sim.uid_of(pi) == sc.phase_uid) {
      phase = pi;
      break;
    }
  }
  REQUIRE_MESSAGE(phase.has_value(), "cut phase_uid not found");

  ArCutView view {
      .scene_pos = static_cast<std::size_t>(*scene),
      .phase_pos = static_cast<int>(static_cast<std::size_t>(*phase)),
      .rhs = sc.rhs,
  };

  const auto& svars = sim.state_variables(*scene, *phase);
  bool alpha_seen = false;
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
      } else if (key.class_name == "Reservoir") {
        view.energy_coeff = coeff;
      } else if (key.class_name == "Flow") {
        view.inflow_coeff = coeff;
        view.has_inflow_coeff = true;
      } else {
        REQUIRE_MESSAGE(false, "unexpected state-variable class in cut");
      }
      break;
    }
    REQUIRE_MESSAGE(matched, "cut coefficient not in the state registry");
  }
  REQUIRE_MESSAGE(alpha_seen, "optimality cut does not reference α");
  return view;
}

/// Cut bound at (e, q):  α ≥ rhs − c_e·e − c_q·q.
[[nodiscard]] double ar_cut_value_at(const ArCutView& view, double e, double q)
{
  return view.rhs - view.energy_coeff * e - view.inflow_coeff * q;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// Opt-out structure: absent inflow_model, no Flow state variable exists
// and the LP carries no lag column / AR rows.  With the model, exactly
// one lag column and blocks-per-stage AR rows appear per downstream
// phase — the "no new columns, no new rows" half of the byte-parity
// requirement, pinned within a single binary.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("AR inflow — opt-in structure and opt-out absence")  // NOLINT
{
  const auto count_flow_svars = [](const SimulationLP& sim)
  {
    int n = 0;
    for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
      for (const auto pi : iota_range<PhaseIndex>(0, sim.phase_count())) {
        for (const auto& [key, svar] : sim.state_variables(si, pi)) {
          if (key.class_name == "Flow") {
            ++n;
          }
        }
      }
    }
    return n;
  };

  auto run_and_measure = [&](bool with_model)
      -> std::tuple<int, std::vector<Index>, std::vector<Index>>
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
    if (with_model) {
      ar_attach_inflow_model(planning, kArPhi);
    }
    PlanningLP plp(std::move(planning));
    SDDPMethod sddp(plp, ar_sddp_opts(2));
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    std::vector<Index> ncols;
    std::vector<Index> nrows;
    for (const auto pi :
         iota_range<PhaseIndex>(0, plp.simulation().phase_count()))
    {
      const auto& li = plp.system(first_scene_index(), pi).linear_interface();
      ncols.push_back(li.get_numcols());
      nrows.push_back(li.get_numrows());
    }
    return {count_flow_svars(plp.simulation()), ncols, nrows};
  };

  const auto [n_off, cols_off, rows_off] = run_and_measure(false);
  const auto [n_on, cols_on, rows_on] = run_and_measure(true);

  // Opt-out: not a single Flow state variable, anywhere.
  CHECK(n_off == 0);
  // Opt-in: one "inflow" state per (scene, phase) = 2 scenes × 3 phases.
  CHECK(n_on == 6);

  REQUIRE(cols_off.size() == static_cast<std::size_t>(kArNumPhases));
  REQUIRE(cols_on.size() == cols_off.size());
  // Phase 0 has no lag: identical column/row counts.
  CHECK(cols_on[0] == cols_off[0]);
  CHECK(rows_on[0] == rows_off[0]);
  // Phases 1, 2: exactly one inflow_lag column and one AR row per block.
  for (std::size_t p = 1; p < cols_on.size(); ++p) {
    CHECK(cols_on[p] == cols_off[p] + 1);
    CHECK(rows_on[p] == rows_off[p] + kArBlocksPerStage);
  }
}

// ═════════════════════════════════════════════════════════════════════════
// phi = 0 parity: the AR structure with no memory converges to the same
// objective as the model-free run (task step-1 acceptance: "phi = 0
// fixture equals master on objective").
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("AR inflow — phi=0 equals the model-free objective")  // NOLINT
{
  const auto run = [](bool with_model)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
    ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
    if (with_model) {
      ar_attach_inflow_model(planning, 0.0);
    }
    PlanningLP plp(std::move(planning));
    SDDPMethod sddp(plp, ar_sddp_opts(25, 1.0e-8));
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return std::pair {results->back().lower_bound, results->back().upper_bound};
  };

  const auto [lb_off, ub_off] = run(false);
  const auto [lb_on, ub_on] = run(true);

  CHECK(lb_on == doctest::Approx(lb_off).epsilon(1.0e-8));
  CHECK(ub_on == doctest::Approx(ub_off).epsilon(1.0e-8));
}

// ═════════════════════════════════════════════════════════════════════════
// Exact oracle: with eps = 0 (v1 forward realization IS the schedule),
// the AR(1) run is equivalent to the explicit deterministic run with
// precomputed inflows — assert objective equality against the
// extensive-form optimum built from explicit tail plannings.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — AR(1) run equals the explicit deterministic optimum")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
  ar_attach_inflow_model(planning, kArPhi);
  PlanningLP plp(std::move(planning));

  SDDPMethod sddp(plp, ar_sddp_opts(25, 1.0e-8));
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  const auto& last = results->back();

  // Explicit extensive optimum: Σ_s p_s · V_s(full horizon).  Probing
  // at lag = mu makes the AR shift vanish — the deterministic run.
  const double v_wet = ar_tail_value(
      kArNumPhases, 0.6, kArWetInflow, kArPhi, kArWetInflow, kArEini);
  const double v_dry = ar_tail_value(
      kArNumPhases, 0.4, kArDryInflow, kArPhi, kArDryInflow, kArEini);
  const double extensive = v_wet + v_dry;

  CHECK(last.lower_bound == doctest::Approx(extensive).epsilon(1.0e-5));
  CHECK(last.upper_bound == doctest::Approx(extensive).epsilon(1.0e-5));
}

// ═════════════════════════════════════════════════════════════════════════
// 2-D cut validity: every optimality cut carries (α, energy, inflow)
// coefficients and must underestimate the explicit AR tail
// V_tail(e, q) over the (energy, lagged inflow) grid — Theorems O1/O2
// with the enlarged state; `cut_sharing = none` so the persistent
// per-scene tail is the certified truth at every transition.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — cuts underestimate the explicit AR tail over (e, q)")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
  ar_attach_inflow_model(planning, kArPhi);
  PlanningLP plp(std::move(planning));

  SDDPMethod sddp(plp, ar_sddp_opts(6));
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<double, 2> scene_prob = {0.6, 0.4};
  const std::array<double, 2> scene_mu = {kArWetInflow, kArDryInflow};
  // Grid trimmed 5×3 → 3×2 (endpoints kept): the audited property is
  // "affine cut ≤ convex AR tail", which a violation trips at any bracketing
  // grid point, so {0,100,200}×{−3,+3} preserves detection at ~40% the cost.
  const std::array<double, 3> e_grid = {0.0, 100.0, 200.0};
  const std::array<double, 2> q_off = {-3.0, 3.0};

  // Tail values cached per (tail length, scene, lag offset, e index).
  std::map<std::tuple<int, std::size_t, std::size_t, std::size_t>, double>
      tail_cache;

  const auto& sim = plp.simulation();
  const auto cuts = sddp.stored_cuts();
  int n_checked = 0;
  int n_with_inflow = 0;
  for (const auto& sc : cuts) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto view = ar_classify_cut(sim, sc);
    const int tail_stages = kArNumPhases - 1 - view.phase_pos;
    REQUIRE(tail_stages >= 1);
    if (view.has_inflow_coeff && view.inflow_coeff != 0.0) {
      ++n_with_inflow;
    }

    const double mu = scene_mu[view.scene_pos];
    for (std::size_t qi = 0; qi < q_off.size(); ++qi) {
      const double q = mu + q_off[qi];
      for (std::size_t ei = 0; ei < e_grid.size(); ++ei) {
        const auto cache_key = std::tuple {tail_stages, view.scene_pos, qi, ei};
        auto it = tail_cache.find(cache_key);
        if (it == tail_cache.end()) {
          it = tail_cache
                   .emplace(cache_key,
                            ar_tail_value(tail_stages,
                                          scene_prob[view.scene_pos],
                                          mu,
                                          kArPhi,
                                          q,
                                          e_grid[ei]))
                   .first;
        }
        const double v_tail = it->second;
        const double cut_val = ar_cut_value_at(view, e_grid[ei], q);
        INFO("scene=",
             view.scene_pos,
             " phase=",
             view.phase_pos,
             " e=",
             e_grid[ei],
             " q=",
             q,
             " cut=",
             cut_val,
             " V_tail=",
             v_tail);
        CHECK(cut_val <= v_tail + ar_tol(std::abs(v_tail)));
      }
    }
    ++n_checked;
  }
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);
  // The whole point of the feature: cuts price hydrological memory.
  CHECK(n_with_inflow >= 1);
}

// ═════════════════════════════════════════════════════════════════════════
// Aperture parity at phi = 0: synthetic (cross-scenario) apertures
// exercise the AR-row-RHS rewrite in FlowLP::update_aperture; with no
// memory the run must converge to the model-free aperture run's
// objective.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — synthetic apertures at phi=0 match the model-free run")
{
  const auto run = [](bool with_model)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
    if (with_model) {
      ar_attach_inflow_model(planning, 0.0);
    }
    PlanningLP plp(std::move(planning));
    SDDPMethod sddp(plp,
                    ar_sddp_opts(15, 1.0e-8, /*synthetic_apertures=*/true));
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return std::pair {results->back().lower_bound, results->back().upper_bound};
  };

  const auto [lb_off, ub_off] = run(false);
  const auto [lb_on, ub_on] = run(true);

  CHECK(lb_on == doctest::Approx(lb_off).epsilon(1.0e-7));
  CHECK(ub_on == doctest::Approx(ub_off).epsilon(1.0e-7));
}

// ═════════════════════════════════════════════════════════════════════════
// T2 (coverage-audit G2): AR × forward_sampling=resampled.  The
// resampled forward pass re-draws a realization at every phase boundary
// and applies it through the SAME `update_aperture` entry point the
// aperture pass uses — under `Flow.inflow_model` that write is the AR
// equality-row RHS, not a column-bound pin.  At phi = 0 the AR
// machinery must replicate the schedule exactly, so an AR+resampled run
// must converge to the SAME bounds as a model-free+resampled run over
// the identical heterogeneous wet/dry data.  A missed or mis-ordered
// AR-RHS rewrite (e.g. the lag pin read before
// `apply_sampled_realization`) biases the forward UB and breaks this
// parity.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — phi=0 + resampled forward sampling matches the "
    "model-free resampled run")
{
  const auto run = [](bool with_model)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
    if (with_model) {
      ar_attach_inflow_model(planning, 0.0);
    }
    PlanningLP plp(std::move(planning));
    auto opts = ar_sddp_opts(15, 1.0e-8);
    opts.cut_sharing = CutSharingMode::multicut;
    opts.forward_sampling = ForwardSamplingMode::resampled;
    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return std::pair {results->back().lower_bound, results->back().upper_bound};
  };

  const auto [lb_off, ub_off] = run(false);
  const auto [lb_on, ub_on] = run(true);

  CHECK(lb_on == doctest::Approx(lb_off).epsilon(1.0e-7));
  CHECK(ub_on == doctest::Approx(ub_off).epsilon(1.0e-7));
}

// ═════════════════════════════════════════════════════════════════════════
// AR × dual_shared guard (ledger F12): the dual-shared synthesis prices
// column-bound deltas only, but AR hydrology enters through the AR-row
// RHS — an unguarded synthesized intercept would drop the yᵀΔb term.
// `initialize_solver` must WARN + downgrade dual_shared/screened to
// warm; the run must then match the warm run cut-for-cut (per-aperture
// exact solves).  phi = 0.6 with heterogeneous wet/dry inflows makes
// the AR RHS delta across apertures non-zero, so pre-guard the
// dual-shared cut RHS sequence genuinely diverges from warm.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — dual_shared/screened downgrade to warm (F12 guard parity)")
{
  const auto run = [](ApertureSolveMode mode)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
    ar_attach_inflow_model(planning, kArPhi);
    PlanningLP plp(std::move(planning));
    auto opts = ar_sddp_opts(8, 1.0e-8, /*synthetic_apertures=*/true);
    opts.aperture_solve_mode = mode;
    SDDPMethod sddp(plp, opts);
    // Capture the setup WARN around initialization only — the solve's
    // trace/info traffic would evict it from the ring buffer.
    bool warned = false;
    {
      gtopt::test::LogCapture logs;
      REQUIRE(sddp.ensure_initialized().has_value());
      warned = logs.contains("downgrading to aperture_solve_mode=warm");
    }
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return std::tuple {warned,
                       results->back().lower_bound,
                       results->back().upper_bound,
                       sddp.stored_cuts()};
  };

  const auto [warned_ds, lb_ds, ub_ds, cuts_ds] =
      run(ApertureSolveMode::dual_shared);
  CHECK(warned_ds);

  const auto [warned_sc, lb_sc, ub_sc, cuts_sc] =
      run(ApertureSolveMode::screened);
  CHECK(warned_sc);

  const auto [warned_warm, lb_warm, ub_warm, cuts_warm] =
      run(ApertureSolveMode::warm);
  CHECK_FALSE(warned_warm);
  // Non-vacuity: the parity below must compare actual cuts (the
  // RHS-blindness of the synthesis formula itself is pinned at the
  // unit level — "F12 pre-gate delta witness" in
  // test_sddp_aperture_functions.cpp).
  CHECK_FALSE(cuts_warm.empty());

  // Post-downgrade both runs execute the warm path — bounds and the
  // full stored-cut sequence (rhs + every coefficient) must match.
  CHECK(lb_ds == doctest::Approx(lb_warm).epsilon(1.0e-12));
  CHECK(ub_ds == doctest::Approx(ub_warm).epsilon(1.0e-12));
  CHECK(lb_sc == doctest::Approx(lb_warm).epsilon(1.0e-12));
  CHECK(ub_sc == doctest::Approx(ub_warm).epsilon(1.0e-12));

  const auto check_cut_parity =
      [](const auto& lhs, const auto& rhs, std::string_view tag)
  {
    INFO("cut-parity ", tag);
    REQUIRE(lhs.size() == rhs.size());
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      const auto& a = lhs[i];
      const auto& b = rhs[i];
      CHECK(a.type == b.type);
      CHECK(a.scene_uid == b.scene_uid);
      CHECK(a.phase_uid == b.phase_uid);
      CHECK(a.rhs == doctest::Approx(b.rhs).epsilon(1.0e-12));
      REQUIRE(a.coefficients.size() == b.coefficients.size());
      for (std::size_t j = 0; j < a.coefficients.size(); ++j) {
        CHECK(a.coefficients[j].first == b.coefficients[j].first);
        CHECK(a.coefficients[j].second
              == doctest::Approx(b.coefficients[j].second).epsilon(1.0e-12));
      }
    }
  };
  check_cut_parity(cuts_ds, cuts_warm, "dual_shared vs warm");
  check_cut_parity(cuts_sc, cuts_warm, "screened vs warm");
}

// ═════════════════════════════════════════════════════════════════════════
// JSON: inflow_model round-trips and defaults.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("AR inflow — inflow_model JSON round-trip")  // NOLINT
{
  constexpr std::string_view json = R"({
    "uid": 7,
    "name": "inflow_j1",
    "junction": 1,
    "discharge": 8.0,
    "inflow_model": { "type": "ar1", "phi": 0.62, "sigma": 14.3 }
  })";

  const auto flow = daw::json::from_json<Flow>(json);
  REQUIRE(flow.inflow_model.has_value());
  CHECK(flow.inflow_model->type.value_or("") == "ar1");
  CHECK(flow.inflow_model->phi.value_or(-1.0) == doctest::Approx(0.62));
  CHECK(flow.inflow_model->sigma.value_or(-1.0) == doctest::Approx(14.3));

  // Absent → nullopt (opt-out).
  constexpr std::string_view json_plain = R"({
    "uid": 8,
    "name": "plain",
    "junction": 1,
    "discharge": 8.0
  })";
  const auto plain = daw::json::from_json<Flow>(json_plain);
  CHECK_FALSE(plain.inflow_model.has_value());

  // Round-trip preserves the model.
  const auto round = daw::json::from_json<Flow>(daw::json::to_json(flow));
  REQUIRE(round.inflow_model.has_value());
  CHECK(round.inflow_model->phi.value_or(-1.0) == doctest::Approx(0.62));
}

// ═════════════════════════════════════════════════════════════════════════
// T4 (coverage-audit G4): AR cuts through the Parquet codec.  An AR run
// stores cuts whose coefficients include the Flow-class lagged-inflow
// coordinate; a narrowing, off-by-one, or dropped column in the
// serialization would silently lose the inflow gradient.  Save the
// trained cut set, reload it into a FRESH identical planning via the
// canonical hot-start path (`SDDPMethod::load_cuts`), and assert every
// cut classifies identically — rhs, energy coefficient, and the inflow
// coefficient — through each run's own state-variable registry.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "AR inflow — cut Parquet round-trip preserves the inflow coefficient")
{
  const auto cuts_file = (std::filesystem::temp_directory_path()
                          / "gtopt_test_ar_cut_roundtrip.parquet")
                             .string();

  // Train and save.
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  ar_set_scenario_inflows(planning, kArWetInflow, kArDryInflow);
  ar_attach_inflow_model(planning, kArPhi);
  PlanningLP plp(std::move(planning));
  SDDPMethod sddp(plp, ar_sddp_opts(3, 1.0e-9));
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  const auto orig = sddp.stored_cuts();
  REQUIRE_FALSE(orig.empty());
  REQUIRE(save_cuts_parquet(orig, plp, cuts_file).has_value());

  // Classify the originals; at least one optimality cut must carry the
  // inflow (Flow-class) coefficient, or the round-trip check is vacuous.
  //
  // Cuts are matched on the (type, scene, phase, iteration) 4-tuple —
  // NOT the full `CutKey` — because the loader deliberately re-encodes
  // the `extra` discriminator as the origin-encoded broadcast context
  // (`ctx_extra = extra·N + origin`, `sddp_cut_parquet.cpp`); the
  // 4-tuple is unique per optimality cut on this fixture.
  using RoundTripKey = std::tuple<CutType, SceneUid, PhaseUid, Uid>;
  const auto rt_key = [](const StoredCut& sc) -> RoundTripKey
  {
    return {sc.type,
            sc.scene_uid,
            sc.phase_uid,
            static_cast<Uid>(gtopt::uid_of(sc.iteration_index))};
  };
  std::map<RoundTripKey, ArCutView> orig_views;
  for (const auto& sc : orig) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto [it_ins, inserted] =
        orig_views.emplace(rt_key(sc), ar_classify_cut(plp.simulation(), sc));
    REQUIRE_MESSAGE(inserted, "4-tuple key not unique in the saved set");
  }
  const bool any_inflow = std::ranges::any_of(
      orig_views, [](const auto& kv) { return kv.second.has_inflow_coeff; });
  REQUIRE(any_inflow);

  // Reload into a fresh identical planning (canonical hot-start path —
  // `load_cuts` resolves the structured coefficient keys against the
  // NEW run's state-variable registry and stores the loaded cuts).
  auto planning2 = make_2scene_3phase_hydro_planning(0.6, 0.4);
  ar_set_scenario_inflows(planning2, kArWetInflow, kArDryInflow);
  ar_attach_inflow_model(planning2, kArPhi);
  PlanningLP plp2(std::move(planning2));
  SDDPMethod sddp2(plp2, ar_sddp_opts(1, 1.0e-9));
  REQUIRE(sddp2.ensure_initialized().has_value());
  const auto loaded = sddp2.load_cuts(cuts_file);
  REQUIRE(loaded.has_value());

  const auto rt = sddp2.stored_cuts();
  std::size_t matched = 0;
  for (const auto& sc : rt) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto it = orig_views.find(rt_key(sc));
    REQUIRE_MESSAGE(it != orig_views.end(),
                    "reloaded cut key not in save: type=",
                    static_cast<int>(sc.type),
                    " scene=",
                    static_cast<int>(sc.scene_uid),
                    " phase=",
                    static_cast<int>(sc.phase_uid),
                    " iter=",
                    static_cast<int>(gtopt::uid_of(sc.iteration_index)));
    const auto view = ar_classify_cut(plp2.simulation(), sc);
    const auto& ov = it->second;
    INFO("cut scene=", ov.scene_pos, " phase=", ov.phase_pos);
    // Parquet stores raw float64 — the round-trip must be bit-tight.
    CHECK(view.rhs == doctest::Approx(ov.rhs).epsilon(1.0e-15));
    CHECK(view.energy_coeff
          == doctest::Approx(ov.energy_coeff).epsilon(1.0e-15));
    CHECK(view.has_inflow_coeff == ov.has_inflow_coeff);
    if (ov.has_inflow_coeff) {
      CHECK(view.inflow_coeff
            == doctest::Approx(ov.inflow_coeff).epsilon(1.0e-15));
    }
    ++matched;
  }
  CHECK(matched == orig_views.size());

  std::filesystem::remove(cuts_file);
}
