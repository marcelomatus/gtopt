// SPDX-License-Identifier: BSD-3-Clause
//
// Issue #510 Phase 1 — per-block fuel override via
// `Generator.fuel_per_block` (`OptTBUidSched`).
//
// Validates the LP cost path resolves the fuel cost per block
// (not per stage) when the new schedule is set: a generator with
// static fuel = gas but `fuel_per_block = [[gas, gas, diesel]]`
// must dispatch at the diesel price ONLY in block 2, and at the gas
// price in blocks 0-1.  Per-block accuracy implies the LP objective
// equals the sum-of-per-block fuel costs, not the static-fuel cost
// times the block count.

#include <string_view>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_multi_fuel
{

Simulation make_3block_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {0},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {0},
                  .first_block = 0,
                  .count_block = 3,
                  .chronological = false,
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

// Single bus, demand=50 MW constant, two fuels (gas @ $10, diesel
// @ $100), one generator burning 'gas' statically with heat_rate=2.
// When ``fuel_per_block`` is set (gas, gas, diesel), the LP cost path
// must charge the gas-fuel price in blocks 0-1 and the diesel-fuel
// price in block 2.  Per-block obj:
//   * blocks 0-1: 50 MW · 1 h · 2 MMBtu/MWh · $10/MMBtu = $1000
//   * block 2  : 50 MW · 1 h · 2 MMBtu/MWh · $100/MMBtu = $10 000
//   * total = $12 000  →  raw obj (default scale_objective=1000) = 12.0
System make_two_fuel_system(bool enable_per_block_override)
{
  System sys;
  sys.name = "multi_fuel";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 10.0,
      },
      {
          .uid = Uid {2},
          .name = "diesel",
          .price = 100.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 200.0,
          .gcost = 0.0,
          .fuel = Uid {1},  // static default = gas
          .heat_rate = 2.0,
          .capacity = 200.0,
      },
  };

  if (enable_per_block_override) {
    // 1 stage × 3 blocks matrix: blocks 0-1 → gas (1), block 2 → diesel (2)
    sys.generator_array[0].fuel_per_block = std::vector<std::vector<Uid>> {
        {
            Uid {1},
            Uid {1},
            Uid {2},
        },
    };
  }
  return sys;
}

}  // namespace test_multi_fuel

TEST_CASE("Generator.fuel_per_block default is unset")  // NOLINT
{
  const Generator g;
  CHECK_FALSE(g.fuel_per_block.has_value());
}

TEST_CASE("Generator.fuel_per_block JSON round-trip — matrix form")  // NOLINT
{
  constexpr std::string_view js = R"({
    "uid": 1,
    "name": "g1",
    "bus": 1,
    "pmax": 200.0,
    "fuel": 1,
    "heat_rate": 2.0,
    "fuel_per_block": [[1, 1, 2]]
  })";
  const auto g = daw::json::from_json<Generator>(js);
  REQUIRE(g.fuel_per_block.has_value());
  const auto& mat = std::get<std::vector<std::vector<Uid>>>(*g.fuel_per_block);
  REQUIRE(mat.size() == 1);
  REQUIRE(mat[0].size() == 3);
  CHECK(mat[0][0] == Uid {1});
  CHECK(mat[0][2] == Uid {2});
}

TEST_CASE(
    "Generator.fuel_per_block — baseline (unset) costs only the "
    "static fuel")  // NOLINT
{
  using namespace test_multi_fuel;
  System sys = make_two_fuel_system(/*enable_per_block_override=*/false);

  const auto simulation = make_3block_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 3 blocks × 50 MWh · 2 MMBtu/MWh · $10/MMBtu = $3 000 raw  → 3.0 scaled.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(3.0).epsilon(0.01));
}

TEST_CASE(
    "Generator.fuel_per_block — per-block override charges diesel "
    "in block 2")  // NOLINT
{
  using namespace test_multi_fuel;
  System sys = make_two_fuel_system(/*enable_per_block_override=*/true);

  const auto simulation = make_3block_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // blocks 0-1: 2 × $1 000 = $2 000.
  // block 2: 50 · 2 · 100 = $10 000.
  // Total = $12 000 raw → 12.0 scaled.  ~4× the static-only run.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE(
    "Fuel.max_offtake — per-block fuel correctly buckets gen × fuel "
    "consumption")  // NOLINT
{
  using namespace test_multi_fuel;
  // Same 3-block system with per-block override, plus a max_offtake
  // cap on diesel = 50 MMBtu (block 2 wants 100 MMBtu).  Gas is
  // uncapped.  With per-block bucketization the LP must curtail
  // block-2 dispatch from 50 MWh → 25 MWh so diesel consumption
  // hits 50 MMBtu exactly; unserved energy at $1 000/MWh fills
  // the gap.  If bucketization were broken (e.g. block-0/1 gas
  // consumption mis-attributed to diesel) the LP would be far
  // worse than this expected operating point.
  System sys = make_two_fuel_system(/*enable_per_block_override=*/true);
  sys.fuel_array[1].max_offtake = 50.0;  // diesel cap = 50 MMBtu/stage

  const auto simulation = make_3block_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Block 0-1 unchanged: 2 × $1 000 = $2 000 fuel cost.
  // Block 2 with cap: gen = 25 MWh (diesel 50 MMBtu, $5 000),
  // unserved = 25 MWh × $1 000 = $25 000.
  // Total raw = 2 000 + 5 000 + 25 000 = $32 000 → 32.0 scaled.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(32.0).epsilon(0.01));
}
