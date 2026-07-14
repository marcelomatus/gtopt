// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_cut_oracle.cpp
 * @brief     Extensive-form oracle property harness for SDDP cut validity
 *            (deliverable 2 of the cut-validity certification campaign).
 * @date      2026-07-08
 *
 * Asserts that every optimality cut emitted by the SDDP backward pass
 * UNDERESTIMATES the true recourse function, using gtopt itself as the
 * extensive-form solver:
 *
 *   * **Tail-LP oracle** — for a cut generated for (scene s, target
 *     phase t+1) and installed on phase t's α, the persistent-scene
 *     truth is `V_s^tail(x)` = optimal value of the SAME planning
 *     restricted to phases t+1..T for scene s, with the reservoir
 *     initial energy set to x, solved monolithically as ONE LP (a
 *     single phase carrying all remaining stages).  The tail keeps the
 *     scenario's `probability_factor`, so the oracle value carries the
 *     same `cost_factor = p_s × discount × duration` folding the SDDP
 *     scene-LP objective does (`docs/formulation/sddp-cut-validity.md`
 *     §1) — verified by the unit-consistency self-check below.
 *
 *   * **Cut evaluation** — a `StoredCut` holds the PHYSICAL row
 *     `α + Σ_j coeff_j·x_j ≥ rhs` (α coefficient 1.0), so the cut's
 *     bound at state x is `rhs − Σ_{j≠α} coeff_j·x_j`.  α columns are
 *     identified through the state-variable registry (class "Sddp",
 *     covering every `varphi_s` under multicut); the remaining
 *     coefficient must be the reservoir's outgoing-state column.
 *
 * Mode coverage (verdicts per `docs/formulation/sddp-cut-validity.md`):
 *   * `none`      — strict at every transition (Theorems O1/O2/N1).
 *   * `multicut`, identical scenes — resampled ≡ persistent process,
 *     strict at every transition (Theorem M1 degenerate case).
 *   * `multicut`, heterogeneous scenes, uniform probabilities — strict
 *     at the FINAL transition only (the last phase has no future term,
 *     so its cut is a per-scene-exact support regardless of sharing).
 *     Earlier transitions bound the RESAMPLED-tree value (Theorem M1),
 *     which is not the persistent tail; certifying them would require
 *     backward support reconstruction of the mean-future PWL function,
 *     which is deliberately NOT shipped here (too intrusive to wire
 *     into a Planning without a flaky approximation).  The strict
 *     earlier-phase coverage lives in the identical-scene case.
 *   * `multicut`, non-uniform probabilities — since the M4 pricing fix
 *     (`alpha_unit_cost`: w_r = p_s, Prop. M4 in the theorem doc §8)
 *     the recursion is the Bellman recursion of the process resampled
 *     with measure q_r = p_r for ANY probability vector.  On IDENTICAL
 *     dynamics resampled ≡ persistent, so the tail oracle is exact and
 *     STRICT at every transition (the failing-then-passing gate for
 *     M4: the pre-M4 1/N pricing inflated the low-probability scene's
 *     future term by 1/(N·p_s) and its non-terminal cuts overshot).
 *     On heterogeneous dynamics the persistent-tail comparison remains
 *     WARN-only (process mismatch, Corollary M2 — same status as the
 *     uniform heterogeneous case).
 *
 * The pure-Benders sections disable the aperture backward pass
 * (`apertures = {}`) so the audited cuts are plain Benders cuts.  The
 * APERTURE sections at the bottom re-enable it with synthetic
 * apertures (one per scenario, q_a = 1/N) and audit the expected
 * aperture cuts against the q-mixture tail oracle (Theorem AP1 /
 * Lemma AP2, §6) across the aperture solve modes — including the
 * dual-shared (Infanger–Morton) synthesis and its screened variant.
 */

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Geometry of the audited fixture ────────────────────────────────────────
//
// All cases below run on `make_2scene_3phase_hydro_planning` (see
// sddp_helpers.hpp): 3 phases × 1 stage × 4 blocks of 1 h, one bus,
// hydro 50 MW @ $5, thermal 200 MW @ $50, demand 80 MW, one reservoir
// with box [0, 200] dam³, eini = 100, inflow 8 dam³/h (overridable per
// scenario below).  `scale_objective = 1` so `get_obj_value()` is the
// physical objective directly.
constexpr int kOracleNumPhases = 3;
constexpr int kOracleBlocksPerStage = 4;
constexpr double kOracleEmax = 200.0;  // reservoir state box is [0, emax]
constexpr double kOracleEini = 100.0;
constexpr double kOracleBaseInflow = 8.0;
constexpr double kOracleWetInflow = 11.0;
constexpr double kOracleDryInflow = 5.0;

// ─── ε-validity tolerance (theorem doc §4) ──────────────────────────────────
//
// Per `docs/formulation/sddp-cut-validity.md` §4, an emitted cut is only
// ε-valid: the build-time filter (Theorem O3) may over-tighten by up to
// `cut_coeff_eps × Σ_dropped diam(box_i)`, and the LP solves feeding
// both the cut (z*, reduced costs) and the oracle value carry solver
// feasibility noise proportional to the objective magnitude.  The
// harness runs with `cut_coeff_eps = 0` (the SDDPOptions default), so
// the first term is a conservative allowance and the solver term
// dominates:  tol = 1e-8 × box_diam + 1e-6 × max(1, |V|).
constexpr double kOracleCutCoeffEps = 1.0e-8;
constexpr double kOracleSolverFeasTol = 1.0e-6;

[[nodiscard]] double oracle_tol(double box_diam, double v_abs)
{
  return kOracleCutCoeffEps * box_diam
      + kOracleSolverFeasTol * std::max(1.0, v_abs);
}

// ─── State grid ─────────────────────────────────────────────────────────────

constexpr std::size_t kOracleGridN = 5;  // 0, 50, 100, 150, 200

