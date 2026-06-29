// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_storage_trajectory.cpp
 * @brief     Per-phase SDDP storage-trajectory equivalence: an AMPL-text
 *            reservoir traces the SAME volume path as the native reservoir.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * The companion `test_user_storage_equivalence.cpp` matches the SDDP *bounds*
 * (the optimum cost) on the shipped flat-cost 2-reservoir fixture.  That
 * fixture is intentionally NOT used for a per-phase trajectory comparison:
 * with both reservoirs symmetric and the marginal substitution cost flat
 * across phases, the optimal *release timing* is degenerate (many primal
 * trajectories realise the same optimum), so the native LP and the
 * structurally-different AMPL LP may legitimately land on different vertices.
 *
 * This file adds a purpose-built NON-DEGENERATE fixture whose optimal storage
 * trajectory is UNIQUE, then asserts the AMPL reservoir reproduces it phase by
 * phase under `method=sddp`:
 *
 *   - 6 phases (1 block / phase), 2 deterministic (identical) scenes.
 *   - Single bus, time-varying demand `lmax = {50,70,30,80,60,40}`.
 *   - Two ASYMMETRIC hydro generators with DISTINCT costs (gcost 1 and 2) so
 *     the merit order is strict, plus an expensive thermal backup (gcost 100).
 *   - No inflow; reservoir 1 holds 250, reservoir 2 holds 120 (generous), so
 *     hydro covers all load and thermal stays off.
 *
 * Because hydro is strictly cheaper than thermal and the cheaper unit (gen1)
 * is used maximally in every phase (`min(cap, demand)`), the per-phase
 * generation split — and therefore each reservoir's volume path — is uniquely
 * determined and varies across phases.  Two structurally different LPs landing
 * on the SAME trajectory is then a meaningful 1:1 check (a degenerate optimum
 * would let them diverge).
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

constexpr int num_phases = 6;
// Per-phase served load (one stage / phase, one block / stage).  Distinct,
// varying values are what pin the unique trajectory.
constexpr std::array<double, num_phases> kDemand {50, 70, 30, 80, 60, 40};
// Reservoir 1 (cheap hydro) initial volume; reservoir 2 (dearer hydro).
constexpr double kEini1 = 250.0;
constexpr double kEini2 = 120.0;

/// All-native oracle: two reservoirs, each with its own hydro turbine; an
/// expensive thermal backup; time-varying demand.  See the file header for the
/// non-degeneracy argument.
[[nodiscard]] auto make_unique_trajectory_two_reservoir_planning() -> Planning
{
  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(num_phases), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases), 1);
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  // emin = 0 on every (stage, block).
  std::vector<std::vector<double>> emin_per_stage(num_phases,
                                                  std::vector<double> {
                                                      0.0,
                                                  });

  // Time-varying demand as a per-(stage, block) schedule.
  std::vector<std::vector<double>> lmax_per_stage;
  lmax_per_stage.reserve(static_cast<std::size_t>(num_phases));
  for (const double d : kDemand) {
    lmax_per_stage.push_back(std::vector<double> {
        d,
    });
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
          .gcost = 1.0,  // cheapest → used maximally every phase
          .capacity = 40.0,
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
          .capacity = 40.0,
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
          .capacity = 300.0,
          .emin = emin_per_stage,
          .emax = 300.0,
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
          .capacity = 300.0,
          .emin = emin_per_stage,
          .emax = 300.0,
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
    std::vector<std::vector<double>> zero_2d(num_phases,
                                             std::vector<double> {
                                                 0.0,
                                             });
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
              .name = "sddp_unique_traj_2rsv",
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
/// reservoir (a `block_state` DecisionVariable + a within/cross-phase balance).
/// Reservoir 2 stays native, so one solve mixes native + AMPL state coupling.
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

  // vol1: per-block storage state, starts at kEini1, bounded [0, 300].
  sys.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {101},
      .name = "vol1",
      .lower_bound = OptReal {0.0},
      .upper_bound = OptReal {300.0},
      .scope = OptName {"block"},
      .state = OptBool {},
      .link = OptBool {true},
      .block_state = OptBool {true},
      .initial_value = OptReal {kEini1},
  });

  // Balance (inflow = 0 on every stage): vol1 - prev(vol1) + generation = 0.
  // prev(vol1) at the first block resolves to the incoming column (the cross-
  // phase / first-stage initial value); the efin terminal (vol1 >= 0) is
  // already enforced by the DecisionVariable lower bound.
  sys.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {201},
      .name = "vol1_balance",
      .expression = "decision_variable('vol1').value "
                    "- prev(decision_variable('vol1').value) "
                    "+ generator('hydro_gen_1').generation = 0",
  });

  return p;
}

