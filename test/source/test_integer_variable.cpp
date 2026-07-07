/**
 * @file      test_integer_variable.cpp
 * @brief     Unit tests for IntegerVariable registry (Phase-0 choke-point)
 * @date      2026-05-31
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests the IntegerVariable abstraction and the SimulationLP registry
 * plumbing introduced in Phase 0 of the commitment-layout rollout
 * (docs/design/commitment-layout.md §11).  Phase 0 is a NO-OP for LP
 * outputs — these tests validate the new registry, not new LP behaviour.
 *
 * Tier T0 — IntegerVariable direct-object tests (no SystemLP required).
 * Tier T1 — SimulationLP registry round-trip via add_integer_variable.
 * Tier T15 — Column-order preservation: three registrations, all found.
 */

#include <algorithm>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/commitment.hpp>
#include <gtopt/integer_variable.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/uid.hpp>

using namespace gtopt;

namespace ivp
{
namespace
{

// ── Minimal 1-bus / 1-stage / 1-block / 1-scenario fixture ──────────────────

[[nodiscard]] auto make_minimal_simulation() -> Simulation
{
  Simulation sim;
  sim.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
  };
  sim.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  sim.scenario_array = {
      {
          .uid = Uid {0},
      },
  };
  return sim;
}

[[nodiscard]] auto make_minimal_options() -> PlanningOptionsLP
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.use_single_bus = true;
  popts.model_options.scale_objective = 1.0;
  return PlanningOptionsLP {popts};
}

/// Build a standard IntegerVariable::Key that resolves to (scene=0, phase=0).
[[nodiscard]] auto make_iv_key(LPClassName class_name,
                               Uid element_uid,
                               std::string_view col_name,
                               GroupUid group = unknown_group)
    -> IntegerVariable::Key
{
  IntegerVariable::Key k;
  k.class_name = class_name;
  k.element_uid = element_uid;
  k.col_name = col_name;
  k.scenario_uid = make_uid<Scenario>(0);
  k.stage_uid = make_uid<Stage>(0);
  k.group_uid = group;
  k.lp_key = LPKey {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  return k;
}

}  // namespace
}  // namespace ivp

// ── T0: IntegerVariable direct construction and accessors ───────────────────

TEST_CASE(
    "IntegerVariable direct construction: domain and scope round-trip")  // NOLINT
{
  const LPKey lp_key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };

  SUBCASE("Binary scope Block — single-block fan-out")
  {
    const std::vector<BlockUid> blocks {
        make_uid<Block>(7),
    };
    IntegerVariable iv {lp_key,
                        ColIndex {3},
                        IntegerDomain::Binary,
                        IntegerScope::Block,
                        GroupUid {7},
                        blocks};
    CHECK(iv.col() == ColIndex {3});
    CHECK(iv.domain() == IntegerDomain::Binary);
    CHECK(iv.scope() == IntegerScope::Block);
    CHECK(iv.group_uid() == GroupUid {7});
    REQUIRE(iv.blocks().has_value());
    CHECK(iv.blocks().value().size() == 1);
  }

  SUBCASE("Integer scope Group — multi-block fan-out")
  {
    const std::vector<BlockUid> grp_blocks {
        make_uid<Block>(1),
        make_uid<Block>(2),
    };
    IntegerVariable iv {lp_key,
                        ColIndex {5},
                        IntegerDomain::Integer,
                        IntegerScope::Group,
                        GroupUid {42},
                        grp_blocks};
    CHECK(iv.domain() == IntegerDomain::Integer);
    CHECK(iv.scope() == IntegerScope::Group);
    REQUIRE(iv.blocks().has_value());
    CHECK(iv.blocks().value().size() == 2);
  }

  SUBCASE("Relaxed scope Stage — stage fan-out")
  {
    const std::vector<BlockUid> blocks {
        make_uid<Block>(0),
    };
    IntegerVariable iv {lp_key,
                        ColIndex {9},
                        IntegerDomain::Relaxed,
                        IntegerScope::Stage,
                        unknown_group,
                        blocks};
    CHECK(iv.domain() == IntegerDomain::Relaxed);
    CHECK(iv.scope() == IntegerScope::Stage);
    REQUIRE(iv.blocks().has_value());
    CHECK(iv.group_uid() == unknown_group);
  }

  SUBCASE("Phase scope — blocks() returns nullopt")
  {
    IntegerVariable iv {lp_key,
                        ColIndex {11},
                        IntegerDomain::Binary,
                        IntegerScope::Phase,
                        unknown_group,
                        {}};
    CHECK(iv.scope() == IntegerScope::Phase);
    CHECK_FALSE(iv.blocks().has_value());
  }
}

