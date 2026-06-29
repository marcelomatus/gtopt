// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_storage_trajectory.cpp
 * @brief     Per-(stage, block) SDDP storage-trajectory equivalence: an
 *            AMPL-text reservoir traces the SAME volume path as the native
 *            reservoir, with a realistic 4-blocks-per-stage model.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * The companion `test_user_storage_equivalence.cpp` matches the SDDP *bounds*
 * (the optimum cost) on the shipped flat-cost 2-reservoir fixture.  That
 * fixture is intentionally NOT used for a trajectory comparison: with both
 * reservoirs symmetric and the marginal substitution cost flat across time, the
 * optimal *release timing* is degenerate (many primal trajectories realise the
 * same optimum), so the native LP and the structurally-different AMPL LP may
 * legitimately land on different vertices.
 *
 * This file adds a purpose-built NON-DEGENERATE fixture whose optimal storage
 * trajectory is UNIQUE, modelled the way a reservoir actually should be — with
 * MULTIPLE CHRONOLOGICAL BLOCKS PER STAGE (4 here).  That exercises the full
 * `block_state` recurrence under `method=sddp`:
 *
 *   - the WITHIN-stage lag: `prev(vol)` at block b>0 resolves to block b-1;
 *   - the CROSS-stage / cross-phase incoming: block 0 of each stage chains to
 *     the previous stage's end-of-stage volume (the SDDP state).
 *
 * Fixture:
 *   - 6 phases (1 stage / phase), 4 chronological blocks / stage with
 *     NON-UNIFORM durations {6, 4, 2, 12} h (a realistic load-block shape).
 *   - 2 deterministic (identical) scenes, single bus.
 *   - Time-varying per-(stage, block) demand `lmax` (see kDemand2D).
 *   - Two ASYMMETRIC hydro generators with DISTINCT costs (gcost 1 and 2) so
 *     the merit order is strict, plus an expensive thermal backup (gcost 100).
 *   - No inflow; the reservoirs draw down from a generous initial volume.
 *
 * The non-uniform durations make this a duration-CORRECTNESS test: a release of
 * P MW over a Δt-hour block draws P·Δt of volume, so the AMPL balance
 * multiplies the release by `block.duration` to match the native StorageLP
 * (which scales its flow column by `block.duration()`).  A duration-1 fixture
 * would pass even with the Δt factor missing; this one would not.
 *
 * Because hydro is strictly cheaper than thermal and the cheaper unit (gen1) is
 * used maximally in every block (`min(cap, demand_power)`), the per-block
 * generation split — and therefore each reservoir's full volume path — is
 * uniquely determined and varies block to block.  Two structurally different
 * LPs landing on the SAME 24-block trajectory is then a meaningful 1:1 (a
 * degenerate optimum would let them diverge).  The expected path is computed in
 * the test directly from the demand schedule × durations, self-documenting.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/utils.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace user_storage_traj_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

constexpr int num_stages = 6;
constexpr int blocks_per_stage = 4;
constexpr int total_blocks = num_stages * blocks_per_stage;

// Per-(stage, block) served load.  A peak/shoulder/off/evening shape that
// varies stage to stage; every value <= cap1 + cap2 = 60 so thermal stays off.
constexpr std::array<std::array<double, blocks_per_stage>, num_stages>
    kDemand2D {{
        {50, 30, 20, 45},
        {55, 35, 25, 40},
        {40, 20, 15, 50},
        {60, 40, 30, 35},
        {45, 25, 20, 55},
        {50, 30, 25, 40},
    }};

// NON-UNIFORM block durations (hours) — a realistic load-block shape
// (off-peak / shoulder / peak / night).  These make the reservoir balance
// duration-sensitive: a release of P MW over a Δt-hour block draws P·Δt of
// volume, so the AMPL balance MUST scale generation by `block.duration` to
// match the native StorageLP.  Same shape every stage; sums to 24 h/stage.
constexpr std::array<double, blocks_per_stage> kBlockDur {6.0, 4.0, 2.0, 12.0};

constexpr double kCap1 = 30.0;  // cheaper hydro (gen1) capacity
constexpr double kCap2 = 30.0;  // dearer hydro (gen2) capacity
// Generous initial volumes so water never binds — the trajectory stays the
// forced demand-driven drawdown (gen1 maxed each block), now in ENERGY terms.
constexpr double kEini1 = 5000.0;  // reservoir 1 initial volume
constexpr double kEini2 = 5000.0;  // reservoir 2 initial volume

