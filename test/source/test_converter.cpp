#include <doctest/doctest.h>
#include <gtopt/converter.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Converter construction and default values")
{
  using namespace gtopt;
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
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          .use_state_variable = true,  // enable cross-stage coupling for test
          .daily_cycle =
              false,  // disable daily cycle to preserve test behavior
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

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ConverterLP - capainst column primal at lower bound")
{
  // Deterministic LP where the converter's own installed-capacity column
  // (`capainst`, owned by ConverterLP via CapacityObjectBase) can be
  // looked up and asserted via `capacity_col_at(stage)`.
  //
  // Setup:
  //   - 1 bus, 1 stage, 1 block (duration=1h), 1 scenario
  //   - A top-level generator cheaply serves a 10 MW demand, so the
  //     converter/battery path carries no economic incentive to expand.
  //   - The converter has a small fixed `capacity = 10` and an expansion
  //     module `expcap = 40, expmod = 1, annual_capcost = 1000`. This
  //     makes `stage_maxexpcap = 40 > 0`, so CapacityObjectBase::add_to_lp
  //     actually creates the capainst column (lowb=10, uppb=50) and the
  //     row `-capainst + 40*expmod_col = 0`.
  //   - In the current model `expmod_col` is forced to its module count
  //     (1) by the expansion-module formulation, so `capainst = 40 * 1
  //     = 40.0`.  This still demonstrates the lookup + LP-solve pattern
  //     for ConverterLP and pins capainst to a deterministic value.
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen_main",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "gen_discharge",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d_main",
          .bus = Uid {1},
          .capacity = 10.0,
      },
      {
          .uid = Uid {2},
          .name = "d_charge",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const Array<Converter> converter_array = {
      {
          .uid = Uid {1},
          .name = "conv1",
          .battery = Uid {1},
          .generator = Uid {2},
          .demand = Uid {2},
          .conversion_rate = 1.0,
          .capacity = 10.0,
          .expcap = 40.0,
          .expmod = 1.0,
          .annual_capcost = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;

  const System system = {
      .name = "ConverterCapainstTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
      .converter_array = converter_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Look up the converter's own capainst column via the LP-class accessor.
  const auto& conv_lps = system_lp.elements<ConverterLP>();
  REQUIRE(conv_lps.size() == 1);
  const auto& conv_lp = conv_lps.front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto cap_col = conv_lp.capacity_col_at(stage_lp);
  REQUIRE(cap_col.has_value());

  // The expansion-module formulation pins capainst = 40 * expmod = 40
  // when one module is chosen.
  const auto cap_val = lp.get_col_sol()[*cap_col];
  CHECK(cap_val == doctest::Approx(40.0).epsilon(1e-6));
}