// ── T0: blocks_or_throw ──────────────────────────────────────────────────────

TEST_CASE(
    "IntegerVariable::blocks_or_throw: Phase scope throws, others return span")  // NOLINT
{
  const LPKey lp_key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };

  SUBCASE("Block scope does not throw")
  {
    IntegerVariable iv {lp_key,
                        ColIndex {0},
                        IntegerDomain::Binary,
                        IntegerScope::Block,
                        GroupUid {1},
                        {
                            make_uid<Block>(1),
                        }};
    const auto sp = iv.blocks_or_throw();
    CHECK(sp.size() == 1);
  }

  SUBCASE("Stage scope does not throw — two blocks")
  {
    IntegerVariable iv {lp_key,
                        ColIndex {0},
                        IntegerDomain::Binary,
                        IntegerScope::Stage,
                        unknown_group,
                        {
                            make_uid<Block>(1),
                            make_uid<Block>(2),
                        }};
    CHECK(iv.blocks_or_throw().size() == 2);
  }

  SUBCASE("Phase scope throws std::logic_error")
  {
    IntegerVariable iv {lp_key,
                        ColIndex {0},
                        IntegerDomain::Binary,
                        IntegerScope::Phase,
                        unknown_group,
                        {}};
    CHECK_THROWS_AS([[maybe_unused]] auto sp = iv.blocks_or_throw(),
                    std::logic_error);
  }
}

// ── T0: Key factory guards ───────────────────────────────────────────────────

TEST_CASE(
    "IntegerVariable::key: unknown element_uid throws invalid_argument")  // NOLINT
{
  // Lightweight proxy objects satisfying the ScenarioLP / StageLP concept
  // used by the template IntegerVariable::key<ScenarioLP, StageLP>.
  struct FakeScenario
  {
    [[nodiscard]] static ScenarioUid uid() { return make_uid<Scenario>(0); }
    [[nodiscard]] static SceneIndex scene_index() { return SceneIndex {0}; }
  };
  struct FakeStage
  {
    [[nodiscard]] static StageUid uid() { return make_uid<Stage>(0); }
    [[nodiscard]] static PhaseIndex phase_index() { return PhaseIndex {0}; }
  };

  const FakeScenario scenario;
  const FakeStage stage;
  const LPClassName cls {"TestLP"};

  SUBCASE("unknown_uid triggers invalid_argument")
  {
    CHECK_THROWS_AS(
        [[maybe_unused]] auto k = IntegerVariable::key(scenario,
                                                       stage,
                                                       cls,
                                                       unknown_uid,
                                                       "status",
                                                       IntegerScope::Block,
                                                       GroupUid {0}),
        std::invalid_argument);
  }

  SUBCASE("concrete uid succeeds — fields round-trip")
  {
    const auto k = IntegerVariable::key(scenario,
                                        stage,
                                        cls,
                                        Uid {1},
                                        "status",
                                        IntegerScope::Block,
                                        GroupUid {0});
    CHECK(k.element_uid == Uid {1});
    CHECK(k.col_name == "status");
    CHECK(k.scenario_uid == make_uid<Scenario>(0));
    CHECK(k.stage_uid == make_uid<Stage>(0));
    CHECK(k.lp_key.scene_index == SceneIndex {0});
    CHECK(k.lp_key.phase_index == PhaseIndex {0});
  }

  SUBCASE("Stage / Phase scope forces group_uid to unknown_group")
  {
    const auto k_stage = IntegerVariable::key(scenario,
                                              stage,
                                              cls,
                                              Uid {2},
                                              "expmod",
                                              IntegerScope::Stage,
                                              GroupUid {99});
    CHECK(k_stage.group_uid == unknown_group);

    const auto k_phase = IntegerVariable::key(scenario,
                                              stage,
                                              cls,
                                              Uid {3},
                                              "expmod",
                                              IntegerScope::Phase,
                                              GroupUid {99});
    CHECK(k_phase.group_uid == unknown_group);
  }
}

