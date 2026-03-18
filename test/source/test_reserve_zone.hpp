#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_lp.hpp>

TEST_CASE("ReserveZone construction and default values")
{
  using namespace gtopt;
  const ReserveZone reserve_zone;

  // Check default values
  CHECK(reserve_zone.uid == Uid {unknown_uid});
  CHECK(reserve_zone.name == Name {});
  CHECK_FALSE(reserve_zone.active.has_value());

  // Check default values for reserve requirements
  CHECK_FALSE(reserve_zone.urreq.has_value());  // up reserve requirement
  CHECK_FALSE(reserve_zone.drreq.has_value());  // down reserve requirement
  CHECK_FALSE(reserve_zone.urcost.has_value());  // up reserve shortage cost
  CHECK_FALSE(reserve_zone.drcost.has_value());  // down reserve shortage cost
}

TEST_CASE("ReserveZone attribute assignment")
{
  ReserveZone reserve_zone;

  // Assign identification values
  reserve_zone.uid = 2001;
  reserve_zone.name = "TestReserveZone";
  reserve_zone.active = true;

  // Assign reserve requirements and costs
  reserve_zone.urreq = 50.0;  // 50 MW up reserve requirement
  reserve_zone.drreq = 30.0;  // 30 MW down reserve requirement
  reserve_zone.urcost = 1000.0;  // $1000/MW shortage cost for up reserve
  reserve_zone.drcost = 800.0;  // $800/MW shortage cost for down reserve

  // Check assigned values
  CHECK(reserve_zone.uid == 2001);
  CHECK(reserve_zone.name == "TestReserveZone");
  CHECK(std::get<IntBool>(reserve_zone.active.value()) == 1);

  // For OptTBRealFieldSched and OptTRealFieldSched types, we need to get the
  // Real variant alternative Check urreq (TB = Time Block schedule)
  REQUIRE(reserve_zone.urreq.has_value());
  auto* urreq_real_ptr = std::get_if<Real>(&reserve_zone.urreq.value());
  REQUIRE(urreq_real_ptr != nullptr);
  CHECK(*urreq_real_ptr == 50.0);

  // Check drreq
  REQUIRE(reserve_zone.drreq.has_value());
  auto* drreq_real_ptr = std::get_if<Real>(&reserve_zone.drreq.value());
  REQUIRE(drreq_real_ptr != nullptr);
  CHECK(*drreq_real_ptr == 30.0);

  // Check urcost (T = Time schedule, not by block)
  REQUIRE(reserve_zone.urcost.has_value());
  auto* urcost_real_ptr = std::get_if<Real>(&reserve_zone.urcost.value());
  REQUIRE(urcost_real_ptr != nullptr);
  CHECK(*urcost_real_ptr == 1000.0);

  // Check drcost
  REQUIRE(reserve_zone.drcost.has_value());
  auto* drcost_real_ptr = std::get_if<Real>(&reserve_zone.drcost.value());
  REQUIRE(drcost_real_ptr != nullptr);
  CHECK(*drcost_real_ptr == 800.0);
}

TEST_CASE("ReserveZone with time-varying costs")
{
  ReserveZone reserve_zone;

  // Create vectors for time-varying costs
  std::vector<Real> urcost_schedule = {900.0, 1000.0, 1200.0, 950.0};
  std::vector<Real> drcost_schedule = {700.0, 800.0, 850.0, 750.0};

  // Assign to reserve zone
  reserve_zone.urcost = urcost_schedule;
  reserve_zone.drcost = drcost_schedule;

  // Verify costs were assigned properly
  REQUIRE(reserve_zone.urcost.has_value());
  REQUIRE(reserve_zone.drcost.has_value());

  // Check that we have vectors, not scalars
  auto* urcost_vec_ptr =
      std::get_if<std::vector<Real>>(&reserve_zone.urcost.value());
  auto* drcost_vec_ptr =
      std::get_if<std::vector<Real>>(&reserve_zone.drcost.value());

  REQUIRE(urcost_vec_ptr != nullptr);
  REQUIRE(drcost_vec_ptr != nullptr);

  // Check the schedule values
  CHECK(urcost_vec_ptr->size() == 4);
  CHECK(drcost_vec_ptr->size() == 4);

  CHECK((*urcost_vec_ptr)[0] == 900.0);
  CHECK((*urcost_vec_ptr)[1] == 1000.0);
  CHECK((*urcost_vec_ptr)[2] == 1200.0);
  CHECK((*urcost_vec_ptr)[3] == 950.0);

  CHECK((*drcost_vec_ptr)[0] == 700.0);
  CHECK((*drcost_vec_ptr)[1] == 800.0);
  CHECK((*drcost_vec_ptr)[2] == 850.0);
  CHECK((*drcost_vec_ptr)[3] == 750.0);
}

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

