// SPDX-License-Identifier: BSD-3-Clause
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/generator_profile.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

TEST_CASE("GeneratorProfile default construction")  // NOLINT
{
  using namespace gtopt;
  const GeneratorProfile gp;

  CHECK(gp.uid == Uid {unknown_uid});
  CHECK(gp.name.empty());
  CHECK_FALSE(gp.active.has_value());
  CHECK(gp.generator == SingleId {unknown_uid});
  CHECK_FALSE(gp.scost.has_value());
}

TEST_CASE("GeneratorProfile attribute assignment")  // NOLINT
{
  GeneratorProfile gp;

  gp.uid = Uid {1};
  gp.name = "solar_profile";
  gp.active = true;
  gp.generator = Uid {42};
  gp.scost = 2.5;

  CHECK(gp.uid == Uid {1});
  CHECK(gp.name == "solar_profile");
  REQUIRE(gp.active.has_value());
  CHECK(std::get<IntBool>(gp.active.value_or(Active {False})) == 1);
  CHECK(std::get<Uid>(gp.generator) == Uid {42});

  REQUIRE(gp.scost.has_value());
  CHECK(std::get<Real>(gp.scost.value_or(Real {0.0})) == doctest::Approx(2.5));
}

TEST_CASE("GeneratorProfile scalar profile")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {2};
  gp.name = "wind_profile";
  gp.generator = Uid {10};

  // Scalar profile (same value for all scenario/time/block combinations)
  gp.profile = 0.75;

  REQUIRE(std::get_if<Real>(&gp.profile) != nullptr);
  CHECK(*std::get_if<Real>(&gp.profile) == doctest::Approx(0.75));
}

TEST_CASE("GeneratorProfile 2D vector profile")  // NOLINT
{
  // STBRealFieldSched is FieldSched3<double> = variant<double,
  // vector<vector<vector<double>>>, string>
  GeneratorProfile gp;
  gp.uid = Uid {3};
  gp.name = "pv_profile_24h";
  gp.generator = Uid {5};

  // Two stages, one block each – scalar profile per stage-block
  // Use the FileSched (string) alternative to name an external file
  gp.profile = std::string {"solar_pv_profile"};

  REQUIRE(std::get_if<std::string>(&gp.profile) != nullptr);
  CHECK(*std::get_if<std::string>(&gp.profile) == "solar_pv_profile");
}

TEST_CASE("GeneratorProfile with name-based generator reference")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {4};
  gp.name = "hydro_profile";
  gp.generator = Name {"hydro_plant_1"};

  CHECK(std::holds_alternative<Name>(gp.generator));
  CHECK(std::get<Name>(gp.generator) == "hydro_plant_1");
}

TEST_CASE("GeneratorProfileLP - basic generator profile with capacity")
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

  const Array<GeneratorProfile> generator_profile_array = {
      {
          .uid = Uid {1},
          .name = "gp1",
          .generator = Uid {1},
          .profile = 0.7,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "GenProfileTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .generator_profile_array = generator_profile_array,
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

TEST_CASE("GeneratorProfileLP - multi-stage generator profile")
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
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
  };

  const Array<GeneratorProfile> generator_profile_array = {
      {
          .uid = Uid {1},
          .name = "gp1",
          .generator = Uid {1},
          .profile = 0.6,
          .scost = 50.0,
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

  const System system = {
      .name = "MultiStageGenProfileTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .generator_profile_array = generator_profile_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("GeneratorProfileLP - generator profile with spillover cost")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "wind_gen",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "backup_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const Array<GeneratorProfile> generator_profile_array = {
      {
          .uid = Uid {1},
          .name = "wind_profile",
          .generator = Uid {1},
          .profile = 0.5,
          .scost = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SpilloverCostTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .generator_profile_array = generator_profile_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("GeneratorProfile default construction")  // NOLINT
{
  const GeneratorProfile gp;

  CHECK(gp.uid == Uid {unknown_uid});
  CHECK(gp.name.empty());
  CHECK_FALSE(gp.active.has_value());
  CHECK(gp.generator == SingleId {unknown_uid});
  CHECK_FALSE(gp.scost.has_value());
}

TEST_CASE("GeneratorProfile attribute assignment")  // NOLINT
{
  GeneratorProfile gp;

  gp.uid = Uid {1};
  gp.name = "solar_profile";
  gp.active = true;
  gp.generator = Uid {42};
  gp.scost = 2.5;

  CHECK(gp.uid == Uid {1});
  CHECK(gp.name == "solar_profile");
  REQUIRE(gp.active.has_value());
  CHECK(std::get<IntBool>(gp.active.value_or(Active {False})) == 1);
  CHECK(std::get<Uid>(gp.generator) == Uid {42});

  REQUIRE(gp.scost.has_value());
  CHECK(std::get<Real>(gp.scost.value_or(Real {0.0})) == doctest::Approx(2.5));
}

TEST_CASE("GeneratorProfile scalar profile")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {2};
  gp.name = "wind_profile";
  gp.generator = Uid {10};

  // Scalar profile (same value for all scenario/time/block combinations)
  gp.profile = 0.75;

  REQUIRE(std::get_if<Real>(&gp.profile) != nullptr);
  CHECK(*std::get_if<Real>(&gp.profile) == doctest::Approx(0.75));
}

TEST_CASE("GeneratorProfile 2D vector profile")  // NOLINT
{
  // STBRealFieldSched is FieldSched3<double> = variant<double,
  // vector<vector<vector<double>>>, string>
  GeneratorProfile gp;
  gp.uid = Uid {3};
  gp.name = "pv_profile_24h";
  gp.generator = Uid {5};

  // Two stages, one block each – scalar profile per stage-block
  // Use the FileSched (string) alternative to name an external file
  gp.profile = std::string {"solar_pv_profile"};

  REQUIRE(std::get_if<std::string>(&gp.profile) != nullptr);
  CHECK(*std::get_if<std::string>(&gp.profile) == "solar_pv_profile");
}

TEST_CASE("GeneratorProfile with name-based generator reference")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {4};
  gp.name = "hydro_profile";
  gp.generator = Name {"hydro_plant_1"};

  CHECK(std::holds_alternative<Name>(gp.generator));
  CHECK(std::get<Name>(gp.generator) == "hydro_plant_1");
}