// ── T1: SimulationLP registry round-trip ─────────────────────────────────────

TEST_CASE("SimulationLP add_integer_variable: registry round-trip")  // NOLINT
{
  const auto simulation = ivp::make_minimal_simulation();
  const auto options = ivp::make_minimal_options();
  SimulationLP sim_lp(simulation, options);

  const LPClassName cls {"CommitmentLP"};
  const ColIndex col_idx {ColIndex {42}};
  const std::vector<BlockUid> blocks {
      make_uid<Block>(0),
  };
  const auto key = ivp::make_iv_key(cls, Uid {1}, "status", GroupUid {0});

  SUBCASE("first registration returns matching IntegerVariable")
  {
    const auto& iv =
        sim_lp.add_integer_variable(key,
                                    col_idx,
                                    IntegerDomain::Binary,
                                    IntegerScope::Block,
                                    key.group_uid,
                                    std::span<const BlockUid>(blocks));
    CHECK(iv.col() == col_idx);
    CHECK(iv.domain() == IntegerDomain::Binary);
    CHECK(iv.scope() == IntegerScope::Block);
    REQUIRE(iv.blocks().has_value());
    CHECK(iv.blocks().value().size() == 1);
  }

  SUBCASE("idempotent re-registration (same col) is a no-op — no throw")
  {
    std::ignore =
        sim_lp.add_integer_variable(key,
                                    col_idx,
                                    IntegerDomain::Binary,
                                    IntegerScope::Block,
                                    key.group_uid,
                                    std::span<const BlockUid>(blocks));
    CHECK_NOTHROW(std::ignore = sim_lp.add_integer_variable(
                      key,
                      col_idx,
                      IntegerDomain::Binary,
                      IntegerScope::Block,
                      key.group_uid,
                      std::span<const BlockUid>(blocks)));
  }

  SUBCASE("re-registration with different ColIndex throws runtime_error")
  {
    std::ignore =
        sim_lp.add_integer_variable(key,
                                    col_idx,
                                    IntegerDomain::Binary,
                                    IntegerScope::Block,
                                    key.group_uid,
                                    std::span<const BlockUid>(blocks));
    const ColIndex other_col {ColIndex {99}};
    CHECK_THROWS_AS(std::ignore = sim_lp.add_integer_variable(
                        key,
                        other_col,
                        IntegerDomain::Binary,
                        IntegerScope::Block,
                        key.group_uid,
                        std::span<const BlockUid>(blocks)),
                    std::runtime_error);
  }

  SUBCASE("lookup via integer_variable() succeeds after registration")
  {
    std::ignore =
        sim_lp.add_integer_variable(key,
                                    col_idx,
                                    IntegerDomain::Binary,
                                    IntegerScope::Block,
                                    key.group_uid,
                                    std::span<const BlockUid>(blocks));
    const auto opt = sim_lp.integer_variable(key);
    REQUIRE(opt.has_value());
    CHECK(opt.value().get().col() == col_idx);
    CHECK(opt.value().get().domain() == IntegerDomain::Binary);
  }

  SUBCASE("lookup for unregistered key returns nullopt")
  {
    auto missing_key = key;
    missing_key.element_uid = Uid {999};
    const auto opt = sim_lp.integer_variable(missing_key);
    CHECK_FALSE(opt.has_value());
  }
}