/// All-native oracle: two reservoirs, each with its own hydro turbine; an
/// expensive thermal backup; time-varying per-block demand; 4 chronological
/// blocks per stage.  See the file header for the non-degeneracy argument.
[[nodiscard]] auto make_unique_trajectory_two_reservoir_planning() -> Planning
{
  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  // Stamp the non-uniform per-block durations (same shape every stage).
  for (int i = 0; i < total_blocks; ++i) {
    block_array[static_cast<std::size_t>(i)].duration =
        kBlockDur[static_cast<std::size_t>(i % blocks_per_stage)];
  }
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_stages),
                          static_cast<std::size_t>(blocks_per_stage));
  // The within-stage `prev()` lag requires chronological blocks.
  for (auto& st : stage_array) {
    st.chronological = OptBool {true};
  }
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_stages));

  // emin = 0 and per-(stage, block) demand schedules.
  std::vector<std::vector<double>> emin_per_stage(
      num_stages, std::vector<double>(blocks_per_stage, 0.0));
  std::vector<std::vector<double>> lmax_per_stage;
  lmax_per_stage.reserve(static_cast<std::size_t>(num_stages));
  for (const auto& stage_demand : kDemand2D) {
    lmax_per_stage.emplace_back(stage_demand.begin(), stage_demand.end());
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
          .name = "hydro_gen_1",
          .bus = Uid {1},
          .gcost = 1.0,  // cheapest → used maximally every block
          .capacity = kCap1,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,  // expensive backup (stays off here)
          .capacity = 200.0,
      },
      {
          .uid = Uid {3},
          .name = "hydro_gen_2",
          .bus = Uid {1},
          .gcost = 2.0,  // dearer than gen1, far cheaper than thermal
          .capacity = kCap2,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .lmax = lmax_per_stage,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up1",
      },
      {
          .uid = Uid {2},
          .name = "j_up2",
      },
      {
          .uid = Uid {3},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww2",
          .junction_a = Uid {2},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 6000.0,
          .emin = emin_per_stage,
          .emax = 6000.0,
          .eini = kEini1,
          .efin = 0.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {2},
          .capacity = 6000.0,
          .emin = emin_per_stage,
          .emax = 6000.0,
          .eini = kEini2,
          .efin = 0.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  // No natural inflow: the reservoirs draw down from their initial volume.
  Array<Flow> flow_array;
  {
    std::vector<std::vector<double>> zero_2d(
        num_stages, std::vector<double>(blocks_per_stage, 0.0));
    std::vector<std::vector<std::vector<double>>> zero_3d {zero_2d, zero_2d};

    Flow flow1;
    flow1.uid = Uid {1};
    flow1.name = "inflow_1";
    flow1.direction = 1;
    flow1.junction = Uid {1};
    flow1.discharge = STBRealFieldSched {zero_3d};
    flow_array.push_back(std::move(flow1));

    Flow flow2;
    flow2.uid = Uid {2};
    flow2.name = "inflow_2";
    flow2.direction = 1;
    flow2.junction = Uid {2};
    flow2.discharge = STBRealFieldSched {std::move(zero_3d)};
    flow_array.push_back(std::move(flow2));
  }

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "tur2",
          .waterway = Uid {2},
          .generator = Uid {3},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = "scene1",
                  .active = true,
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = "scene2",
                  .active = true,
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 10'000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_unique_traj_2rsv_4blk",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = std::move(flow_array),
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Candidate: replace reservoir 1's native sub-topology with an AMPL-text
/// reservoir (a `block_state` DecisionVariable + a per-block within/cross-stage
/// balance).  Reservoir 2 stays native, so one solve mixes native + AMPL state
/// coupling.
[[nodiscard]] auto make_ampl_trajectory_candidate() -> Planning
{
  auto p = make_unique_trajectory_two_reservoir_planning();
  auto& sys = p.system;

  // Drop reservoir 1's whole native chain (all uid == 1).  hydro_gen_1 stays —
  // it is now the release variable driven by the balance below.
  std::erase_if(sys.reservoir_array,
                [](const Reservoir& r) { return r.uid == Uid {1}; });
  std::erase_if(sys.junction_array,
                [](const Junction& j) { return j.uid == Uid {1}; });
  std::erase_if(sys.waterway_array,
                [](const Waterway& w) { return w.uid == Uid {1}; });
  std::erase_if(sys.turbine_array,
                [](const Turbine& t) { return t.uid == Uid {1}; });
  std::erase_if(sys.flow_array, [](const Flow& f) { return f.uid == Uid {1}; });

  // vol1: per-block storage state, starts at kEini1, bounded [0, 6000].
  sys.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {101},
      .name = "vol1",
      .lower_bound = OptReal {0.0},
      .upper_bound = OptReal {6000.0},
      .scope = OptName {"block"},
      .state = OptBool {},
      .link = OptBool {true},
      .block_state = OptBool {true},
      .initial_value = OptReal {kEini1},
  });

  // Per-block balance (inflow = 0):
  //   vol1[b] - prev(vol1[b]) + generation[b] * block.duration = 0.
  // The `* block.duration` Δt weight turns the release POWER into the VOLUME
  // drawn over the block — matching the native StorageLP balance, which scales
  // its flow column by `block.duration()`.  Without it the balance would be
  // wrong for every block whose duration != 1 h.  prev(vol1) at block 0 of a
  // stage resolves to the incoming column (the previous stage's end-of-stage
  // volume, or the initial value on the very first stage); at block b>0 it
  // resolves to block b-1.  The efin terminal (vol1 >= 0) is the lower bound.
  sys.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {201},
      .name = "vol1_balance",
      .expression =
          "decision_variable('vol1').value "
          "- prev(decision_variable('vol1').value) "
          "+ generator('hydro_gen_1').generation * block.duration = 0",
  });

  return p;
}

