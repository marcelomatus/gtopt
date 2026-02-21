/**
 * @file      test_system_lp.cpp
 * @brief     Header of
 * @date      Sat Mar 29 22:09:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_system.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;
TEST_CASE("SystemLP 1")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "b1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {3}, .duration = 1},
              {.uid = Uid {4}, .duration = 2},
              {.uid = Uid {5}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 2},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  REQUIRE(simulation.scenario_array.size() == 1);
  REQUIRE(simulation.stage_array.size() == 2);
  REQUIRE(simulation.block_array.size() == 3);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.demand_array.size() == 1);
  REQUIRE(system.generator_array.size() == 1);
  REQUIRE(!system.line_array.empty() == false);

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  REQUIRE(lp_interface.get_numrows() == 3);

  REQUIRE(lp_interface.get_numrows() == 3);

  const SolverOptions lp_opts {};

  auto result = lp_interface.resolve(lp_opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  const auto sol = lp_interface.get_col_sol();
  REQUIRE(sol[0] == doctest::Approx(100));  // demand
  REQUIRE(sol[1] == doctest::Approx(100));  // generation

  const auto dual = lp_interface.get_row_dual();
  REQUIRE(dual[0] * system_lp.options().scale_objective()
          == doctest::Approx(50));
}

TEST_CASE("SystemLP - Primal Infeasible Case")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "b1",
          .bus = Uid {1},
          .capacity = 200.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();

  REQUIRE(!result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("non-optimal") != std::string::npos);
}

TEST_CASE("SystemLP - Timeout Scenario")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "b1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();

  const SolverOptions opts;
  lp_interface.set_time_limit(0.001);  // Set very small timeout

  auto result = lp_interface.resolve(opts);

  // May either timeout or solve quickly - both are acceptable outcomes
  if (!result) {
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("time") != std::string::npos);
  } else {
    CHECK(result.value() == 0);  // 0 = optimal
  }
}