[[nodiscard]] std::array<double, kOracleGridN> oracle_state_grid()
{
  std::array<double, kOracleGridN> grid {};
  for (std::size_t i = 0; i < kOracleGridN; ++i) {
    grid[i] = kOracleEmax * static_cast<double>(i)
        / static_cast<double>(kOracleGridN - 1);
  }
  return grid;
}

// ─── Fixture plumbing ───────────────────────────────────────────────────────

/// Per-scene oracle data: the scenario's probability factor (cost
/// folding) and its inflow (persistent sample path).
struct OracleSceneData
{
  double prob {};
  double inflow {};
};

/// Give each scenario of the 2-scene fixture its own constant inflow
/// (scenario × stage × block schedule), making the scenes genuinely
/// heterogeneous sample paths.
void oracle_set_scenario_inflows(Planning& planning, double q0, double q1)
{
  std::vector<std::vector<std::vector<double>>> sched;
  sched.reserve(2);
  for (const double q : {q0, q1}) {
    std::vector<std::vector<double>> per_stage;
    per_stage.reserve(static_cast<std::size_t>(kOracleNumPhases));
    for (int st = 0; st < kOracleNumPhases; ++st) {
      per_stage.push_back(std::vector<double>(
          static_cast<std::size_t>(kOracleBlocksPerStage), q));
    }
    sched.push_back(std::move(per_stage));
  }
  planning.system.flow_array[0].discharge = STBRealFieldSched {sched};
}

/// Single-scenario tail Planning: the last @p num_stages stages of the
/// fixture horizon merged into ONE phase (so the monolithic solve is a
/// single LP over the whole tail — the extensive form), with reservoir
/// `eini` = the probed state x.  The tail is built UNFOLDED: a lone
/// scenario's `probability_factor` is normalized away at LP build time
/// (probabilities are rescaled to sum to 1 over the planning's own
/// scenarios), so the p_s cost folding of the SDDP scene-LP objective
/// is applied EXTERNALLY in `oracle_tail_value` — a convention pinned
/// by the unit-consistency self-check below.
[[nodiscard]] Planning oracle_make_tail_planning(int num_stages,
                                                 double inflow,
                                                 double eini)
{
  auto block_array = make_uniform_blocks(
      static_cast<std::size_t>(num_stages * kOracleBlocksPerStage), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages),
                          static_cast<std::size_t>(kOracleBlocksPerStage));

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

  // System mirrors make_2scene_3phase_hydro_planning exactly, except
  // for the parameterised eini / inflow.
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
          .capacity = kOracleEmax,
          .emin = 0.0,
          .emax = kOracleEmax,
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
              .name = "sddp_cut_oracle_tail",
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

/// Solve the tail extensive form and return its physical objective,
/// folded by @p prob EXTERNALLY (see `oracle_make_tail_planning` for
/// why the tail LP itself is unfolded).  This reproduces the SDDP
/// scene-LP folding `cost_factor = p_s × discount × duration` — the
/// SDDP fixture's scenario probabilities already sum to 1, so no
/// build-time rescaling applies there and p_s survives verbatim.
[[nodiscard]] double oracle_tail_value(int num_stages,
                                       double prob,
                                       double inflow,
                                       double eini)
{
  auto planning = oracle_make_tail_planning(num_stages, inflow, eini);
  PlanningLP plp(std::move(planning));
  auto status = plp.resolve();
  REQUIRE(status.has_value());
  REQUIRE(*status == 1);  // optimal
  return prob
      * plp.system(first_scene_index(), PhaseIndex {0})
            .linear_interface()
            .get_obj_value();
}

/// Extensive optimum of the PERSISTENT-scene process: Σ_s (p_s-folded
/// full-horizon tail value at the fixture's original eini).
[[nodiscard]] double oracle_persistent_extensive_optimum(
    const std::array<OracleSceneData, 2>& scenes)
{
  double total = 0.0;
  for (const auto& sd : scenes) {
    total +=
        oracle_tail_value(kOracleNumPhases, sd.prob, sd.inflow, kOracleEini);
  }
  return total;
}

// ─── SDDP driver ────────────────────────────────────────────────────────────

[[nodiscard]] SDDPOptions oracle_sddp_opts(CutSharingMode mode,
                                           int max_iterations,
                                           double convergence_tol = 1.0e-9)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = convergence_tol;
  opts.stationary_tol = 0.0;  // no early stationary exit — audit more cuts
  opts.cut_sharing = mode;
  opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  opts.enable_api = false;
  return opts;
}

// ─── Cut classification ─────────────────────────────────────────────────────

/// A stored optimality cut reduced to oracle coordinates: which scene
/// generated it, which phase's α it bounds (0-based position of the
/// phase it is INSTALLED on), and the physical row `α + coeff·x ≥ rhs`.
struct OracleCutView
{
  std::size_t scene_pos {};
  int phase_pos {};
  double rhs {};
  double state_coeff {};
};