/// Run SDDP (cut_sharing=none) on `plp` in place.  `PlanningLP` is not movable,
/// so the caller owns it and keeps it alive afterwards — the per-phase
/// forward-pass solutions stay readable from each (scene, phase) SystemLP.
void run_sddp(PlanningLP& plp)
{
  SDDPOptions opts;
  opts.max_iterations = 16;
  opts.convergence_tol = 1.0e-6;
  opts.cut_sharing = CutSharingMode::none;
  opts.enable_api = false;

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
}

/// Build a planning, force method=sddp, and return it ready for PlanningLP.
[[nodiscard]] auto as_sddp(Planning planning) -> Planning
{
  planning.options.method = MethodType::sddp;
  return planning;
}

/// Per-(stage, block) volume of a NATIVE reservoir (its per-block `energy`
/// columns) across all stages of scene 0, from the converged forward pass.
/// Ordered (stage, block).
[[nodiscard]] auto native_reservoir_trajectory(PlanningLP& plp, Uid rsv_uid)
    -> std::vector<double>
{
  std::vector<double> traj;
  const auto& sim = plp.simulation();
  const auto n = static_cast<Index>(sim.phases().size());
  const auto scene = first_scene_index();
  const auto& scenario = sim.scenarios().front();  // scene 0's scenario

  for (const auto pi : iota_range<PhaseIndex>(0, n)) {
    auto& sysp = plp.system(scene, pi);
    sysp.rebuild_collections_if_needed();
    const auto& stage = sim.phases()[pi].stages().back();

    const ReservoirLP* rsv = nullptr;
    for (const auto& r : sysp.elements<ReservoirLP>()) {
      if (r.uid() == rsv_uid) {
        rsv = &r;
        break;
      }
    }
    REQUIRE(rsv != nullptr);
    const auto& emap =
        rsv->energy_cols_at(scenario, stage);  // block uid -> col
    for (const auto& block : stage.blocks()) {
      const auto it = emap.find(block.uid());
      REQUIRE(it != emap.end());
      traj.push_back(sysp.linear_interface().get_col_sol()[it->second]);
    }
  }
  return traj;
}

/// Per-(stage, block) volume of a `block_state` DecisionVariable across all
/// stages of scene 0.  Ordered (stage, block).
[[nodiscard]] auto block_state_trajectory(PlanningLP& plp, Uid dv_uid)
    -> std::vector<double>
{
  std::vector<double> traj;
  const auto& sim = plp.simulation();
  const auto n = static_cast<Index>(sim.phases().size());
  const auto scene = first_scene_index();
  const auto& scenario = sim.scenarios().front();

  for (const auto pi : iota_range<PhaseIndex>(0, n)) {
    auto& sysp = plp.system(scene, pi);
    sysp.rebuild_collections_if_needed();
    const auto& stage = sim.phases()[pi].stages().back();

    const DecisionVariableLP* dv = nullptr;
    for (const auto& d : sysp.elements<DecisionVariableLP>()) {
      if (d.uid() == dv_uid) {
        dv = &d;
        break;
      }
    }
    REQUIRE(dv != nullptr);
    const auto& bmap = dv->value_cols_at(scenario, stage);  // block uid -> col
    for (const auto& block : stage.blocks()) {
      const auto it = bmap.find(block.uid());
      REQUIRE(it != bmap.end());
      traj.push_back(sysp.linear_interface().get_col_sol()[it->second]);
    }
  }
  return traj;
}

