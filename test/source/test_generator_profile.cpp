// SPDX-License-Identifier: BSD-3-Clause
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

  const PlanningOptionsLP options;
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

  PlanningOptions opts;
  opts.scale_objective = 1000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Wind: 200 MW × 0.5 profile = 100 MW available.
  // Demand: 80 MW.  Constraint: spillover + generation = 100 MW.
  // Optimal: wind_gen dispatches 80 MW, spillover = 20 MW.
  // Verify via objective: wind_gen gcost=0 (free), backup unused (gcost=100),
  // spillover cost = 20 × 10 = 200.  Objective ≈ 200.
  // scale_objective defaults to 1000, so solver objective = 200 / 1000 = 0.2
  const auto obj = lp.get_obj_value_raw();
  CHECK(obj == doctest::Approx(0.2).epsilon(1e-3));
}

TEST_CASE("GeneratorProfileLP - per-block profile clamps dispatch col_sol")
{
  // With no spillover cost (scost unset) the profile fixes both upper and
  // lower generation bounds for each block at profile * capacity, so the
  // LP dispatch column is forced to the exact product regardless of demand
  // or gcost.  capacity = 100, profile = {0.5, 0.8} => dispatch = {50, 80}.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "solar_gen",
          .bus = Uid {1},
          .pmin = 0.0,
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  // Demand large enough to absorb both blocks' forced dispatch and then
  // some — backup generator covers the shortfall so the LP is feasible.
  const Array<Generator> backup_generator = {
      {
          .uid = Uid {2},
          .name = "backup_gen",
          .bus = Uid {1},
          .pmin = 0.0,
          .gcost = 1000.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 200.0,
      },
  };

  // 3-D [scenario][stage][block] profile with a single scenario/stage and
  // two blocks: {0.5, 0.8}.
  const STBRealFieldSched profile_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  0.5,
                  0.8,
              },
          },
      },
  };

  const Array<GeneratorProfile> generator_profile_array = {
      {
          .uid = Uid {1},
          .name = "solar_profile",
          .generator = Uid {1},
          .profile = profile_sched,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "ProfileColSolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = {generator_array.front(), backup_generator.front()},
      .generator_profile_array = generator_profile_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Locate the solar generator's dispatch columns for each block.
  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);
  const auto* solar_lp = static_cast<const GeneratorLP*>(nullptr);
  for (const auto& g : gen_lps) {
    if (g.generator().uid == Uid {1}) {
      solar_lp = &g;
      break;
    }
  }
  REQUIRE(solar_lp != nullptr);

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& gen_cols = solar_lp->generation_cols_at(scenario_lp, stage_lp);
  REQUIRE(gen_cols.size() == 2);

  const auto& blocks = stage_lp.blocks();
  REQUIRE(blocks.size() == 2);

  const auto it0 = gen_cols.find(blocks[0].uid());
  const auto it1 = gen_cols.find(blocks[1].uid());
  REQUIRE(it0 != gen_cols.end());
  REQUIRE(it1 != gen_cols.end());

  // profile * capacity = 0.5 * 100 = 50, 0.8 * 100 = 80.
  CHECK(lp.get_col_sol()[it0->second] == doctest::Approx(50.0).epsilon(1e-6));
  CHECK(lp.get_col_sol()[it1->second] == doctest::Approx(80.0).epsilon(1e-6));
}
