// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_simulation_lp_owning_lp.cpp
 * @brief     Tests for SimulationLP::scene_of / phase_of / owning_lp_of /
 *            unique_owning_lp_of — the (scenario, stage) → (scene, phase)
 *            factored lookup added in Phase 1a of the AMPL registry overhaul.
 *
 * The factored lookup is the foundation for partitioning the PAMPL
 * registry by `(SceneIndex, PhaseIndex)`.  These tests cover:
 *
 *   - Monolithic mode (1 scene, 1 phase) — every uid resolves to (0, 0).
 *   - Multi-scene / multi-phase mode — each scenario / stage resolves to
 *     its declared owning scene / phase.
 *   - Unknown uids return nullopt.
 *   - `unique_owning_lp_of` accepts spans of `(scenario, stage)` and
 *     returns the unique LP iff every entry maps to the same (scene,
 *     phase); mismatches and unknown entries return nullopt.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

[[nodiscard]] Simulation make_monolithic_sim()
{
  return Simulation {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {10},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {100},
              },
          },
  };
}

[[nodiscard]] Simulation make_multiscene_sim()
{
  // 2 scenes × 2 scenarios each, 2 phases × 2 stages each, 1 block / stage.
  Simulation sim;
  sim.block_array = {
      {
          .uid = Uid {1},
          .duration = 1,
      },
      {
          .uid = Uid {2},
          .duration = 1,
      },
      {
          .uid = Uid {3},
          .duration = 1,
      },
      {
          .uid = Uid {4},
          .duration = 1,
      },
  };
  sim.stage_array = {
      {
          .uid = Uid {10},
          .first_block = 0,
          .count_block = 1,
      },
      {
          .uid = Uid {11},
          .first_block = 1,
          .count_block = 1,
      },
      {
          .uid = Uid {12},
          .first_block = 2,
          .count_block = 1,
      },
      {
          .uid = Uid {13},
          .first_block = 3,
          .count_block = 1,
      },
  };
  sim.phase_array = {
      {
          .uid = Uid {20},
          .active = {},
          .first_stage = 0,
          .count_stage = 2,
          .apertures = {},
      },
      {
          .uid = Uid {21},
          .active = {},
          .first_stage = 2,
          .count_stage = 2,
          .apertures = {},
      },
  };
  sim.scenario_array = {
      {
          .uid = Uid {100},
      },
      {
          .uid = Uid {101},
      },
      {
          .uid = Uid {102},
      },
      {
          .uid = Uid {103},
      },
  };
  sim.scene_array = {
      {
          .uid = Uid {30},
          .name = {},
          .active = {},
          .first_scenario = 0,
          .count_scenario = 2,
      },
      {
          .uid = Uid {31},
          .name = {},
          .active = {},
          .first_scenario = 2,
          .count_scenario = 2,
      },
  };
  return sim;
}

}  // namespace

TEST_CASE("SimulationLP::scene_of / phase_of — monolithic")  // NOLINT
{
  const auto sim = make_monolithic_sim();
  const PlanningOptionsLP options(PlanningOptions {});
  const SimulationLP slp(sim, options);

  CHECK(slp.scene_of(make_uid<Scenario>(100)).value_or(SceneIndex {99})
        == first_scene_index());
  CHECK(slp.phase_of(StageUid {10}).value_or(PhaseIndex {99})
        == first_phase_index());

  // Unknown uids return nullopt.
  CHECK_FALSE(slp.scene_of(make_uid<Scenario>(999)).has_value());
  CHECK_FALSE(slp.phase_of(StageUid {999}).has_value());

  const auto own = slp.owning_lp_of(make_uid<Scenario>(100), StageUid {10});
  const auto own_val =
      own.value_or(std::pair {SceneIndex {99}, PhaseIndex {99}});
  CHECK(own_val == std::pair {first_scene_index(), first_phase_index()});
}

