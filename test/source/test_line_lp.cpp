/**
 * @file      test_line_lp.cpp
 * @brief     Unit tests for LineLP (transmission line LP formulation)
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("SystemLP with transmission line - two bus system")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .capacity = 100.0},
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.1,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  System system = {
      .name = "TwoBusSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  system.setup_reference_bus(options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  CHECK(lp_interface.get_numrows() > 0);
  CHECK(lp_interface.get_numcols() > 0);

  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with line - single bus mode")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .capacity = 100.0},
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 150.0,
          .tmax_ab = 150.0,
          .capacity = 150.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.use_single_bus = true;

  System system = {
      .name = "SingleBusMode",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);

  system.setup_reference_bus(options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with loop line is skipped")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  // Loop line: bus_a == bus_b
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "loop",
          .bus_a = Uid {1},
          .bus_b = Uid {1},
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "LoopTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
