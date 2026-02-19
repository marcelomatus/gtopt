/**
 * @file      test_reserve_zone_lp.cpp
 * @brief     Unit tests for ReserveZoneLP and ReserveProvisionLP
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("ReserveZoneLP - basic reserve zone with up requirement")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,
          .urcost = 1000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 100.0,
          .ur_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "ReserveTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveZoneLP - up and down reserve requirements")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 400.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 40.0,
          .drreq = 30.0,
          .urcost = 500.0,
          .drcost = 400.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 100.0,
          .drmax = 80.0,
          .ur_provision_factor = 1.0,
          .dr_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "ReserveUpDownTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveZoneLP - multi-stage reserve with provision")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 40.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 150.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 60.0,
          .urcost = 2000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 150.0,
          .ur_provision_factor = 1.0,
          .urcost = 5.0,
      },
      {
          .uid = Uid {2},
          .name = "rp2",
          .generator = Uid {2},
          .reserve_zones = "1",
          .urmax = 100.0,
          .ur_provision_factor = 1.0,
          .urcost = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "MultiStageReserveTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveProvisionLP - capacity factor constraint")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 40.0,
          .urcost = 1000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 100.0,
          .ur_capacity_factor = 0.5,
          .ur_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "CapFactorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveZoneLP - reserve zone without requirement (no-op)")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

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

  // Reserve zone without any requirements set
  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz_empty",
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmptyReserveTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveZoneLP - down-reserve provision (dprov)")
{
  // Exercises the dprov branch in ReserveProvisionLP::add_to_lp which was
  // previously uncovered.
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .drreq = 40.0,  // down-reserve requirement
          .drcost = 500.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .drmax = 80.0,
          .dr_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "DownReserveTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReserveZoneLP - both up and down reserve with capacity factor")
{
  // Exercises add_provision with use_capacity=true for both up and down.
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 400.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 30.0,
          .drreq = 20.0,
          .urcost = 800.0,
          .drcost = 600.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .ur_capacity_factor = 0.3,
          .dr_capacity_factor = 0.2,
          .ur_provision_factor = 1.0,
          .dr_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "BothReservesCapFactor",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
