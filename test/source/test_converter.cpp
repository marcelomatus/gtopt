#include <doctest/doctest.h>
#include <gtopt/converter.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("Converter construction and default values")
{
  const Converter converter;

  CHECK(converter.uid == Uid {unknown_uid});
  CHECK(converter.name == Name {});
  CHECK_FALSE(converter.active.has_value());

  CHECK(converter.battery == SingleId {unknown_uid});
  CHECK(converter.generator == SingleId {unknown_uid});
  CHECK(converter.demand == SingleId {unknown_uid});

  CHECK_FALSE(converter.conversion_rate.has_value());
  CHECK_FALSE(converter.capacity.has_value());
  CHECK_FALSE(converter.expcap.has_value());
  CHECK_FALSE(converter.expmod.has_value());
  CHECK_FALSE(converter.capmax.has_value());
  CHECK_FALSE(converter.annual_capcost.has_value());
  CHECK_FALSE(converter.annual_derating.has_value());
}

TEST_CASE("Converter attribute assignment")
{
  Converter converter;

  converter.uid = 4001;
  converter.name = "TestConverter";
  converter.active = true;

  converter.battery = Uid {1001};
  converter.generator = Uid {2001};
  converter.demand = Uid {3001};

  converter.conversion_rate = 0.95;
  converter.capacity = 500.0;
  converter.expcap = 100.0;
  converter.expmod = 25.0;
  converter.capmax = 1000.0;
  converter.annual_capcost = 20000.0;
  converter.annual_derating = 0.01;

  CHECK(converter.uid == 4001);
  CHECK(converter.name == "TestConverter");
  CHECK(std::get<IntBool>(converter.active.value()) == 1);

  CHECK(std::get<Uid>(converter.battery) == Uid {1001});
  CHECK(std::get<Uid>(converter.generator) == Uid {2001});
  CHECK(std::get<Uid>(converter.demand) == Uid {3001});

  REQUIRE(converter.conversion_rate.has_value());
  CHECK(*std::get_if<Real>(&converter.conversion_rate.value()) == 0.95);

  CHECK(*std::get_if<Real>(&converter.capacity.value()) == 500.0);
  CHECK(*std::get_if<Real>(&converter.expcap.value()) == 100.0);
  CHECK(*std::get_if<Real>(&converter.expmod.value()) == 25.0);
  CHECK(*std::get_if<Real>(&converter.capmax.value()) == 1000.0);
  CHECK(*std::get_if<Real>(&converter.annual_capcost.value()) == 20000.0);
  CHECK(*std::get_if<Real>(&converter.annual_derating.value()) == 0.01);
}

TEST_CASE("Converter with inactive status")
{
  Converter converter;

  converter.uid = 4002;
  converter.name = "InactiveConverter";
  converter.active = false;

  CHECK(converter.uid == 4002);
  CHECK(converter.name == "InactiveConverter");
  REQUIRE(converter.active.has_value());
  CHECK(std::get<IntBool>(converter.active.value()) == 0);
}

/**
 * @file      test_converter_lp.cpp
 * @brief     Unit tests for ConverterLP and battery+converter LP formulation
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 */


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