[[nodiscard]] OracleCutView oracle_classify_cut(const SimulationLP& sim,
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

  // Resolve every coefficient column against the (scene, phase)
  // state-variable registry: α columns carry class "Sddp" (this covers
  // every varphi_s under multicut); the fixture's only physical state
  // is the reservoir energy.
  OracleCutView view {
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
        // Cut row convention (§1): the α coefficient is exactly 1.
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
[[nodiscard]] double oracle_cut_value_at(const OracleCutView& view, double x)
{
  return view.rhs - view.state_coeff * x;
}

// ─── The oracle audit ───────────────────────────────────────────────────────

/// Sweep every stored optimality cut across the reservoir state box and
/// compare against the persistent-scene tail oracle.
///
/// @param only_tail_stages  0 = audit every transition; k > 0 = audit
///        only cuts whose target tail is exactly k stages long (k = 1
///        selects the FINAL transition — cuts bounding the last phase).
/// @param strict  CHECK when the theory certifies the configuration,
///        WARN for the known-unsound demonstrations.
/// @returns Number of cuts audited.
int oracle_audit_cuts(const SimulationLP& sim,
                      const std::vector<StoredCut>& cuts,
                      const std::array<OracleSceneData, 2>& scenes,
                      bool strict,
                      int only_tail_stages = 0)
{
  const auto grid = oracle_state_grid();

  // Tail values cached per (tail length, scene): the oracle re-solves
  // the same extensive form for every cut on the same transition.
  std::map<std::pair<int, std::size_t>, std::array<double, kOracleGridN>>
      tail_cache;

  int n_checked = 0;
  for (const auto& sc : cuts) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto view = oracle_classify_cut(sim, sc);

    // A cut installed on phase t bounds the tail over phases t+1..T;
    // each fixture phase carries one stage.
    const int tail_stages = kOracleNumPhases - 1 - view.phase_pos;
    REQUIRE(tail_stages >= 1);
    if (only_tail_stages != 0 && tail_stages != only_tail_stages) {
      continue;
    }

    const auto cache_key = std::pair {tail_stages, view.scene_pos};
    auto it = tail_cache.find(cache_key);
    if (it == tail_cache.end()) {
      std::array<double, kOracleGridN> vals {};
      for (std::size_t i = 0; i < kOracleGridN; ++i) {
        vals[i] = oracle_tail_value(tail_stages,
                                    scenes[view.scene_pos].prob,
                                    scenes[view.scene_pos].inflow,
                                    grid[i]);
      }
      it = tail_cache.emplace(cache_key, vals).first;
    }

    double worst_overshoot = -std::numeric_limits<double>::infinity();
    double worst_x = grid[0];
    for (std::size_t i = 0; i < kOracleGridN; ++i) {
      const double v_tail = it->second[i];
      const double cut_val = oracle_cut_value_at(view, grid[i]);
      const double tol = oracle_tol(kOracleEmax, std::abs(v_tail));
      if (strict) {
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
             " V_tail=",
             v_tail);
        CHECK(cut_val <= v_tail + tol);
      } else if (cut_val - (v_tail + tol) > worst_overshoot) {
        worst_overshoot = cut_val - (v_tail + tol);
        worst_x = grid[i];
      }
    }
    if (!strict) {
      // One WARN per cut at its worst grid point — keeps the
      // demonstrative M3 output compact.
      INFO("scene=",
           view.scene_pos,
           " phase=",
           view.phase_pos,
           " iter=",
           static_cast<int>(sc.iteration_index),
           " worst_x=",
           worst_x,
           " overshoot=",
           worst_overshoot);
      WARN(worst_overshoot <= 0.0);
    }
    ++n_checked;
  }
  return n_checked;
}

/// LB property: the SDDP master lower bound never exceeds the
/// extensive-form optimum of the process the cuts certify.
void oracle_audit_lower_bounds(const std::vector<SDDPIterationResult>& results,
                               double extensive_opt,
                               bool strict)
{
  // Two per-scene master solves feed the LB sum → double the solver
  // allowance.
  const double tol = 2.0 * oracle_tol(kOracleEmax, std::abs(extensive_opt));
  for (const auto& ir : results) {
    INFO("iter=",
         static_cast<int>(ir.iteration_index),
         " LB=",
         ir.lower_bound,
         " extensive_opt=",
         extensive_opt);
    if (strict) {
      CHECK(ir.lower_bound <= extensive_opt + tol);
    } else {
      WARN(ir.lower_bound <= extensive_opt + tol);
    }
  }
}

// ─── Aperture (mixture) oracle ──────────────────────────────────────────────

/// SDDP options with the SYNTHETIC aperture backward pass enabled: one
/// aperture per requested scenario UID, each with probability 1/N (see
/// `build_synthetic_apertures`), so the expected aperture cut prices the
/// uniform q-mixture of the aperture inflows.
[[nodiscard]] SDDPOptions oracle_aperture_sddp_opts(
    ApertureSolveMode solve_mode,
    std::vector<Uid> aperture_uids,
    int max_iterations,
    int screen_count = 2)
{
  auto opts = oracle_sddp_opts(CutSharingMode::none, max_iterations);
  opts.apertures = std::move(aperture_uids);
  opts.aperture_solve_mode = solve_mode;
  opts.aperture_screen_count = screen_count;
  return opts;
}

/// Audit aperture expected cuts against the q-mixture tail oracle
/// (Theorem AP1): with synthetic apertures (q_a = 1/N over the two
/// scenarios) the ecut for (scene s, tail k) must underestimate
///
///   V_mix(s, k, x) = p_s × (1/2) Σ_a tail(k, inflow_a, x)
///
/// — the scene-LP objective keeps the scene's own p_s folding while
/// the aperture bound rewrite mixes the inflows under q.  Certifiable
/// at the FINAL transition unconditionally (no future term); at every
/// transition when the mixture degenerates (identical inflows).
int oracle_audit_aperture_cuts(const SimulationLP& sim,
                               const std::vector<StoredCut>& cuts,
                               const std::array<double, 2>& scene_probs,
                               const std::array<double, 2>& aperture_inflows,
                               bool strict,
                               int only_tail_stages = 0)
{
  const auto grid = oracle_state_grid();

  // (tail length, aperture) → UNFOLDED tail values on the state grid.
  std::map<std::pair<int, std::size_t>, std::array<double, kOracleGridN>>
      tail_cache;
  const auto tail_vals =
      [&](int k, std::size_t a) -> const std::array<double, kOracleGridN>&
  {
    const auto key = std::pair {k, a};
    auto it = tail_cache.find(key);
    if (it == tail_cache.end()) {
      std::array<double, kOracleGridN> vals {};
      for (std::size_t i = 0; i < kOracleGridN; ++i) {
        vals[i] = oracle_tail_value(k, 1.0, aperture_inflows[a], grid[i]);
      }
      it = tail_cache.emplace(key, vals).first;
    }
    return it->second;
  };

  int n_checked = 0;
  for (const auto& sc : cuts) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto view = oracle_classify_cut(sim, sc);
    const int tail_stages = kOracleNumPhases - 1 - view.phase_pos;
    REQUIRE(tail_stages >= 1);
    if (only_tail_stages != 0 && tail_stages != only_tail_stages) {
      continue;
    }

    const auto& v0 = tail_vals(tail_stages, 0);
    const auto& v1 = tail_vals(tail_stages, 1);
    const double p_s = scene_probs[view.scene_pos];
    for (std::size_t i = 0; i < kOracleGridN; ++i) {
      const double v_mix = p_s * 0.5 * (v0[i] + v1[i]);
      const double cut_val = oracle_cut_value_at(view, grid[i]);
      const double tol = oracle_tol(kOracleEmax, std::abs(v_mix));
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
           " V_mix=",
           v_mix);
      if (strict) {
        CHECK(cut_val <= v_mix + tol);
      } else {
        WARN(cut_val <= v_mix + tol);
      }
    }
    ++n_checked;
  }
  return n_checked;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// Unit-consistency self-check: the tail oracle's folding conventions
