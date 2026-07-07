// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_block_state_multiblock.cpp
 * @brief     Multi-block / multi-stage coverage for the `block_state`
 *            DecisionVariable + `prev(...)` lag (AMPL reservoir).
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * The equivalence fixture (test_user_storage_equivalence.cpp) is 1 block /
 * stage and 1 stage / phase, so it never exercises the WITHIN-stage lag
 * (`prev()` at block b>0 → block b-1) nor the same-phase-later-stage incoming
 * (incoming of stage k+1 == end column of stage k).  These monolithic tests
 * cover both directly by reading the per-block volume solution, plus the
 * non-chronological hard-error guard.
 */

#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace block_state_mb_test
{
namespace
{

/// Solve a monolithic planning JSON and return the per-block volume solution
/// of the `block_state` DecisionVariable with the given uid (default 101),
/// ordered by (stage, block).  Used to assert the storage recurrence chains
/// correctly across blocks and stages.
[[nodiscard]] auto solve_and_read_vol(std::string_view json,
                                      Uid which = Uid {101})
    -> std::vector<double>
{
  Planning base;
  base.merge(parse_planning_json(json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  auto&& sys = planning_lp.system(first_scene_index(), first_phase_index());
  const auto sol = sys.linear_interface().get_col_sol();

  std::vector<double> out;
  for (const auto& dv : sys.elements<DecisionVariableLP>()) {
    if (dv.uid() != which) {
      continue;
    }
    // value_cols_holder: (scenario, stage) → (block → col), iterated in key
    // (stage uid, then block uid) order.
    for (const auto& [st_key, bmap] : dv.value_cols_holder()) {
      for (const auto& [buid, col] : bmap) {
        out.push_back(sol[col]);
      }
    }
  }
  return out;
}

}  // namespace
}  // namespace block_state_mb_test

TEST_CASE(
    "block_state prev() interior-block lag + initial_value chains a 3-block "
    "stage")
{
  using namespace block_state_mb_test;

  // 1 chronological stage, 3 blocks.  vol[0] = initial_value(50) + 10 = 60;
  // vol[1] = vol[0] + 10 = 70; vol[2] = 80 — driven by the within-stage lag
  // `prev(vol)` at blocks 1,2 and the incoming (initial_value) at block 0.
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1}, {"uid":2,"duration":1}, {"uid":3,"duration":1} ],
      "stage_array": [ {"uid":1,"first_block":0,"count_block":3,"active":1,"chronological":true} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ]
    },
    "system": {
      "name": "block_state_interior",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[5,5,5]]} ],
      "decision_variable_array": [ {"uid":101,"name":"vol","scope":"block",
        "lower_bound":0,"upper_bound":1000,"link":true,"block_state":true,"initial_value":50} ],
      "user_constraint_array": [ {"uid":201,"name":"bal",
        "expression":"decision_variable('vol').value - prev(decision_variable('vol').value) = 10"} ]
    }
  })json";

  const auto vol = solve_and_read_vol(json);
  REQUIRE(vol.size() == 3);
  CHECK(vol[0] == doctest::Approx(60.0));
  CHECK(vol[1] == doctest::Approx(70.0));
  CHECK(vol[2] == doctest::Approx(80.0));
}

TEST_CASE(
    "block_state prev() same-phase-later-stage incoming chains across stages")
{
  using namespace block_state_mb_test;

  // 1 phase, 2 stages (1 block each).  Stage 2's incoming column IS stage 1's
  // end-of-stage column (same LP), so vol chains: vol[s1]=60, vol[s2]=70.
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1}, {"uid":2,"duration":1} ],
      "stage_array": [
        {"uid":1,"first_block":0,"count_block":1,"active":1,"chronological":true},
        {"uid":2,"first_block":1,"count_block":1,"active":1,"chronological":true} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ],
      "phase_array": [ {"uid":1,"first_stage":0,"count_stage":2} ]
    },
    "system": {
      "name": "block_state_two_stage",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[5],[5]]} ],
      "decision_variable_array": [ {"uid":101,"name":"vol","scope":"block",
        "lower_bound":0,"upper_bound":1000,"link":true,"block_state":true,"initial_value":50} ],
      "user_constraint_array": [ {"uid":201,"name":"bal",
        "expression":"decision_variable('vol').value - prev(decision_variable('vol').value) = 10"} ]
    }
  })json";

  const auto vol = solve_and_read_vol(json);
  REQUIRE(vol.size() == 2);
  CHECK(vol[0] == doctest::Approx(60.0));
  CHECK(vol[1] == doctest::Approx(70.0));
}

TEST_CASE(
    "block_state on a multi-block non-chronological stage is a hard error")
{
  using namespace block_state_mb_test;

  // Same 3-block stage but WITHOUT chronological=true.  The within-stage
  // prev() lag has no well-defined previous block, so the build must throw
  // rather than silently drop the lag term (which would corrupt the balance).
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1}, {"uid":2,"duration":1}, {"uid":3,"duration":1} ],
      "stage_array": [ {"uid":1,"first_block":0,"count_block":3,"active":1} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ]
    },
    "system": {
      "name": "block_state_nonchrono",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[5,5,5]]} ],
      "decision_variable_array": [ {"uid":101,"name":"vol","scope":"block",
        "lower_bound":0,"upper_bound":1000,"link":true,"block_state":true,"initial_value":50} ],
      "user_constraint_array": [ {"uid":201,"name":"bal",
        "expression":"decision_variable('vol').value - prev(decision_variable('vol').value) = 10"} ]
    }
  })json";

  auto planning = parse_planning_json(json);
  CHECK_THROWS_AS(PlanningLP(std::move(planning)),  // NOLINT
                  std::runtime_error);
}

