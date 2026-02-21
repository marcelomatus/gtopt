/**
 * @file      test_flat_helper_uids.cpp
 * @brief     Unit tests for FlatHelper UID generation methods (stb_uids,
 *            st_uids, t_uids)
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/flat_helper.hpp>
#include <gtopt/simulation_lp.hpp>

using namespace gtopt;

TEST_CASE("FlatHelper - stb_uids basic")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {10}}, {.uid = Uid {20}}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.stb_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  REQUIRE(uids.stage_uids.size() == 2);
  REQUIRE(uids.block_uids.size() == 2);

  // Each block should have the same scenario and stage
  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.scenario_uids[1] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {1});
  CHECK(uids.block_uids[0] == Uid {10});
  CHECK(uids.block_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - stb_uids multi-stage")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 2},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.stb_uids();

  REQUIRE(uids.scenario_uids.size() == 3);
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.block_uids[0] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {20});
  CHECK(uids.block_uids[1] == Uid {2});
  CHECK(uids.stage_uids[2] == Uid {20});
  CHECK(uids.block_uids[2] == Uid {3});
}

TEST_CASE("FlatHelper - st_uids basic")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}, {.uid = Uid {2}}},
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.st_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  REQUIRE(uids.stage_uids.size() == 2);

  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.scenario_uids[1] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.stage_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - st_uids multi-scenario")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}},
      .stage_array = {{.uid = Uid {10}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}, {.uid = Uid {1}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.st_uids();

  REQUIRE(uids.scenario_uids.size() == 2);
  CHECK(uids.scenario_uids[0] == Uid {0});
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.scenario_uids[1] == Uid {1});
  CHECK(uids.stage_uids[1] == Uid {10});
}

TEST_CASE("FlatHelper - t_uids basic")
{
  const OptionsLP options;
  const Simulation psimulation = {
      .block_array = {{.uid = Uid {1}}, {.uid = Uid {2}}},
      .stage_array =
          {
              {.uid = Uid {10}, .first_block = 0, .count_block = 1},
              {.uid = Uid {20}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto uids = helper.t_uids();

  REQUIRE(uids.stage_uids.size() == 2);
  CHECK(uids.stage_uids[0] == Uid {10});
  CHECK(uids.stage_uids[1] == Uid {20});
}

TEST_CASE("FlatHelper - uids with empty simulation")
{
  const OptionsLP options;
  const Simulation psimulation;
  const SimulationLP simulation {psimulation, options};
  const FlatHelper helper(simulation);

  auto stb = helper.stb_uids();
  CHECK(stb.scenario_uids.empty());
  CHECK(stb.stage_uids.empty());
  CHECK(stb.block_uids.empty());

  auto st = helper.st_uids();
  CHECK(st.scenario_uids.empty());
  CHECK(st.stage_uids.empty());

  auto t = helper.t_uids();
  CHECK(t.stage_uids.empty());
}