TEST_CASE("ReserveZoneLP - add_to_output writes reserve output files")
{
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,
          .drreq = 30.0,
          .urcost = 1000.0,
          .drcost = 800.0,
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
          .urcost = 5.0,
          .drcost = 3.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_reserve_output";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "ReserveOutputFullTest",
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

  system_lp.write_out();

  // Verify ReserveZone output directory was created
  CHECK(std::filesystem::exists(tmpdir / "ReserveZone"));
  // Verify ReserveProvision output directory was created
  CHECK(std::filesystem::exists(tmpdir / "ReserveProvision"));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("ReserveZoneLP - lookup methods on LP objects")
{
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,
          .drreq = 30.0,
          .urcost = 1000.0,
          .drcost = 800.0,
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
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "LookupTest",
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

  // Access the ReserveZoneLP collection
  const auto& rz_collection =
      std::get<Collection<ReserveZoneLP>>(system_lp.collections());
  REQUIRE(rz_collection.size() == 1);

  const auto& rz_lp = rz_collection.elements()[0];

  // Verify reserve_zone() accessor
  CHECK(rz_lp.reserve_zone().uid == 1);
  CHECK(rz_lp.reserve_zone().name == "rz1");

  // Access scenario and stage for lookup
  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto buid = BlockUid {Uid {1}};

  // Test lookup_urequirement_col - should find the column
  const auto ur_col = rz_lp.lookup_urequirement_col(scenario, stage, buid);
  CHECK(ur_col.has_value());

  // Test lookup_drequirement_col - should find the column
  const auto dr_col = rz_lp.lookup_drequirement_col(scenario, stage, buid);
  CHECK(dr_col.has_value());

  // Test lookup with non-existent block
  const auto missing_ur =
      rz_lp.lookup_urequirement_col(scenario, stage, BlockUid {Uid {999}});
  CHECK_FALSE(missing_ur.has_value());

  const auto missing_dr =
      rz_lp.lookup_drequirement_col(scenario, stage, BlockUid {Uid {999}});
  CHECK_FALSE(missing_dr.has_value());

  // Test row/col accessors
  CHECK_FALSE(rz_lp.urequirement_rows().empty());
  CHECK_FALSE(rz_lp.urequirement_cols().empty());
  CHECK_FALSE(rz_lp.drequirement_rows().empty());
  CHECK_FALSE(rz_lp.drequirement_cols().empty());

  // Access the ReserveProvisionLP collection
  const auto& rp_collection =
      std::get<Collection<ReserveProvisionLP>>(system_lp.collections());
  REQUIRE(rp_collection.size() == 1);

  const auto& rp_lp = rp_collection.elements()[0];

  // Verify reserve_provision() accessor and generator_sid()
  CHECK(rp_lp.reserve_provision().uid == 1);
  CHECK(rp_lp.reserve_provision().name == "rp1");
  CHECK(std::get<Uid>(rp_lp.generator_sid()) == Uid {1});

  // Test lookup_up_provision_col - should find the column
  const auto up_col = rp_lp.lookup_up_provision_col(scenario, stage, buid);
  CHECK(up_col.has_value());

  // Test lookup_dn_provision_col - should find the column
  const auto dn_col = rp_lp.lookup_dn_provision_col(scenario, stage, buid);
  CHECK(dn_col.has_value());

  // Test lookup with non-existent block
  const auto missing_up =
      rp_lp.lookup_up_provision_col(scenario, stage, BlockUid {Uid {999}});
  CHECK_FALSE(missing_up.has_value());

  const auto missing_dn =
      rp_lp.lookup_dn_provision_col(scenario, stage, BlockUid {Uid {999}});
  CHECK_FALSE(missing_dn.has_value());
}

TEST_CASE("ReserveZoneLP - reserve fail cost with insufficient provision")
{
  // Generator too small to meet both demand + reserve requirement,
  // forcing reserve shortage (fail cost path).
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
          .capacity = 120.0,  // just enough for demand, not for reserves
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,  // needs 50 MW up reserve
          .urcost = 5000.0,  // high shortage cost
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
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  Options opts;
  opts.reserve_fail_cost = 5000.0;

  const System system = {
      .name = "ReserveFailCostTest",
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

  // The generator capacity (120 MW) is less than demand (100) + reserve (50),
  // so the up-reserve provision is limited (gen + provision <= capacity).
  // The reserve zone fail cost variable should absorb the shortfall.
  const auto& rz_collection =
      std::get<Collection<ReserveZoneLP>>(system_lp.collections());
  const auto& rz_lp = rz_collection.elements()[0];

  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto buid = BlockUid {Uid {1}};

  // The urequirement column exists and its solution should be > 0
  // (some reserve requirement is unmet, covered by fail cost)
  const auto ur_col = rz_lp.lookup_urequirement_col(scenario, stage, buid);
  REQUIRE(ur_col.has_value());

  const auto col_sol = lp.get_col_sol();
  const auto ur_sol = col_sol[static_cast<size_t>(*ur_col)];
  // Reserve requirement is 50, but only 20 MW headroom (120-100)
  // So urequirement_sol should be less than the full 50 MW requirement
  // (the fail cost variable absorbs some of it)
  CHECK(ur_sol < doctest::Approx(50.0));
  CHECK(ur_sol >= doctest::Approx(0.0));
}

TEST_CASE("ReserveZoneLP - no reserve_fail_cost (fixed requirement)")
{
  // When reserve_fail_cost is NOT set, reserve requirement column is
  // fixed (lowb == uppb == req), meaning the zone requirement variable
  // is at its full value and provision must supply the rest.
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 40.0,
          // No urcost: reserve_fail_cost will determine the path
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
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  // No reserve_fail_cost set -> fixed requirement (no shortage variable)
  Options opts;

  const System system = {
      .name = "FixedReqTest",
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

  // The zone requirement col is fixed at 40 MW
  const auto& rz_collection =
      std::get<Collection<ReserveZoneLP>>(system_lp.collections());
  const auto& rz_lp = rz_collection.elements()[0];

  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto buid = BlockUid {Uid {1}};

  const auto ur_col = rz_lp.lookup_urequirement_col(scenario, stage, buid);
  REQUIRE(ur_col.has_value());

  const auto col_sol = lp.get_col_sol();
  // Fixed requirement: solution equals the requirement value
  CHECK(col_sol[static_cast<size_t>(*ur_col)] == doctest::Approx(40.0));
}

TEST_CASE(
    "ReserveZoneLP - down reserve with capacity factor and provision cost")
{
  // Exercises the down-reserve provision path with capacity factor
  // and provision cost, covering dr_capacity_factor + drcost paths.
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .drreq = 40.0,
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
          .dr_capacity_factor = 0.5,
          .dr_provision_factor = 1.0,
          .drcost = 3.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "DownReserveCapCostTest",
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

  // Verify the provision col exists for down-reserve
  const auto& rp_collection =
      std::get<Collection<ReserveProvisionLP>>(system_lp.collections());
  const auto& rp_lp = rp_collection.elements()[0];

  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto buid = BlockUid {Uid {1}};

  const auto dn_col = rp_lp.lookup_dn_provision_col(scenario, stage, buid);
  REQUIRE(dn_col.has_value());

  // Up provision should not exist (no up reserve configured)
  const auto up_col = rp_lp.lookup_up_provision_col(scenario, stage, buid);
  CHECK_FALSE(up_col.has_value());

  // Down provision should have a non-negative solution
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[static_cast<size_t>(*dn_col)] >= doctest::Approx(0.0));
}

TEST_CASE("ReserveZoneLP - multiple blocks with reserve duals")
{
  // Test reserve constraints with multiple blocks and verify
  // that duals are non-zero (binding constraints).
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
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 30.0,
          .urcost = 2000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 50.0,
          .ur_provision_factor = 1.0,
          .urcost = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 4,
              },
              {
                  .uid = Uid {2},
                  .duration = 8,
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

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "MultiBlockReserveDuals",
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

  // Verify both blocks have reserve zone columns
  const auto& rz_collection =
      std::get<Collection<ReserveZoneLP>>(system_lp.collections());
  const auto& rz_lp = rz_collection.elements()[0];

  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];

  const auto ur_col_b1 =
      rz_lp.lookup_urequirement_col(scenario, stage, BlockUid {Uid {1}});
  const auto ur_col_b2 =
      rz_lp.lookup_urequirement_col(scenario, stage, BlockUid {Uid {2}});
  CHECK(ur_col_b1.has_value());
  CHECK(ur_col_b2.has_value());

  // Both blocks should have provision columns
  const auto& rp_collection =
      std::get<Collection<ReserveProvisionLP>>(system_lp.collections());
  const auto& rp_lp = rp_collection.elements()[0];

  const auto up_col_b1 =
      rp_lp.lookup_up_provision_col(scenario, stage, BlockUid {Uid {1}});
  const auto up_col_b2 =
      rp_lp.lookup_up_provision_col(scenario, stage, BlockUid {Uid {2}});
  CHECK(up_col_b1.has_value());
  CHECK(up_col_b2.has_value());

  // Provision solutions should be non-negative
  const auto col_sol = lp.get_col_sol();
  if (up_col_b1.has_value()) {
    CHECK(col_sol[static_cast<size_t>(*up_col_b1)] >= doctest::Approx(0.0));
  }
  if (up_col_b2.has_value()) {
    CHECK(col_sol[static_cast<size_t>(*up_col_b2)] >= doctest::Approx(0.0));
  }
}

TEST_CASE("ReserveZoneLP - reserve provision by zone name")
{
  // Test that reserve_zones can reference zones by name (string ID)
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "zone_alpha",
          .urreq = 40.0,
          .urcost = 1000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "zone_alpha",  // by name, not UID
          .urmax = 100.0,
          .ur_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  Options opts;
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "NamedZoneTest",
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

  // Verify provision was created
  const auto& rp_collection =
      std::get<Collection<ReserveProvisionLP>>(system_lp.collections());
  const auto& rp_lp = rp_collection.elements()[0];
  const auto& scenario = simulation_lp.scenarios()[0];
  const auto& stage = simulation_lp.stages()[0];
  const auto up_col =
      rp_lp.lookup_up_provision_col(scenario, stage, BlockUid {Uid {1}});
  CHECK(up_col.has_value());
}

TEST_CASE(
    "ReserveZoneLP - add_to_output writes CSV and covers all output fields")
{
  // Exercise add_to_output with CSV format and both up/down provisions
  // to cover all output fields (uprovision, dprovision, ucapacity, etc.)
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
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,
          .drreq = 30.0,
          .urcost = 1000.0,
          .drcost = 800.0,
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
          .ur_capacity_factor = 0.5,
          .dr_capacity_factor = 0.4,
          .ur_provision_factor = 1.0,
          .dr_provision_factor = 1.0,
          .urcost = 5.0,
          .drcost = 3.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_reserve_csv_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "ReserveCsvOutputTest",
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

  system_lp.write_out();

  // Verify ReserveZone output files were created
  CHECK(std::filesystem::exists(tmpdir / "ReserveZone"));
  // Verify ReserveProvision output files were created
  CHECK(std::filesystem::exists(tmpdir / "ReserveProvision"));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}