/// Run SDDP (cut_sharing=none) on `plp` in place.  `PlanningLP` is not movable,
/// so the caller owns it and keeps it alive afterwards — the per-phase
/// forward-pass solutions stay readable from each (scene, phase) SystemLP.
void run_sddp(PlanningLP& plp)
{
  SDDPOptions opts;
  opts.max_iterations = 12;
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

/// End-of-phase volume of a NATIVE reservoir (the efin / cross-phase state
/// column) across all phases of scene 0, from the converged forward pass.
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
    const auto efin_col = rsv->efin_col_at(scenario, stage);
    traj.push_back(sysp.linear_interface().get_col_sol()[efin_col]);
  }
  return traj;
}

/// End-of-phase volume of a `block_state` DecisionVariable (its last-block
/// column == the registered cross-phase state) across all phases of scene 0.
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
    const auto last_block_uid = stage.blocks().back().uid();  // end-of-phase
    const auto it = bmap.find(last_block_uid);
    REQUIRE(it != bmap.end());
    traj.push_back(sysp.linear_interface().get_col_sol()[it->second]);
  }
  return traj;
}

[[nodiscard]] auto spread(const std::vector<double>& v) -> double
{
  const auto [lo, hi] = std::ranges::minmax_element(v);
  return *hi - *lo;
}

}  // namespace
}  // namespace user_storage_traj_test

TEST_CASE(
    "AMPL-text reservoir traces the native reservoir's per-phase SDDP "
    "trajectory (non-degenerate 2-reservoir 6-phase)")
{
  using namespace user_storage_traj_test;  // NOLINT(google-build-using-namespace)

  PlanningLP native(as_sddp(make_unique_trajectory_two_reservoir_planning()));
  run_sddp(native);
  PlanningLP candidate(as_sddp(make_ampl_trajectory_candidate()));
  run_sddp(candidate);

  // Reservoir-1 trajectory: native efin path vs the AMPL block_state path.
  const auto rsv1_native = native_reservoir_trajectory(native, Uid {1});
  const auto rsv1_ampl = block_state_trajectory(candidate, Uid {101});
  // Reservoir 2 stays native in both models — it must be unperturbed.
  const auto rsv2_native = native_reservoir_trajectory(native, Uid {2});
  const auto rsv2_cand = native_reservoir_trajectory(candidate, Uid {2});

  REQUIRE(static_cast<int>(rsv1_native.size()) == num_phases);
  REQUIRE(static_cast<int>(rsv1_ampl.size()) == num_phases);

  // The trajectory must genuinely move across phases — a flat path would make
  // the comparison vacuous (and signal a degenerate / mis-built fixture).
  CHECK(spread(rsv1_native) > 50.0);
  CHECK(spread(rsv2_native) > 20.0);

  // Hand-computed unique optimum (gen1 used maximally = min(40, demand) each
  // phase; gen2 serves the remainder):
  //   gen1 = {40,40,30,40,40,40}, cumsum {40,80,110,150,190,230}
  //     -> rsv1 = 250 - cumsum = {210,170,140,100,60,20}
  //   gen2 = {10,30, 0,40,20, 0}, cumsum {10,40, 40, 80,100,100}
  //     -> rsv2 = 120 - cumsum = {110, 80, 80, 40, 20, 20}
  const std::vector<double> rsv1_expected {210, 170, 140, 100, 60, 20};
  const std::vector<double> rsv2_expected {110, 80, 80, 40, 20, 20};

  for (int i = 0; i < num_phases; ++i) {
    // Core 1:1 check: AMPL reservoir == native reservoir, phase by phase.
    CHECK(rsv1_ampl[static_cast<std::size_t>(i)]
          == doctest::Approx(rsv1_native[static_cast<std::size_t>(i)])
                 .epsilon(1e-4));
    // The unperturbed native reservoir 2 is identical in both solves.
    CHECK(rsv2_cand[static_cast<std::size_t>(i)]
          == doctest::Approx(rsv2_native[static_cast<std::size_t>(i)])
                 .epsilon(1e-4));
    // Both match the hand-derived unique trajectory (locks the numbers).
    CHECK(rsv1_native[static_cast<std::size_t>(i)]
          == doctest::Approx(rsv1_expected[static_cast<std::size_t>(i)])
                 .epsilon(1e-3));
    CHECK(rsv2_native[static_cast<std::size_t>(i)]
          == doctest::Approx(rsv2_expected[static_cast<std::size_t>(i)])
                 .epsilon(1e-3));
  }
}