TEST_CASE("SimulationLP::scene_of / phase_of — multi-scene / multi-phase")
// NOLINT
{
  const auto sim = make_multiscene_sim();
  const PlanningOptionsLP options(PlanningOptions {});
  const SimulationLP slp(sim, options);

  // Scene 0 owns scenarios 100, 101.  Scene 1 owns 102, 103.
  CHECK(slp.scene_of(make_uid<Scenario>(100)).value_or(SceneIndex {99})
        == first_scene_index());
  CHECK(slp.scene_of(make_uid<Scenario>(101)).value_or(SceneIndex {99})
        == first_scene_index());
  CHECK(slp.scene_of(make_uid<Scenario>(102)).value_or(SceneIndex {99})
        == SceneIndex {1});
  CHECK(slp.scene_of(make_uid<Scenario>(103)).value_or(SceneIndex {99})
        == SceneIndex {1});

  // Phase 0 owns stages 10, 11.  Phase 1 owns 12, 13.
  CHECK(slp.phase_of(StageUid {10}).value_or(PhaseIndex {99})
        == first_phase_index());
  CHECK(slp.phase_of(StageUid {11}).value_or(PhaseIndex {99})
        == first_phase_index());
  CHECK(slp.phase_of(StageUid {12}).value_or(PhaseIndex {99})
        == PhaseIndex {1});
  CHECK(slp.phase_of(StageUid {13}).value_or(PhaseIndex {99})
        == PhaseIndex {1});
}

TEST_CASE("SimulationLP::owning_lp_of — multi-scene factored lookup")  // NOLINT
{
  const auto sim = make_multiscene_sim();
  const PlanningOptionsLP options(PlanningOptions {});
  const SimulationLP slp(sim, options);

  constexpr std::pair sentinel {SceneIndex {99}, PhaseIndex {99}};

  // (scenario in scene 0, stage in phase 0) → (0, 0)
  CHECK(slp.owning_lp_of(make_uid<Scenario>(100), StageUid {10})
            .value_or(sentinel)
        == std::pair {first_scene_index(), first_phase_index()});

  // (scenario in scene 1, stage in phase 1) → (1, 1)
  CHECK(slp.owning_lp_of(make_uid<Scenario>(103), StageUid {13})
            .value_or(sentinel)
        == std::pair {SceneIndex {1}, PhaseIndex {1}});

  // Cross combinations are valid: scene 1 × phase 0 = (1, 0).
  CHECK(slp.owning_lp_of(make_uid<Scenario>(102), StageUid {11})
            .value_or(sentinel)
        == std::pair {SceneIndex {1}, first_phase_index()});

  // Unknown uid on either side → nullopt.
  CHECK_FALSE(
      slp.owning_lp_of(make_uid<Scenario>(999), StageUid {10}).has_value());
  CHECK_FALSE(
      slp.owning_lp_of(make_uid<Scenario>(100), StageUid {999}).has_value());
}

TEST_CASE("SimulationLP::unique_owning_lp_of — agreement and conflicts")
// NOLINT
{
  const auto sim = make_multiscene_sim();
  const PlanningOptionsLP options(PlanningOptions {});
  const SimulationLP slp(sim, options);

  constexpr std::pair sentinel {SceneIndex {99}, PhaseIndex {99}};

  // All entries within the same (scene, phase) → unique LP found.
  const std::vector<std::pair<ScenarioUid, StageUid>> same_lp {
      {make_uid<Scenario>(100), StageUid {10}},
      {make_uid<Scenario>(101), StageUid {11}},
  };
  CHECK(slp.unique_owning_lp_of(same_lp).value_or(sentinel)
        == std::pair {first_scene_index(), first_phase_index()});

  // Cross-phase: scenario 100 is in scene 0, but stage 12 is in phase 1.
  // The first entry is (0, 0), the second is (0, 1) → mismatch.
  const std::vector<std::pair<ScenarioUid, StageUid>> cross_phase {
      {make_uid<Scenario>(100), StageUid {10}},
      {make_uid<Scenario>(100), StageUid {12}},
  };
  CHECK_FALSE(slp.unique_owning_lp_of(cross_phase).has_value());

  // Cross-scene: stages stay in phase 0, but scenarios move from scene 0
  // to scene 1.  First entry is (0, 0), second is (1, 0) → mismatch.
  const std::vector<std::pair<ScenarioUid, StageUid>> cross_scene {
      {make_uid<Scenario>(100), StageUid {10}},
      {make_uid<Scenario>(102), StageUid {10}},
  };
  CHECK_FALSE(slp.unique_owning_lp_of(cross_scene).has_value());

  // Empty span returns nullopt.
  const std::vector<std::pair<ScenarioUid, StageUid>> empty;
  CHECK_FALSE(slp.unique_owning_lp_of(empty).has_value());

  // Any unknown uid taints the entire span.
  const std::vector<std::pair<ScenarioUid, StageUid>> with_unknown {
      {make_uid<Scenario>(100), StageUid {10}},
      {make_uid<Scenario>(999), StageUid {10}},
  };
  CHECK_FALSE(slp.unique_owning_lp_of(with_unknown).has_value());
}