// match the SDDP master objective.  On the identical-scene fixture at
// convergence, LB = UB = Σ_s V_s(0) = the full-horizon extensive
// optimum, and each per-scene master objective is the p_s-folded tail.
// This validates the oracle's folding convention (unfolded tail LP ×
// external p_s — see `oracle_tail_value`) against the SDDP scene-LP
// `cost_factor` folding before any cut is audited.  Historical note:
// the first version of this check exposed that a lone scenario's
// `probability_factor` is rescaled away at LP build time, which is why
// the folding is external.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — self-check: full-horizon tail matches the SDDP "
    "master objective at convergence")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::none,
                               /*max_iterations=*/25,
                               /*convergence_tol=*/1.0e-7);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  const auto& last = results->back();
  REQUIRE(last.gap <= 1.0e-6);

  // Full-horizon extensive optimum, unfolded (prob = 1):  both scenes
  // are identical with p = 0.5 each, so Σ_s V_s = V_full.
  const double v_full =
      oracle_tail_value(kOracleNumPhases, 1.0, kOracleBaseInflow, kOracleEini);
  REQUIRE(v_full > 0.0);

  CHECK(last.lower_bound == doctest::Approx(v_full).epsilon(1.0e-4));
  CHECK(last.upper_bound == doctest::Approx(v_full).epsilon(1.0e-4));

  // Per-scene folding: each scene's master objective is the 0.5-folded
  // full-horizon tail — the direct pin of the SDDP `cost_factor`
  // folding against the oracle's external p_s multiplication.
  if (!last.scene_lower_bounds.empty()) {
    REQUIRE(last.scene_lower_bounds.size() == 2);
    CHECK(last.scene_lower_bounds[0]
          == doctest::Approx(0.5 * v_full).epsilon(1.0e-4));
    CHECK(last.scene_lower_bounds[1]
          == doctest::Approx(0.5 * v_full).epsilon(1.0e-4));
  }
}

// ═════════════════════════════════════════════════════════════════════════
// Mode `none`: unconditionally valid (Theorems O1/O2/N1).  Every stored
// optimality cut must underestimate its own scene's persistent tail at
// every state in the box, and the LB must never exceed the persistent
// extensive optimum.  Heterogeneous inflows AND probabilities.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — cut_sharing=none: every optimality cut "
    "underestimates the persistent per-scene tail")
{
  const std::array<std::pair<double, double>, 2> prob_splits = {{
      {0.6, 0.4},
      {0.3, 0.7},
  }};

  for (const auto& [p0, p1] : prob_splits) {
    const auto label = std::format("none {}/{}", p0, p1);
    SUBCASE(label.c_str())
    {
      auto planning = make_2scene_3phase_hydro_planning(p0, p1);
      oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
      PlanningLP plp(std::move(planning));

      auto opts = oracle_sddp_opts(CutSharingMode::none,
                                   /*max_iterations=*/6);
      SDDPMethod sddp(plp, opts);
      auto results = sddp.solve();
      REQUIRE(results.has_value());
      REQUIRE_FALSE(results->empty());

      const std::array<OracleSceneData, 2> scenes = {{
          {.prob = p0, .inflow = kOracleWetInflow},
          {.prob = p1, .inflow = kOracleDryInflow},
      }};

      const auto cuts = sddp.stored_cuts();
      const int n_checked =
          oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
      CAPTURE(n_checked);
      REQUIRE(n_checked >= 2);

      // LB ≤ persistent extensive optimum at EVERY iteration
      // (Theorem N1).
      oracle_audit_lower_bounds(*results,
                                oracle_persistent_extensive_optimum(scenes),
                                /*strict=*/true);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
// Mode `multicut`, identical scenes: the resampled process coincides
// with the persistent one (every scene realizes the same path), so the
// persistent tail oracle applies at EVERY phase — strict (Theorem M1,
// degenerate case).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — multicut on identical scenes: cuts underestimate "
    "the common tail at every transition")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.5, .inflow = kOracleBaseInflow},
      {.prob = 0.5, .inflow = kOracleBaseInflow},
  }};

  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  // Identical scenes: resampled optimum == persistent optimum, so the
  // LB property is strict here too.
  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/true);
}

