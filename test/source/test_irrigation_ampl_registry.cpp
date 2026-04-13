/**
 * @file      test_irrigation_ampl_registry.cpp
 * @brief     Tier 6 — AMPL element-attribute registry post-refactor tests
 * @date      2026-04-10
 * @copyright BSD-3-Clause
 *
 * After commit f02856d7 each LP element class registers its own
 * AMPL-visible variables in `SimulationLP::AmplVariableMap` (one cell
 * per (scene, phase)).  These tests guard the irrigation entry points
 * — `VolumeRightLP::add_to_lp` and `FlowRightLP::add_to_lp` — by
 * inspecting the registry directly via
 * `SystemContext::find_ampl_col`.  Any silent regression in the
 * decentralized registration would surface as a missing or duplicated
 * entry here, before it can poison downstream UserConstraint
 * resolution.
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

[[nodiscard]] Simulation make_single_block_simulation()
{
  return {
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
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

[[nodiscard]] Array<Bus> trivial_bus_array()
{
  return {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
}

[[nodiscard]] Array<Generator> trivial_generator_array()
{
  return {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };
}

[[nodiscard]] Array<Demand> trivial_demand_array()
{
  return {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
}

}  // namespace

TEST_CASE(  // NOLINT
    "Tier 6.1 - VolumeRightLP registers extraction/flow/saving entries")
{
  // VolumeRight with saving_rate set so all three attribute keys
  // (extraction, flow, saving) are present.  The "extraction" and
  // "flow" entries must point at the same column index — `flow` is a
  // convenience alias matching FlowRight's spelling.
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {42},
          .name = "vrt_ampl",
          .emax = 100.0,
          .eini = 100.0,
          .demand = 10.0,
          .fail_cost = 1000.0,
          .saving_rate = 50.0,
          .use_state_variable = false,
      },
  };

  const System system = {
      .name = "Tier6_1",
      .bus_array = trivial_bus_array(),
      .demand_array = trivial_demand_array(),
      .generator_array = trivial_generator_array(),
      .volume_right_array = vrs,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_single_block_simulation(), options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(scenarios.size() == 1);
  REQUIRE(stages.size() == 1);
  const auto& blocks = stages[0].blocks();
  REQUIRE(blocks.size() == 1);

  const auto sc_uid = scenarios[0].uid();
  const auto st_uid = stages[0].uid();
  const auto bk_uid = blocks[0].uid();
  constexpr auto cls = std::string_view {"volume_right"};

  const auto extraction_col =
      sc.find_ampl_col(cls, Uid {42}, "extraction", sc_uid, st_uid, bk_uid);
  const auto flow_col =
      sc.find_ampl_col(cls, Uid {42}, "flow", sc_uid, st_uid, bk_uid);
  const auto saving_col =
      sc.find_ampl_col(cls, Uid {42}, "saving", sc_uid, st_uid, bk_uid);

  REQUIRE(extraction_col.has_value());
  REQUIRE(flow_col.has_value());
  REQUIRE(saving_col.has_value());

  // `flow` is registered as an alias of `extraction` — same column.
  CHECK(*extraction_col == *flow_col);
  // `saving` must be a distinct column.
  CHECK(*saving_col != *extraction_col);
}

TEST_CASE(  // NOLINT
    "Tier 6.2 - FlowRightLP registers flow + conditional fail")
{
  // Two FlowRights:
  //  - `fr_with_fail` has discharge>0 and fail_cost>0 → both `flow`
  //    and `fail` must be registered.
  //  - `fr_no_fail`   has discharge>0 but no fail_cost → only `flow`
  //    is registered; `fail` lookup must return std::nullopt.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {7},
          .name = "fr_with_fail",
          .junction = Uid {1},
          .direction = -1,
          .discharge = 10.0,
          .fail_cost = 5000.0,
      },
      {
          .uid = Uid {8},
          .name = "fr_no_fail",
          .junction = Uid {1},
          .direction = -1,
          .discharge = 10.0,
      },
  };
  const Array<Junction> junctions = {
      {
          .uid = Uid {1},
          .name = "j1",
          .drain = true,
      },
  };

  const System system = {
      .name = "Tier6_2",
      .bus_array = trivial_bus_array(),
      .demand_array = trivial_demand_array(),
      .generator_array = trivial_generator_array(),
      .junction_array = junctions,
      .flow_right_array = frs,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_single_block_simulation(), options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& blocks = stages[0].blocks();
  const auto sc_uid = scenarios[0].uid();
  const auto st_uid = stages[0].uid();
  const auto bk_uid = blocks[0].uid();
  constexpr auto cls = std::string_view {"flow_right"};

  // fr_with_fail: both attrs present.
  const auto flow_yes =
      sc.find_ampl_col(cls, Uid {7}, "flow", sc_uid, st_uid, bk_uid);
  const auto fail_yes =
      sc.find_ampl_col(cls, Uid {7}, "fail", sc_uid, st_uid, bk_uid);
  REQUIRE(flow_yes.has_value());
  REQUIRE(fail_yes.has_value());
  CHECK(*flow_yes != *fail_yes);

  // fr_no_fail: flow present, fail absent.
  const auto flow_no =
      sc.find_ampl_col(cls, Uid {8}, "flow", sc_uid, st_uid, bk_uid);
  const auto fail_no =
      sc.find_ampl_col(cls, Uid {8}, "fail", sc_uid, st_uid, bk_uid);
  REQUIRE(flow_no.has_value());
  CHECK_FALSE(fail_no.has_value());
}

TEST_CASE(  // NOLINT
    "Tier 6.3 - Inactive irrigation elements do not register anything")
{
  // An inactive VolumeRight and an inactive FlowRight must short-circuit
  // out of `add_to_lp` (returning true with no LP work) — the
  // registry must not contain entries for them.
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {99},
          .name = "vrt_inactive",
          .active = OptActive {IntBool {0}},
          .emax = 100.0,
          .eini = 100.0,
      },
  };
  const Array<FlowRight> frs = {
      {
          .uid = Uid {100},
          .name = "fr_inactive",
          .active = OptActive {IntBool {0}},
          .junction = Uid {1},
          .direction = -1,
          .discharge = 10.0,
          .fail_cost = 5000.0,
      },
  };
  const Array<Junction> junctions = {
      {
          .uid = Uid {1},
          .name = "j1",
          .drain = true,
      },
  };

  const System system = {
      .name = "Tier6_3",
      .bus_array = trivial_bus_array(),
      .demand_array = trivial_demand_array(),
      .generator_array = trivial_generator_array(),
      .junction_array = junctions,
      .flow_right_array = frs,
      .volume_right_array = vrs,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_single_block_simulation(), options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& blocks = stages[0].blocks();
  const auto sc_uid = scenarios[0].uid();
  const auto st_uid = stages[0].uid();
  const auto bk_uid = blocks[0].uid();

  // No entries for either inactive element, on any registered attribute.
  CHECK_FALSE(
      sc.find_ampl_col(
            "volume_right", Uid {99}, "extraction", sc_uid, st_uid, bk_uid)
          .has_value());
  CHECK_FALSE(
      sc.find_ampl_col("volume_right", Uid {99}, "flow", sc_uid, st_uid, bk_uid)
          .has_value());
  CHECK_FALSE(
      sc.find_ampl_col("flow_right", Uid {100}, "flow", sc_uid, st_uid, bk_uid)
          .has_value());
  CHECK_FALSE(
      sc.find_ampl_col("flow_right", Uid {100}, "fail", sc_uid, st_uid, bk_uid)
          .has_value());
}

TEST_CASE(  // NOLINT
    "Tier 6.4 - Multiple stages register entries per (scenario, stage)")
{
  // The registry key is (class, uid, attr, scenario_uid, stage_uid).
  // Across stages within a single (scene, phase) cell, distinct stage
  // uids must yield distinct registered ColIndex values for the same
  // element + attribute combination.  This guards against accidental
  // key collisions where a second add_to_lp call could overwrite an
  // earlier stage's entry.
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {77},
          .name = "vrt_multi_stage",
          .emax = 100.0,
          .eini = 100.0,
          .demand = 5.0,
          .fail_cost = 1000.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
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
              {
                  .uid = Uid {20},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "Tier6_4",
      .bus_array = trivial_bus_array(),
      .demand_array = trivial_demand_array(),
      .generator_array = trivial_generator_array(),
      .volume_right_array = vrs,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  simulation_lp.set_need_ampl_variables(true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(stages.size() == 2);
  const auto sc_uid = scenarios[0].uid();
  const auto bk_uid = stages[0].blocks()[0].uid();

  const auto col_s1 = sc.find_ampl_col(
      "volume_right", Uid {77}, "extraction", sc_uid, stages[0].uid(), bk_uid);
  const auto col_s2 = sc.find_ampl_col(
      "volume_right", Uid {77}, "extraction", sc_uid, stages[1].uid(), bk_uid);
  REQUIRE(col_s1.has_value());
  REQUIRE(col_s2.has_value());
  // Distinct stages must own distinct extraction columns.
  CHECK(*col_s1 != *col_s2);
}