/// Unique optimal per-block volume path for the reservoir feeding `gen1` (the
/// cheaper unit, when `is_gen1`) or `gen2` (the remainder).  gen1 is used
/// maximally each block (min(cap, demand_POWER)); gen2 serves what's left.
/// The VOLUME drawn over a block is the release power × the block duration
/// (Δt), so vol = eini − cumulative(release · Δt), chained continuously across
/// stages (each stage's incoming is the previous stage's end-of-stage volume).
[[nodiscard]] auto expected_trajectory(double eini, bool is_gen1)
    -> std::vector<double>
{
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(total_blocks));
  double cum = 0.0;
  for (const auto& stage_demand : kDemand2D) {
    for (std::size_t b = 0; b < blocks_per_stage; ++b) {
      const double d = stage_demand[b];
      const double g1 = std::min(kCap1, d);
      const double g2 = d - g1;
      cum += (is_gen1 ? g1 : g2) * kBlockDur[b];  // power × Δt = volume drawn
      v.push_back(eini - cum);
    }
  }
  return v;
}

[[nodiscard]] auto spread(const std::vector<double>& v) -> double
{
  const auto [lo, hi] = std::ranges::minmax_element(v);
  return *hi - *lo;
}

}  // namespace
}  // namespace user_storage_traj_test

TEST_CASE(
    "AMPL-text reservoir traces the native reservoir's per-(stage,block) SDDP "
    "trajectory (non-degenerate 2-reservoir, 6 stages x 4 blocks)")
{
  using namespace user_storage_traj_test;  // NOLINT(google-build-using-namespace)

  PlanningLP native(as_sddp(make_unique_trajectory_two_reservoir_planning()));
  run_sddp(native);
  PlanningLP candidate(as_sddp(make_ampl_trajectory_candidate()));
  run_sddp(candidate);

  // Reservoir-1 trajectory: native per-block energy vs the AMPL block_state.
  const auto rsv1_native = native_reservoir_trajectory(native, Uid {1});
  const auto rsv1_ampl = block_state_trajectory(candidate, Uid {101});
  // Reservoir 2 stays native in both models — it must be unperturbed.
  const auto rsv2_native = native_reservoir_trajectory(native, Uid {2});
  const auto rsv2_cand = native_reservoir_trajectory(candidate, Uid {2});

  REQUIRE(static_cast<int>(rsv1_native.size()) == total_blocks);
  REQUIRE(static_cast<int>(rsv1_ampl.size()) == total_blocks);
  REQUIRE(static_cast<int>(rsv2_native.size()) == total_blocks);

  // The trajectory must genuinely move block to block — a flat path would make
  // the comparison vacuous (and signal a degenerate / mis-built fixture).
  CHECK(spread(rsv1_native) > 100.0);
  CHECK(spread(rsv2_native) > 50.0);

  // Unique optimum, computed directly from the demand schedule.
  const auto rsv1_expected = expected_trajectory(kEini1, /*is_gen1=*/true);
  const auto rsv2_expected = expected_trajectory(kEini2, /*is_gen1=*/false);

  for (int i = 0; i < total_blocks; ++i) {
    const auto k = static_cast<std::size_t>(i);
    // Core 1:1 check: AMPL reservoir == native reservoir, block by block.
    CHECK(rsv1_ampl[k] == doctest::Approx(rsv1_native[k]).epsilon(1e-4));
    // The unperturbed native reservoir 2 is identical in both solves.
    CHECK(rsv2_cand[k] == doctest::Approx(rsv2_native[k]).epsilon(1e-4));
    // Both match the unique demand-driven trajectory.
    CHECK(rsv1_native[k] == doctest::Approx(rsv1_expected[k]).epsilon(1e-3));
    CHECK(rsv2_native[k] == doctest::Approx(rsv2_expected[k]).epsilon(1e-3));
  }
}