// ═════════════════════════════════════════════════════════════════════════
// Mode `multicut`, heterogeneous scenes, UNIFORM probabilities: cuts at
// the FINAL transition bound the last phase, which has no future term —
// they are per-scene-exact supports of the single-phase tail regardless
// of the sharing mode, so the persistent oracle applies strictly there.
//
// Earlier transitions bound the RESAMPLED-tree value (Theorem M1),
// which is a different function from the persistent tail; certifying
// them exactly would require backward support reconstruction of the
// mean-future PWL function wired into a Planning.  That is deliberately
// NOT shipped (see the file header) — the strict earlier-phase coverage
// for multicut lives in the identical-scene case above, and the
// heterogeneous case is certified at the terminal transition only.
// No LB assertion either: the resampled-process optimum and the
// persistent extensive optimum are not ordered (Corollary M2).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — multicut heterogeneous uniform (0.5/0.5): "
    "final-transition cuts are per-scene exact underestimators")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.5, .inflow = kOracleWetInflow},
      {.prob = 0.5, .inflow = kOracleDryInflow},
  }};

  const auto cuts = sddp.stored_cuts();
  const int n_checked = oracle_audit_cuts(plp.simulation(),
                                          cuts,
                                          scenes,
                                          /*strict=*/true,
                                          /*only_tail_stages=*/1);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);
}

// ═════════════════════════════════════════════════════════════════════════
// Mode `multicut`, IDENTICAL dynamics, NON-uniform probabilities
// (0.6/0.4): the M4 pricing (`alpha_unit_cost`: every varphi_r in
// scene-s's LP priced at w_r = p_s, Prop. M4 in
// `docs/formulation/sddp-cut-validity.md` §8) makes the scene-LP
// recursion the Bellman recursion of the process resampled with
// measure q_r = p_r.  With identical dynamics the resampled process
// coincides with the persistent one, so the per-scene persistent tail
// oracle is exact and STRICT at every transition — including the
// non-terminal ones.
//
// This is the mandated failing-then-passing gate for the M4 fix: with
// the pre-M4 uniform 1/N pricing (theorem M3), the low-probability
// scene's phase-1 LP priced its future term at 1/2 instead of p_s =
// 0.4, so its phase-0 cuts carried z* with an INFLATED future term and
// overshot the 0.4-folded persistent tail at EVERY grid point (54/494
// assertions failed; overshoot from ≈ $412 at x = 200, 5.1% of
// V_tail = 8048, to ≈ $1312 at x = 0, 11.3% of V_tail = 11648 — see
// the commit message).  With M4 pricing the same sweep passes
// strictly.
// ═════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════
// Mode `markov` (opt-in, experimental — docs/formulation/sddp-markov.md).
//
// (a) M = N degenerate ≡ multicut: singleton states with transition
//     rows equal to the (normalized) scene probabilities give
//     w_{s,m'} = p_s (§4.1 of sddp-markov.md) — under UNIFORM
//     probabilities this equals the multicut 1/N pricing at this
//     commit (and the post-M4 `w_r = p_s` pricing thereafter), and the
//     cut routing (`varphi_{m(S)} = varphi_S`) is identical.  The two
//     modes must therefore produce the same LB trajectory and the same
//     cuts on the same fixture.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — markov M=N degenerate reproduces multicut "
    "(identical LB trajectory and cuts)")
{
  // Cuts must be classified INSIDE each run, against the run's own
  // live simulation: `oracle_classify_cut` resolves coefficient
  // columns through the state-variable registry, and the α (`varphi`)
  // columns exist only in the PlanningLP whose SDDPMethod registered
  // them — a fresh fixture rebuild has no α registration, so the α
  // coefficient column would match nothing.  The classified
  // `OracleCutView` is column-agnostic (physical rhs + reservoir
  // coefficient), so the cross-mode comparison below is unaffected.
  using CutId = std::tuple<SceneUid, PhaseUid, int>;

  struct ModeRun
  {
    std::vector<SDDPIterationResult> results;
    std::map<CutId, OracleCutView> cut_views;
    std::size_t optimality_cuts {};
  };

  auto run_mode = [](CutSharingMode mode,
                     const MarkovChainConfig* markov) -> ModeRun
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
    PlanningLP plp(std::move(planning));

    auto opts = oracle_sddp_opts(mode, /*max_iterations=*/6);
    if (markov != nullptr) {
      opts.markov = *markov;
    }
    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    ModeRun run;
    run.results = *results;
    for (const auto& sc : sddp.stored_cuts()) {
      if (sc.type != CutType::Optimality) {
        continue;
      }
      ++run.optimality_cuts;
      const auto id = CutId {
          sc.scene_uid,
          sc.phase_uid,
          static_cast<int>(sc.iteration_index),
      };
      const auto view = oracle_classify_cut(plp.simulation(), sc);
      REQUIRE_MESSAGE(run.cut_views.emplace(id, view).second,
                      "duplicate optimality cut id in one run");
    }
    return run;
  };

  // Singleton states, transition rows = the (uniform) scene
  // probabilities — the degenerate configuration of §4.1.
  const auto markov_config = make_markov_config(
      {
          0,
          1,
      },
      {
          0.5,
          0.5,
          0.5,
          0.5,
      });

  const auto multicut = run_mode(CutSharingMode::multicut, nullptr);
  const auto markov = run_mode(CutSharingMode::markov, &markov_config);

  // Identical LB trajectory, iteration by iteration.
  REQUIRE(markov.results.size() == multicut.results.size());
  for (std::size_t i = 0; i < multicut.results.size(); ++i) {
    INFO("iter=", i);
    CHECK(markov.results[i].lower_bound
          == doctest::Approx(multicut.results[i].lower_bound).epsilon(1.0e-7));
  }

  // Identical cuts: match on (scene_uid, phase_uid, iteration) and
  // compare the physical row (rhs + reservoir coefficient) plus the
  // cut value across the state grid.
  REQUIRE(markov.optimality_cuts == multicut.optimality_cuts);
  REQUIRE(markov.cut_views.size() == multicut.cut_views.size());
  REQUIRE(markov.cut_views.size() >= 2);

  const auto grid = oracle_state_grid();

  for (const auto& [id, mc_view] : multicut.cut_views) {
    const auto it = markov.cut_views.find(id);
    REQUIRE_MESSAGE(it != markov.cut_views.end(),
                    "multicut cut id missing from the markov run");
    const auto& mk_view = it->second;

    CHECK(mk_view.rhs == doctest::Approx(mc_view.rhs).epsilon(1.0e-7));
    CHECK(mk_view.state_coeff
          == doctest::Approx(mc_view.state_coeff).epsilon(1.0e-7));
    for (const double x : grid) {
      CHECK(
          oracle_cut_value_at(mk_view, x)
          == doctest::Approx(oracle_cut_value_at(mc_view, x)).epsilon(1.0e-7));
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
// (b) M = 1 — the future term collapses to a single column priced p_s
//     (§4.2 of sddp-markov.md).  Every scene's cut lands on the shared
//     varphi_0, whose theorem-MK1 target is the probability-weighted
//     mean tail Φ_0(x) = Σ_s p_s·Ṽ_s(x): under A2 each per-scene folded
//     tail is dominated by the sum (the Lemma-A1-style within-state
//     domination step), so every FINAL-transition cut must
//     underestimate Φ_0.  Earlier transitions bound the
//     Markov-modulated (resampled) value — not the persistent tail —
//     so, exactly like the heterogeneous multicut case, they are not
//     audited against this oracle.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — markov M=1: final-transition cuts underestimate "
    "the probability-weighted mean tail")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::markov,
                               /*max_iterations=*/6);
  opts.markov = make_markov_config(
      {
          0,
          0,
      },
      {
          1.0,
      });
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.5, .inflow = kOracleWetInflow},
      {.prob = 0.5, .inflow = kOracleDryInflow},
  }};

  const auto grid = oracle_state_grid();

  // Φ_0(x) = Σ_s (p_s-folded 1-stage tail) at each grid point.
  std::array<double, kOracleGridN> phi {};
  for (std::size_t i = 0; i < kOracleGridN; ++i) {
    for (const auto& sd : scenes) {
      phi[i] += oracle_tail_value(1, sd.prob, sd.inflow, grid[i]);
    }
  }

  int n_checked = 0;
  for (const auto& sc : sddp.stored_cuts()) {
    if (sc.type != CutType::Optimality) {
      continue;
    }
    const auto view = oracle_classify_cut(plp.simulation(), sc);
    const int tail_stages = kOracleNumPhases - 1 - view.phase_pos;
    if (tail_stages != 1) {
      continue;  // final transition only (see the header note)
    }
    for (std::size_t i = 0; i < kOracleGridN; ++i) {
      const double cut_val = oracle_cut_value_at(view, grid[i]);
      const double tol = oracle_tol(kOracleEmax, std::abs(phi[i]));
      INFO("scene=",
           view.scene_pos,
           " iter=",
           static_cast<int>(sc.iteration_index),
           " x=",
           grid[i],
           " cut=",
           cut_val,
           " phi=",
           phi[i]);
      CHECK(cut_val <= phi[i] + tol);
    }
    ++n_checked;
  }
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);
}

