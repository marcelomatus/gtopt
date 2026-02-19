/**
 * @file      test_converter_lp.cpp
 * @brief     Unit tests for ConverterLP and battery+converter LP formulation
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("SystemLP with battery and converter")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // Generator for charging the battery
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen_charge",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  // Demand representing converter discharge
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "dem_discharge",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .vmin = 0.0,
          .vmax = 100.0,
          .capacity = 100.0,
      },
  };

  const Array<Converter> converter_array = {
      {
          .uid = Uid {1},
          .name = "conv1",
          .battery = Uid {1},
          .generator = Uid {1},
          .demand = Uid {1},
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "BatteryConverterTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
      .converter_array = converter_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
