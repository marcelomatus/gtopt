#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/demand.hpp>
#include <gtopt/demand_profile.hpp>
#include <gtopt/json/json_demand.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Demand")
{
  using namespace gtopt;

  const Demand demand = {.uid = 1, .name = "demand_1"};

  CHECK(demand.uid == 1);
  CHECK(demand.name == "demand_1");
}

TEST_CASE("Demand with capacity")
{
  using namespace gtopt;

  Demand demand = {.uid = 1, .name = "demand_1", .capacity = 100.0};

  CHECK(demand.uid == 1);
  CHECK(demand.name == "demand_1");

  CHECK(std::get<double>(demand.capacity.value()) == 100.0);
}

TEST_CASE("DemandLP - basic demand with capacity")
{
  using namespace gtopt;
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
      .name = "DemandTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto sol = lp.get_col_sol();
  // First variable is demand load, should equal capacity
  CHECK(sol[0] == doctest::Approx(100.0));
}

TEST_CASE("DemandLP - multiple demands")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
      {.uid = Uid {2}, .name = "d2", .bus = Uid {1}, .capacity = 150.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "MultiDemandTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("DemandLP - demand with expansion")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,
          .expcap = 100.0,
          .expmod = 50.0,
          .annual_capcost = 1000.0,
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
      .name = "DemandExpansionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("DemandProfile construction and default values")
{
  using namespace gtopt;

  const DemandProfile demand_profile;

  CHECK(demand_profile.uid == Uid {unknown_uid});
  CHECK(demand_profile.name == Name {});
  CHECK_FALSE(demand_profile.active.has_value());

  CHECK(demand_profile.demand == SingleId {unknown_uid});
  CHECK_FALSE(demand_profile.scost.has_value());
}

TEST_CASE("DemandProfile attribute assignment")
{
  using namespace gtopt;

  DemandProfile demand_profile;

  demand_profile.uid = 10001;
  demand_profile.name = "TestDemandProfile";
  demand_profile.active = true;

  demand_profile.demand = Uid {3001};
  demand_profile.scost = 50.0;

  CHECK(demand_profile.uid == 10001);
  CHECK(demand_profile.name == "TestDemandProfile");
  CHECK(std::get<IntBool>(demand_profile.active.value()) == 1);

  CHECK(std::get<Uid>(demand_profile.demand) == Uid {3001});

  REQUIRE(demand_profile.scost.has_value());
  CHECK(*std::get_if<Real>(&demand_profile.scost.value())
        == doctest::Approx(50.0));
}

TEST_CASE("DemandProfile with inactive status")
{
  using namespace gtopt;

  DemandProfile demand_profile;

  demand_profile.uid = 10002;
  demand_profile.name = "InactiveDemandProfile";
  demand_profile.active = false;

  CHECK(demand_profile.uid == 10002);
  REQUIRE(demand_profile.active.has_value());
  CHECK(std::get<IntBool>(demand_profile.active.value()) == 0);
}

TEST_CASE("DemandProfileLP - basic demand profile with capacity")
{
  using namespace gtopt;

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
  };

  const Array<DemandProfile> demand_profile_array = {
      {
          .uid = Uid {1},
          .name = "dp1",
          .demand = Uid {1},
          .profile = 0.8,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "DemandProfileTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .demand_profile_array = demand_profile_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandProfileLP - multi-stage demand profile")
{
  using namespace gtopt;

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 150.0},
  };

  const Array<DemandProfile> demand_profile_array = {
      {
          .uid = Uid {1},
          .name = "dp1",
          .demand = Uid {1},
          .profile = 0.9,
          .scost = 100.0,
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
      .name = "MultiStageDemandProfileTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .demand_profile_array = demand_profile_array,
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests targeting uncovered branches in demand_lp.cpp
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DemandLP - emin with ecost (soft minimum energy constraint)")
{
  // Exercises: emin path (lines 59-88), emin_row lambda with ecost set,
  // lman cols (lines 150-162), emin_rows/lman_cols storage (lines 164-168)
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // emin = 80 MWh soft constraint with ecost = 200 $/MWh
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .emin = 80.0,
          .ecost = 200.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
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
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandEminEcostTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandLP - emin without ecost falls back to fcost")
{
  // Exercises: line 61-63 where stage_ecost is set from stage_fcost
  // when ecost is not provided but fcost (global) is available.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // emin set, ecost NOT set -> falls back to demand_fail_cost
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .emin = 50.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
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
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandEminFcostFallbackTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandLP - emin as hard constraint (no ecost, no fcost)")
{
  // Exercises: line 80 where emin_col is added as a fixed-bound column
  // (lowb = uppb = *stage_emin) when stage_ecost is nullopt.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // emin set, no ecost, no fcost -> hard constraint
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .emin = 60.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
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
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandEminHardTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // No ecost -> hard emin constraint
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandLP - lossfactor on demand")
{
  // Exercises: line 52 lossfactor, line 133 bus_brow with lossfactor,
  // and line 161 lman bus_brow with lossfactor.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // lossfactor = 0.05 means 5% additional losses
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lossfactor = 0.05,
          .emin = 40.0,
          .ecost = 300.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
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
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandLossFactorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandLP - per-demand fcost (not global)")
{
  // Exercises: demand with its own fcost field, triggering the fail cost
  // path (lines 114-127) independently of global demand_fail_cost option.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 50.0,  // less than demand to force some shedding
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 500.0,
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandPerFcostTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Per-demand fcost is set (overrides global demand_fail_cost)
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Generator can only serve 50 MW; demand is 100 MW -> 50 MW shed
  const auto sol = lp.get_col_sol();
  // load col should be 50 (gen capacity), fail col should be 50
  CHECK(sol[0] == doctest::Approx(50.0));
  CHECK(sol[1] == doctest::Approx(50.0));
}

TEST_CASE("DemandLP - add_to_output with fail and emin variables")
{
  // Exercises: add_to_output (lines 185-206) including fail_cols,
  // emin_cols, emin_rows, lman_cols, and balance_rows output paths.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // Demand with fail cost, emin, ecost, lossfactor, and expansion
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lossfactor = 0.02,
          .emin = 70.0,
          .ecost = 150.0,
          .capacity = 0.0,
          .expcap = 50.0,
          .expmod = 10.0,
          .annual_capcost = 500.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
              {
                  .uid = Uid {2},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_demand_output";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.demand_fail_cost = 1000.0;

  const System system = {
      .name = "DemandOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // write_out exercises DemandLP::add_to_output for all output fields
  system_lp.write_out();

  CHECK(std::filesystem::exists(tmpdir / "Demand"));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("DemandLP - emin with fcost and expansion across stages")
{
  // Exercises the combined path: emin + fcost fallback + capacity expansion
  // across two stages, ensuring emin_row, lman_cols, fail_cols, balance_rows,
  // and capacity_rows are all populated.
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .emin = 100.0,
          .capacity = 0.0,
          .expcap = 80.0,
          .expmod = 5.0,
          .annual_capcost = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 2.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 3.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
              {
                  .uid = Uid {2},
                  .first_block = 1,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DemandEminExpansionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 2000.0;
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("DemandLP - forced demand fixes load at lmax")
{
  // When forced=true the demand load is fixed at block_lmax (not elastic)
  // and no fail variable is created, even when demand_fail_cost is set.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
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
                  .duration = 2,
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
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  SUBCASE("forced demand is served at full capacity")
  {
    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .forced = true,
            .capacity = 100.0,
        },
    };

    const System system = {
        .name = "ForcedDemandTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);  // optimal

    // Load is fixed at 100 MW, generation = 100 MW.
    // cost = gcost × load × duration = 50 × 100 × (1+2) = 15000
    const auto obj = lp.get_obj_value_physical();
    CHECK(obj == doctest::Approx(15000.0));

    // No fail columns: with forced demand, only load + generation columns
    // per block (2 blocks × 2 cols = 4 cols), plus bus balance rows (2).
    const auto sol = lp.get_col_sol();
    // block 1: load=100, gen=100; block 2: load=100, gen=100
    CHECK(sol[0] == doctest::Approx(100.0));  // load block 1
    CHECK(sol[1] == doctest::Approx(100.0));  // gen block 1
    CHECK(sol[2] == doctest::Approx(100.0));  // load block 2
    CHECK(sol[3] == doctest::Approx(100.0));  // gen block 2
  }

  SUBCASE("forced demand causes infeasibility when supply is insufficient")
  {
    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .forced = true,
            .capacity = 1000.0,  // exceeds generator capacity (500 MW)
        },
    };

    const System system = {
        .name = "ForcedInfeasibleTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    // Infeasible: load fixed at 1000 MW but gen capacity is only 500 MW,
    // and no fail variable is created when forced=true.
    REQUIRE(!result.has_value());
  }

  SUBCASE("elastic demand (default) allows partial service with fail cost")
  {
    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 100.0,
        },
    };

    const System system = {
        .name = "ElasticDemandTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions opts;
    opts.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);  // optimal

    // With demand_fail_cost=1000 > gcost=50, optimizer serves full demand.
    // Same result as forced, but via incentive rather than constraint.
    const auto obj = lp.get_obj_value_physical();
    CHECK(obj == doctest::Approx(15000.0));
  }

  SUBCASE("elastic demand without fail cost serves zero load")
  {
    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 100.0,
        },
    };

    const System system = {
        .name = "ElasticNoFailCostTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    // No demand_fail_cost: optimizer has no incentive to serve demand
    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Zero cost: no generation needed since demand is not served
    const auto obj = lp.get_obj_value_physical();
    CHECK(obj == doctest::Approx(0.0));
  }
}