// ═════════════════════════════════════════════════════════════════════════
// (c) Identical scenes, 2 states, non-trivial transition matrix: with
//     identical dynamics every state's post-draw value coincides with
//     the persistent tail (U_r ≡ Ṽ^pers for any row-stochastic P), so
//     the persistent per-scene tail oracle is EXACT at every
//     transition — strict per-cut checks everywhere, plus the strict
//     LB property against the persistent extensive optimum (theorem
//     MK1; docs/formulation/sddp-markov.md §5).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — markov on identical scenes (2 states, "
    "non-uniform transition): cuts underestimate the common tail at "
    "every transition")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::markov,
                               /*max_iterations=*/6);
  // Singleton states; deliberately non-uniform rows so the
  // transition-dependent pricing w_{s,m'} = p_s·P[m(s)][m']/pi_{m'} is
  // genuinely exercised (identical dynamics make the value functions
  // P-invariant, so the oracle stays exact).
  opts.markov = make_markov_config(
      {
          0,
          1,
      },
      {
          0.7,
          0.3,
          0.2,
          0.8,
      });
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.5, .inflow = kOracleBaseInflow},
      {.prob = 0.5, .inflow = kOracleBaseInflow},
  }};

  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  // Identical scenes: the Markov-modulated optimum equals the
  // persistent one for any P, so the LB property is strict.
  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/true);
}

TEST_CASE(  // NOLINT
    "SDDP cut oracle — multicut identical dynamics with non-uniform "
    "probabilities (0.6/0.4): M4 pricing keeps every cut a strict "
    "underestimator")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.6, .inflow = kOracleBaseInflow},
      {.prob = 0.4, .inflow = kOracleBaseInflow},
  }};

  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  // Identical dynamics: resampled ≡ persistent, so the LB property is
  // strict against the persistent extensive optimum too (theorem M4
  // degenerate case).
  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/true);
}

// ═════════════════════════════════════════════════════════════════════════
// Mode `multicut`, HETEROGENEOUS dynamics, NON-uniform probabilities
// (0.6/0.4): under M4 pricing the cuts bound the q_r = p_r RESAMPLED
// tree, which is a different function from the persistent per-scene
// tails swept below — same process-mismatch status as the uniform
// heterogeneous case (Corollary M2), so the persistent-tail sweep stays
// WARN-only (demonstrative, not a regression signal).  Before the M4
// fix this case was additionally unsound (theorem M3: the 1/N pricing
// certified no process at all); M4 removed the unsoundness but not the
// process mismatch.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — multicut heterogeneous non-uniform (0.6/0.4): "
    "persistent-tail comparison is process-mismatched (WARN-only)")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.6, .inflow = kOracleWetInflow},
      {.prob = 0.4, .inflow = kOracleDryInflow},
  }};

  // WARN-only sweep against the persistent per-scene tails — under M4
  // the cuts bound the resampled tree (a different function), so
  // violations here are expected and demonstrative, not regressions.
  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/false);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/false);
}