// ── T1: domain value controls the IntegerDomain stored ───────────────────────

TEST_CASE(
    "SimulationLP add_integer_variable: three domain values round-trip")  // NOLINT
{
  const auto simulation = ivp::make_minimal_simulation();
  const auto options = ivp::make_minimal_options();
  SimulationLP sim_lp(simulation, options);

  const std::vector<BlockUid> blocks {
      make_uid<Block>(0),
  };
  const auto span = std::span<const BlockUid>(blocks);
  const LPClassName cls {"TestLP"};

  SUBCASE("Binary domain stored as Binary")
  {
    const auto& iv =
        sim_lp.add_integer_variable(ivp::make_iv_key(cls, Uid {10}, "bin_col"),
                                    ColIndex {0},
                                    IntegerDomain::Binary,
                                    IntegerScope::Block,
                                    GroupUid {0},
                                    span);
    CHECK(iv.domain() == IntegerDomain::Binary);
    CHECK(iv.domain() != IntegerDomain::Relaxed);
  }

  SUBCASE("Integer domain stored as Integer")
  {
    const auto& iv =
        sim_lp.add_integer_variable(ivp::make_iv_key(cls, Uid {11}, "int_col"),
                                    ColIndex {1},
                                    IntegerDomain::Integer,
                                    IntegerScope::Stage,
                                    unknown_group,
                                    span);
    CHECK(iv.domain() == IntegerDomain::Integer);
    CHECK(iv.domain() != IntegerDomain::Relaxed);
  }

  SUBCASE("Relaxed domain stored as Relaxed")
  {
    const auto& iv =
        sim_lp.add_integer_variable(ivp::make_iv_key(cls, Uid {12}, "rel_col"),
                                    ColIndex {2},
                                    IntegerDomain::Relaxed,
                                    IntegerScope::Block,
                                    GroupUid {0},
                                    span);
    CHECK(iv.domain() == IntegerDomain::Relaxed);
  }
}

// ── T1: integer_variables(scene, phase) enumerates registered entries
// ─────────

TEST_CASE("SimulationLP::integer_variables: empty then two entries")  // NOLINT
{
  const auto simulation = ivp::make_minimal_simulation();
  const auto options = ivp::make_minimal_options();
  SimulationLP sim_lp(simulation, options);

  const auto scene = SceneIndex {0};
  const auto phase = PhaseIndex {0};

  // Before registration, per-(scene, phase) map is empty.
  CHECK(sim_lp.integer_variables(scene, phase).empty());

  const std::vector<BlockUid> empty_blocks {};
  const auto span = std::span<const BlockUid>(empty_blocks);
  const LPClassName cls {"CapacityObjectLP"};

  std::ignore =
      sim_lp.add_integer_variable(ivp::make_iv_key(cls, Uid {1}, "expmod_a"),
                                  ColIndex {0},
                                  IntegerDomain::Binary,
                                  IntegerScope::Phase,
                                  unknown_group,
                                  span);
  std::ignore =
      sim_lp.add_integer_variable(ivp::make_iv_key(cls, Uid {2}, "expmod_b"),
                                  ColIndex {1},
                                  IntegerDomain::Integer,
                                  IntegerScope::Phase,
                                  unknown_group,
                                  span);

  const auto& map = sim_lp.integer_variables(scene, phase);
  CHECK(map.size() == 2);
}

// ── T15: Column-order preservation guard ─────────────────────────────────────

