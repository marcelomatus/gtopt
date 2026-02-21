#include <doctest/doctest.h>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("Generator set_attrs functionality")
{
  Generator gen;
  GeneratorAttrs attrs;

  // Setup test values
  attrs.bus = Uid {42};
  attrs.pmin = 10.0;
  attrs.pmax = 100.0;

  // Test moving attrs to generator
  gen.set_attrs(attrs);

  CHECK(std::get<Uid>(gen.bus) == 42);
  CHECK(attrs.bus == SingleId {});  // Should be reset after exchange
  CHECK(gen.pmin.has_value());
  CHECK(gen.pmax.has_value());
  CHECK_FALSE(attrs.pmin.has_value());  // Should be moved
  CHECK_FALSE(attrs.pmax.has_value());  // Should be moved

  if (gen.pmin) {
    CHECK(std::get<Real>(gen.pmin.value()) == 10.0);
  }
  if (gen.pmax) {
    CHECK(std::get<Real>(gen.pmax.value()) == 100.0);
  }
}

TEST_CASE("Generator construction and attributes")
{
  Generator gen;

  // Default values
  CHECK(gen.uid == Uid {unknown_uid});
  CHECK(gen.name == Name {});
  CHECK_FALSE(gen.active.has_value());

  // Set some values
  gen.uid = Uid {1001};
  gen.name = "TestGenerator";
  gen.active = true;
  gen.bus = Uid {5};

  CHECK(gen.uid == 1001);
  CHECK(gen.name == "TestGenerator");
  CHECK(std::get<IntBool>(gen.active.value()) == 1);
  CHECK(std::get<Uid>(gen.bus) == 5);
}

TEST_CASE("GeneratorLP - basic generation")
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

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "GenTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto sol = lp.get_col_sol();
  CHECK(sol[1] == doctest::Approx(100.0));  // generation matches demand
}

TEST_CASE("GeneratorLP - multiple generators merit order")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "cheap",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "expensive",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 80.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "MeritOrderTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("GeneratorLP - generator with expansion")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 0.0,
          .expcap = 200.0,
          .expmod = 100.0,
          .annual_capcost = 5000.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
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

  const System system = {
      .name = "GenExpansionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("GeneratorLP - generator with pmin/pmax constraints")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 20.0,
          .pmax = 150.0,
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "PMinMaxTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}