// ═════════════════════════════════════════════════════════════════════════
// `forward_sampling = resampled` (2026-07-08): the forward pass itself
// simulates the stagewise-resampled process (per-phase-boundary
// probability-weighted draw applied through the bound-only
// `update_aperture` machinery; deterministic in (iteration, scene,
// phase)).  On IDENTICAL dynamics every drawn realization pins the SAME
// bound values, so resampled ≡ persistent and the full strict M4 sweep
// must hold unchanged — cuts and LB against the persistent tail oracle.
// On HETEROGENEOUS dynamics the pure-Benders backward builds
// scenario-s's cut from scene-s's LP carrying the SAMPLED realization's
// bounds (the same realization the forward pass solved — cached on the
// phase state), which certifies nothing against the persistent tails —
// WARN-only, same tier as the persistent-forward heterogeneous case
// (theorem doc §8 remark; the certified heterogeneous route is the
// aperture backward pass, out of scope for this harness).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — resampled forward sampling, identical dynamics "
    "(0.6/0.4 multicut): strict M4 sweep unchanged")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  opts.forward_sampling = ForwardSamplingMode::resampled;
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.6, .inflow = kOracleBaseInflow},
      {.prob = 0.4, .inflow = kOracleBaseInflow},
  }};

  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/true);
}

TEST_CASE(  // NOLINT
    "SDDP cut oracle — resampled forward sampling, heterogeneous "
    "dynamics (0.6/0.4 multicut): persistent-tail sweep WARN-only")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
  PlanningLP plp(std::move(planning));

  auto opts = oracle_sddp_opts(CutSharingMode::multicut,
                               /*max_iterations=*/6);
  opts.forward_sampling = ForwardSamplingMode::resampled;
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const std::array<OracleSceneData, 2> scenes = {{
      {.prob = 0.6, .inflow = kOracleWetInflow},
      {.prob = 0.4, .inflow = kOracleDryInflow},
  }};

  // WARN-only: under resampled forward sampling the cut for varphi_s is
  // built at whatever realization the forward pass drew for that cell,
  // so the persistent-tail comparison is demonstrative (v1 caveat —
  // theorem doc §8 remark), exactly like the persistent-forward
  // heterogeneous case above.
  const auto cuts = sddp.stored_cuts();
  const int n_checked =
      oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/false);
  CAPTURE(n_checked);
  REQUIRE(n_checked >= 2);

  oracle_audit_lower_bounds(*results,
                            oracle_persistent_extensive_optimum(scenes),
                            /*strict=*/false);
}