TEST_CASE(
    "IntegerVariable: three consecutive registrations all appear in map")  // NOLINT
{
  // Register three variables for distinct elements in a deterministic order
  // with ascending ColIndices (simulating add_integer_col output).
  // Verify all three ColIndices appear in the per-(scene,phase) map.

  const auto simulation = ivp::make_minimal_simulation();
  const auto options = ivp::make_minimal_options();
  SimulationLP sim_lp(simulation, options);

  const auto scene = SceneIndex {0};
  const auto phase = PhaseIndex {0};

  const std::vector<BlockUid> blks {
      make_uid<Block>(0),
  };
  const auto span = std::span<const BlockUid>(blks);
  const LPClassName cls {"CommitmentLP"};

  const ColIndex col_status {ColIndex {10}};
  const ColIndex col_startup {ColIndex {11}};
  const ColIndex col_shutdown {ColIndex {12}};

  std::ignore = sim_lp.add_integer_variable(
      ivp::make_iv_key(cls, Uid {1}, "status", GroupUid {1}),
      col_status,
      IntegerDomain::Binary,
      IntegerScope::Block,
      GroupUid {1},
      span);
  std::ignore = sim_lp.add_integer_variable(
      ivp::make_iv_key(cls, Uid {2}, "startup", GroupUid {2}),
      col_startup,
      IntegerDomain::Binary,
      IntegerScope::Block,
      GroupUid {2},
      span);
  std::ignore = sim_lp.add_integer_variable(
      ivp::make_iv_key(cls, Uid {3}, "shutdown", GroupUid {3}),
      col_shutdown,
      IntegerDomain::Binary,
      IntegerScope::Block,
      GroupUid {3},
      span);

  const auto& map = sim_lp.integer_variables(scene, phase);
  CHECK(map.size() == 3);

  // Collect all ColIndices present in the registry.
  std::vector<ColIndex> found_cols;
  found_cols.reserve(map.size());
  for (const auto& [k, iv] : map) {
    found_cols.push_back(iv.col());
    // All three are Binary domain.
    CHECK(iv.domain() == IntegerDomain::Binary);
  }

  // All three registered columns must appear (map ordering is by Key,
  // not insertion, so we use find rather than positional checks).
  CHECK(std::ranges::find(found_cols, col_status) != found_cols.end());
  CHECK(std::ranges::find(found_cols, col_startup) != found_cols.end());
  CHECK(std::ranges::find(found_cols, col_shutdown) != found_cols.end());
}

// ── T1-integration: SystemContext::add_integer_col side-effect ──────────────
//
// Closes the gap noted in the test agent's report: the registry tests
// above exercise `SimulationLP::add_integer_variable` directly but not
// the `SystemContext::add_integer_col` glue that producers actually
// call.  This test builds a small SystemLP with one CommitmentLP, then
// verifies the IntegerVariable registry has the expected `status` entry
// — proving the choke-point's side-effect end-to-end.

TEST_CASE(
    "SystemContext::add_integer_col: CommitmentLP populates registry")  // NOLINT
{
  using namespace gtopt;

  System system;
  system.name = "iv_choke_test";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
  };
  system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 1.0,
          .relax = true,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 2,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.use_single_bus = true;
  const PlanningOptionsLP options {popts};
  SimulationLP sim_lp {simulation, options};
  SystemLP sys_lp {system, sim_lp};

  // After SystemLP construction, CommitmentLP::add_to_lp has run for
  // each (scenario, stage) and called sc.add_integer_col for the `u`
  // status column.  The registry must contain a Block-scope Binary
  // entry per block (no commitment_period set → identity layout).
  const auto& map = sim_lp.integer_variables(SceneIndex {0}, PhaseIndex {0});
  CHECK_FALSE(map.empty());

  int status_count = 0;
  for (const auto& [key, iv] : map) {
    if (key.col_name == "status") {
      ++status_count;
      // Phase-0 identity layout: Block scope, Binary (relax was on).
      // With `relax = true` on the Commitment, the resolved domain is
      // Relaxed (per add_integer_col logic).
      CHECK(iv.scope() == IntegerScope::Block);
      CHECK(iv.domain() == IntegerDomain::Relaxed);
      REQUIRE(iv.blocks().has_value());
      CHECK(iv.blocks().value().size() == 1);
    }
  }
  // Two blocks in the stage, identity layout → 2 status entries.
  CHECK(status_count == 2);
}