TEST_CASE("block_state: `* block.duration` weights the per-block balance by Δt")
{
  using namespace block_state_mb_test;

  // 1 chronological stage, 3 blocks with NON-UNIT durations {1, 2, 4}.  The
  // balance `vol - prev(vol) + generation * block.duration = 0` draws
  // generation·Δt of volume each block.  Constant demand 10 MW forces
  // generation = 10 every block (cheapest, capacity 100).  So:
  //   vol[0] = 1000 - 10*1 = 990;  vol[1] = 990 - 10*2 = 970;
  //   vol[2] = 970 - 10*4 = 930.
  // Without the `* block.duration` factor this would be {990, 980, 970} — the
  // test pins the Δt weighting.
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1}, {"uid":2,"duration":2}, {"uid":3,"duration":4} ],
      "stage_array": [ {"uid":1,"first_block":0,"count_block":3,"active":1,"chronological":true} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ]
    },
    "system": {
      "name": "block_state_duration",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[10,10,10]]} ],
      "decision_variable_array": [ {"uid":101,"name":"vol","scope":"block",
        "lower_bound":0,"upper_bound":10000,"link":true,"block_state":true,"initial_value":1000} ],
      "user_constraint_array": [ {"uid":201,"name":"bal",
        "expression":"decision_variable('vol').value - prev(decision_variable('vol').value) + generator('g1').generation * block.duration = 0"} ]
    }
  })json";

  const auto vol = solve_and_read_vol(json);
  REQUIRE(vol.size() == 3);
  CHECK(vol[0] == doctest::Approx(990.0));
  CHECK(vol[1] == doctest::Approx(970.0));
  CHECK(vol[2] == doctest::Approx(930.0));
}

TEST_CASE(
    "block_state: initial_value unset defaults the incoming to lower_bound")
{
  using namespace block_state_mb_test;

  // 1 stage, 1 block, NO initial_value, lower_bound = 10.  The first-stage
  // incoming column is fixed to the lower bound (10), not 0, so the balance
  // `vol - prev(vol) = 5` gives vol[0] = 10 + 5 = 15.
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1} ],
      "stage_array": [ {"uid":1,"first_block":0,"count_block":1,"active":1,"chronological":true} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ]
    },
    "system": {
      "name": "block_state_initdefault",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[5]]} ],
      "decision_variable_array": [ {"uid":101,"name":"vol","scope":"block",
        "lower_bound":10,"upper_bound":1000,"link":true,"block_state":true} ],
      "user_constraint_array": [ {"uid":201,"name":"bal",
        "expression":"decision_variable('vol').value - prev(decision_variable('vol').value) = 5"} ]
    }
  })json";

  const auto vol = solve_and_read_vol(json);
  REQUIRE(vol.size() == 1);
  CHECK(vol[0] == doctest::Approx(15.0));
}

TEST_CASE("block_state: two independent block_state DVs do not cross-wire")
{
  using namespace block_state_mb_test;

  // Two block_state DVs in one 3-block chronological stage, each with its own
  // balance and initial value.  vol1: 60,70,80 (RHS 10, init 50); vol2:
  // 120,140,160 (RHS 20, init 100).  Reading each by uid confirms the
  // `value_in` / state registration keys per-DV without aliasing.
  static constexpr std::string_view json = R"json({
    "options": { "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": { "use_single_bus": true, "scale_objective": 1, "demand_fail_cost": 1000 } },
    "simulation": {
      "block_array": [ {"uid":1,"duration":1}, {"uid":2,"duration":1}, {"uid":3,"duration":1} ],
      "stage_array": [ {"uid":1,"first_block":0,"count_block":3,"active":1,"chronological":true} ],
      "scenario_array": [ {"uid":1,"probability_factor":1} ]
    },
    "system": {
      "name": "block_state_two_dv",
      "bus_array": [ {"uid":1,"name":"b1"} ],
      "generator_array": [ {"uid":1,"name":"g1","bus":1,"gcost":1,"capacity":100} ],
      "demand_array": [ {"uid":1,"name":"d1","bus":1,"lmax":[[5,5,5]]} ],
      "decision_variable_array": [
        {"uid":101,"name":"vol1","scope":"block","lower_bound":0,"upper_bound":1000,"link":true,"block_state":true,"initial_value":50},
        {"uid":102,"name":"vol2","scope":"block","lower_bound":0,"upper_bound":1000,"link":true,"block_state":true,"initial_value":100} ],
      "user_constraint_array": [
        {"uid":201,"name":"bal1","expression":"decision_variable('vol1').value - prev(decision_variable('vol1').value) = 10"},
        {"uid":202,"name":"bal2","expression":"decision_variable('vol2').value - prev(decision_variable('vol2').value) = 20"} ]
    }
  })json";

  const auto vol1 = solve_and_read_vol(json, Uid {101});
  const auto vol2 = solve_and_read_vol(json, Uid {102});
  REQUIRE(vol1.size() == 3);
  REQUIRE(vol2.size() == 3);
  CHECK(vol1[0] == doctest::Approx(60.0));
  CHECK(vol1[1] == doctest::Approx(70.0));
  CHECK(vol1[2] == doctest::Approx(80.0));
  CHECK(vol2[0] == doctest::Approx(120.0));
  CHECK(vol2[1] == doctest::Approx(140.0));
  CHECK(vol2[2] == doctest::Approx(160.0));
}