// ═════════════════════════════════════════════════════════════════════════
// APERTURE ECUTS — synthetic apertures (q = 1/N), final transition.
// The last phase has no future term, so each per-aperture cut is an
// exact support of the single-stage tail under that aperture's inflow
// (Theorem O1 on the clone), and the expected cut underestimates the
// q-mixture V_mix = p_s × ½ (tail_wet + tail_dry) (Theorem AP1).  This
// holds for EVERY solve mode: `cold` (exact per-aperture solves),
// `dual_shared` (representative solve + Lemma AP2 synthesis) and
// `screened` (synthesis + top-|corr| exact re-solves).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — aperture ecuts (synthetic q=1/N): final-transition "
    "mixture audit across cold/dual_shared/screened")
{
  const std::array<std::pair<ApertureSolveMode, const char*>, 3> modes = {{
      {ApertureSolveMode::cold, "cold"},
      {ApertureSolveMode::dual_shared, "dual_shared"},
      {ApertureSolveMode::screened, "screened"},
  }};

  for (const auto& [mode, label] : modes) {
    SUBCASE(label)
    {
      auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
      oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
      PlanningLP plp(std::move(planning));

      auto opts = oracle_aperture_sddp_opts(mode,
                                            {Uid {1}, Uid {2}},
                                            /*max_iterations=*/6,
                                            /*screen_count=*/1);
      SDDPMethod sddp(plp, opts);
      auto results = sddp.solve();
      REQUIRE(results.has_value());
      REQUIRE_FALSE(results->empty());

      const auto cuts = sddp.stored_cuts();
      const int n_checked =
          oracle_audit_aperture_cuts(plp.simulation(),
                                     cuts,
                                     /*scene_probs=*/ {0.6, 0.4},
                                     {kOracleWetInflow, kOracleDryInflow},
                                     /*strict=*/true,
                                     /*only_tail_stages=*/1);
      CAPTURE(n_checked);
      REQUIRE(n_checked >= 2);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
// APERTURE ECUTS — identical scenes AND identical aperture inflows: the
// aperture-modified process coincides with the persistent one, so the
// persistent tail oracle applies at EVERY transition and the LB is
// bounded by the persistent extensive optimum — strict for the
// dual-shared modes (their synthesized intercepts must not overshoot
// anywhere in the recursion, not just at the terminal transition).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — dual_shared/screened on identical scenes: strict "
    "at every transition, LB ≤ extensive optimum")
{
  const std::array<std::pair<ApertureSolveMode, const char*>, 2> modes = {{
      {ApertureSolveMode::dual_shared, "dual_shared"},
      {ApertureSolveMode::screened, "screened"},
  }};

  for (const auto& [mode, label] : modes) {
    SUBCASE(label)
    {
      auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
      PlanningLP plp(std::move(planning));

      auto opts = oracle_aperture_sddp_opts(mode,
                                            {Uid {1}, Uid {2}},
                                            /*max_iterations=*/6,
                                            /*screen_count=*/1);
      SDDPMethod sddp(plp, opts);
      auto results = sddp.solve();
      REQUIRE(results.has_value());
      REQUIRE_FALSE(results->empty());

      const std::array<OracleSceneData, 2> scenes = {{
          {.prob = 0.5, .inflow = kOracleBaseInflow},
          {.prob = 0.5, .inflow = kOracleBaseInflow},
      }};

      // Identical inflows: mixture == persistent tail — the plain
      // per-scene oracle audits every transition.
      const auto cuts = sddp.stored_cuts();
      const int n_checked =
          oracle_audit_cuts(plp.simulation(), cuts, scenes, /*strict=*/true);
      CAPTURE(n_checked);
      REQUIRE(n_checked >= 2);

      oracle_audit_lower_bounds(*results,
                                oracle_persistent_extensive_optimum(scenes),
                                /*strict=*/true);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
// APERTURE ECUTS — K = 1 aperture: dual_shared ≡ cold exactly.  With a
// single aperture there is nothing to synthesize — the representative
// solve IS the whole chunk, so the two modes execute identical solves
// and must store identical cuts (rhs and coefficients).
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — single aperture: dual_shared and cold store "
    "identical cuts")
{
  const auto run_mode = [](ApertureSolveMode mode)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    oracle_set_scenario_inflows(planning, kOracleWetInflow, kOracleDryInflow);
    PlanningLP plp(std::move(planning));

    auto opts = oracle_aperture_sddp_opts(mode,
                                          {Uid {1}},
                                          /*max_iterations=*/4);
    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return sddp.stored_cuts();
  };

  const auto cold_cuts = run_mode(ApertureSolveMode::cold);
  const auto ds_cuts = run_mode(ApertureSolveMode::dual_shared);

  REQUIRE_FALSE(cold_cuts.empty());
  REQUIRE(cold_cuts.size() == ds_cuts.size());
  for (std::size_t i = 0; i < cold_cuts.size(); ++i) {
    const auto& a = cold_cuts[i];
    const auto& b = ds_cuts[i];
    INFO("cut #", i);
    CHECK(a.type == b.type);
    CHECK(a.scene_uid == b.scene_uid);
    CHECK(a.phase_uid == b.phase_uid);
    CHECK(a.rhs == doctest::Approx(b.rhs).epsilon(1.0e-9));
    REQUIRE(a.coefficients.size() == b.coefficients.size());
    for (std::size_t j = 0; j < a.coefficients.size(); ++j) {
      CHECK(a.coefficients[j].first == b.coefficients[j].first);
      CHECK(a.coefficients[j].second
            == doctest::Approx(b.coefficients[j].second).epsilon(1.0e-9));
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
// BENCHMARK (report only, no hard asserts): aperture solve modes on the
// 2-scene × 10-phase × 2-reservoir fixture.  `warm` is the production
// baseline and needs `aperture_chunk_size = -1` (fully serial per scene)
// for its within-chunk basis chain to engage; `cold` is the legacy
// per-aperture barrier; `dual_shared` / `screened` skip re-solves via
// Lemma AP2 synthesis.  Expectation on a CPU simplex/barrier backend
// (CLP/HiGHS/CPLEX): little to no wall-clock win — the per-aperture
// solves here are sub-millisecond and warm re-solves are already a few
// pivots.  The target backend is cuOpt/PDLP (no warm-start path at all;
// cut-validity ledger F10), where every skipped re-solve is a full cold
// PDLP run — that measurement needs a GPU box and is deferred.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP cut oracle — aperture solve-mode benchmark (report only)")
{
  struct BenchRow
  {
    std::string label;
    std::size_t iterations {};
    double gap {};
    double lb {};
    double wall_s {};
    double cpu_s {};
  };
  std::vector<BenchRow> rows;

  const auto run_bench = [&rows](const std::string& label,
                                 ApertureSolveMode mode,
                                 int chunk_size,
                                 int screen_count)
  {
    auto planning = make_2scene_10phase_two_reservoir_planning();
    PlanningLP plp(std::move(planning));

    SDDPOptions opts;
    opts.max_iterations = 30;
    opts.convergence_tol = 1.0e-4;
    opts.stationary_tol = 0.0;  // no early stationary exit — compare iters
    opts.cut_sharing = CutSharingMode::none;
    opts.apertures = std::vector<Uid> {Uid {1}, Uid {2}};
    opts.aperture_solve_mode = mode;
    opts.aperture_chunk_size = chunk_size;
    opts.aperture_screen_count = screen_count;
    opts.enable_api = false;

    SDDPMethod sddp(plp, opts);
    const auto wall_t0 = std::chrono::steady_clock::now();
    const auto cpu_t0 = std::clock();
    auto results = sddp.solve();
    const double cpu_s = static_cast<double>(std::clock() - cpu_t0)
        / static_cast<double>(CLOCKS_PER_SEC);
    const double wall_s = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - wall_t0)
                              .count();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    const auto& last = results->back();
    rows.push_back(BenchRow {
        .label = label,
        .iterations = results->size(),
        .gap = last.gap,
        .lb = last.lower_bound,
        .wall_s = wall_s,
        .cpu_s = cpu_s,
    });
  };

  run_bench("warm (chunk=-1, baseline)", ApertureSolveMode::warm, -1, 2);
  run_bench("cold", ApertureSolveMode::cold, 0, 2);
  run_bench("dual_shared", ApertureSolveMode::dual_shared, 0, 2);
  run_bench("screened (N=1)", ApertureSolveMode::screened, 0, 1);

  std::string report =
      "\nAperture solve-mode benchmark "
      "(2scene×10phase×2reservoir, tol=1e-4):\n";
  report += std::format("{:<28} {:>5} {:>12} {:>14} {:>9} {:>9}\n",
                        "mode",
                        "iters",
                        "gap",
                        "LB",
                        "wall[s]",
                        "cpu[s]");
  for (const auto& r : rows) {
    report +=
        std::format("{:<28} {:>5} {:>12.3e} {:>14.4f} {:>9.3f} {:>9.3f}\n",
                    r.label,
                    r.iterations,
                    r.gap,
                    r.lb,
                    r.wall_s,
                    r.cpu_s);
  }
  MESSAGE(report);

  // Soft sanity only (no perf asserts): every mode must produce a
  // finite, positive LB on this fixture.
  for (const auto& r : rows) {
    CHECK(std::isfinite(r.lb));
    CHECK(r.lb > 0.0);
  }
}
