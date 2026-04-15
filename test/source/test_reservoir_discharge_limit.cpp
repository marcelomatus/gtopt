// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ReservoirDischargeLimit construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ReservoirDischargeLimit ddl;

  CHECK(ddl.uid == Uid {unknown_uid});
  CHECK(ddl.name == Name {});
  CHECK_FALSE(ddl.active.has_value());

  CHECK(ddl.waterway == SingleId {unknown_uid});
  CHECK(ddl.reservoir == SingleId {unknown_uid});
  CHECK(ddl.segments.empty());
}

TEST_CASE("ReservoirDischargeLimit attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ReservoirDischargeLimit ddl;

  ddl.uid = 1;
  ddl.name = "ddl_ralco";
  ddl.active = true;
  ddl.waterway = Uid {10};
  ddl.reservoir = Uid {20};
  ddl.segments = {
      {.volume = 0.0, .slope = 6.9868e-5, .intercept = 15.787},
      {.volume = 757000.0, .slope = 1.3985e-4, .intercept = 57.454},
  };

  CHECK(ddl.uid == 1);
  CHECK(ddl.name == "ddl_ralco");
  CHECK(std::get<IntBool>(ddl.active.value()) == 1);
  CHECK(std::get<Uid>(ddl.waterway) == Uid {10});
  CHECK(std::get<Uid>(ddl.reservoir) == Uid {20});
  REQUIRE(ddl.segments.size() == 2);
  CHECK(ddl.segments[0].volume == doctest::Approx(0.0));
  CHECK(ddl.segments[0].slope == doctest::Approx(6.9868e-5));
  CHECK(ddl.segments[0].intercept == doctest::Approx(15.787));
  CHECK(ddl.segments[1].volume == doctest::Approx(757000.0));
  CHECK(ddl.segments[1].slope == doctest::Approx(1.3985e-4));
  CHECK(ddl.segments[1].intercept == doctest::Approx(57.454));
}

TEST_CASE("ReservoirDischargeLimitSegment construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ReservoirDischargeLimitSegment seg;

  CHECK(seg.volume == 0.0);
  CHECK(seg.slope == 0.0);
  CHECK(seg.intercept == 0.0);
}

TEST_CASE("find_active_rdl_segment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<ReservoirDischargeLimitSegment> segments = {
      {.volume = 0.0, .slope = 6.9868e-5, .intercept = 15.787},
      {.volume = 757000.0, .slope = 1.3985e-4, .intercept = 57.454},
  };

  SUBCASE("volume below first breakpoint uses first segment")
  {
    const auto* active = find_active_rdl_segment(segments, -100.0);
    CHECK(active == &segments[0]);
  }

  SUBCASE("volume at first breakpoint uses first segment")
  {
    const auto* active = find_active_rdl_segment(segments, 0.0);
    CHECK(active == &segments[0]);
  }

  SUBCASE("volume between breakpoints uses first segment")
  {
    const auto* active = find_active_rdl_segment(segments, 500000.0);
    CHECK(active == &segments[0]);
  }

  SUBCASE("volume at second breakpoint uses second segment")
  {
    const auto* active = find_active_rdl_segment(segments, 757000.0);
    CHECK(active == &segments[1]);
  }

  SUBCASE("volume above second breakpoint uses second segment")
  {
    const auto* active = find_active_rdl_segment(segments, 1000000.0);
    CHECK(active == &segments[1]);
  }
}

TEST_CASE("select_rdl_coeffs")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("empty segments returns zeros")
  {
    const std::vector<ReservoirDischargeLimitSegment> segments {};
    const auto coeffs = select_rdl_coeffs(segments, 100.0);
    CHECK(coeffs.slope == 0.0);
    CHECK(coeffs.intercept == 0.0);
  }

  SUBCASE("selects correct coefficients for volume")
  {
    const std::vector<ReservoirDischargeLimitSegment> segments = {
        {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
        {.volume = 500.0, .slope = 2e-4, .intercept = 20.0},
    };

    const auto c1 = select_rdl_coeffs(segments, 100.0);
    CHECK(c1.slope == doctest::Approx(1e-4));
    CHECK(c1.intercept == doctest::Approx(10.0));

    const auto c2 = select_rdl_coeffs(segments, 500.0);
    CHECK(c2.slope == doctest::Approx(2e-4));
    CHECK(c2.intercept == doctest::Approx(20.0));
  }
}

TEST_CASE("ReservoirDischargeLimitLP - basic single-block LP constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_upstream",
      },
      {
          .uid = Uid {2},
          .name = "j_downstream",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
                  {.volume = 5000.0, .slope = 2e-4, .intercept = 30.0},
              },
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

  const System system = {
      .name = "DDLTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
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

TEST_CASE("ReservoirDischargeLimitLP - multi-block averaging constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // With multiple blocks, qeh should be the weighted average of flows
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
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

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
      },
      {
          .uid = Uid {2},
          .name = "j2",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
                  {.volume = 5000.0, .slope = 2e-4, .intercept = 30.0},
              },
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
              {
                  .uid = Uid {3},
                  .duration = 3,
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
                  .first_block = 2,
                  .count_block = 1,
              },
          },
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "DDLMultiBlock",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("ReservoirDischargeLimitLP - empty segments is a no-op")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // ReservoirDischargeLimit with empty segments should not add any constraints
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
      },
      {
          .uid = Uid {2},
          .name = "j2",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  // ReservoirDischargeLimit with NO segments → should be skipped entirely
  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl_empty",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments = {},
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

  // Build two systems: one without DDL, one with empty-segments DDL
  const System system_no_ddl = {
      .name = "NoDDL",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  const System system_with_ddl = {
      .name = "EmptyDDL",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options;

  SimulationLP sim1(simulation, options);
  SystemLP sys1(system_no_ddl, sim1);

  SimulationLP sim2(simulation, options);
  SystemLP sys2(system_with_ddl, sim2);

  // Same number of rows and cols (empty segments = no constraints added)
  CHECK(sys1.linear_interface().get_numrows()
        == sys2.linear_interface().get_numrows());
  CHECK(sys1.linear_interface().get_numcols()
        == sys2.linear_interface().get_numcols());
}

TEST_CASE("ReservoirDischargeLimitLP - binding discharge limit")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Set a very tight discharge limit and verify the LP respects it
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
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

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
      },
      {
          .uid = Uid {2},
          .name = "j2",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100000.0,
          .emin = 0.0,
          .emax = 100000.0,
          .eini = 50000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  // Tight limit: qeh ≤ 0 × V + 50 = 50 m³/s regardless of volume
  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl_tight",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 0.0, .intercept = 50.0},
              },
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

  // System with DDL
  const System system_ddl = {
      .name = "TightDDL",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  // System without DDL
  const System system_no = {
      .name = "NoDDL",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options;

  SimulationLP sim1(simulation, options);
  SystemLP sys1(system_ddl, sim1);
  auto res1 = sys1.linear_interface().resolve();
  REQUIRE(res1.has_value());
  CHECK(res1.value() == 0);
  const auto obj_ddl = sys1.linear_interface().get_obj_value();

  SimulationLP sim2(simulation, options);
  SystemLP sys2(system_no, sim2);
  auto res2 = sys2.linear_interface().resolve();
  REQUIRE(res2.has_value());
  CHECK(res2.value() == 0);
  const auto obj_no = sys2.linear_interface().get_obj_value();

  // With the DDL limiting flow to 50 m³/s, the hydro generation is capped
  // and more expensive thermal is needed → higher objective
  CHECK(obj_ddl >= obj_no);
}

TEST_CASE("ReservoirDischargeLimitLP - update_lp with piecewise segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two segments: seg1 for V < 5000, seg2 for V >= 5000.
  // eini=5000 → seg2 at construction. update_lp iter=0/phase=0 uses eini →
  // same segment → no change.
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "d1",
      .bus = Uid {1},
      .capacity = 50.0,
  }};

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 500.0,
  }};

  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 10000.0,
      .emin = 0.0,
      .emax = 10000.0,
      .eini = 5000.0,
      .fmin = -1000.0,
      .fmax = 1000.0,
      .flow_conversion_rate = 1.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {{
      .uid = Uid {1},
      .name = "ddl1",
      .waterway = Uid {1},
      .reservoir = Uid {1},
      .segments =
          {
              {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
              {.volume = 5000.0, .slope = 2e-4, .intercept = 30.0},
          },
  }};

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1.0,
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

  const System system = {
      .name = "UpdateDDLTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  SUBCASE("update_lp uses vavg from eini and efin solution")
  {
    // volume = (eini + efin_from_solution) / 2; update count depends on
    // whether the average volume selects a different piecewise segment
    // than the one used at add_to_lp time.
    const auto updated = system_lp.update_lp();
    CHECK(updated >= 0);
  }

  SUBCASE("system solves after update_lp")
  {
    [[maybe_unused]] const auto u = system_lp.update_lp();
    auto result2 = lp.resolve();
    REQUIRE(result2.has_value());
    CHECK(result2.value() == 0);
  }
}

TEST_CASE("ReservoirDischargeLimitLP - update_lp with different eini segment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // eini=1000 → seg1 (volume < 5000). update_lp iter=0/phase=0 uses eini.
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "d1",
      .bus = Uid {1},
      .capacity = 50.0,
  }};

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 500.0,
  }};

  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 10000.0,
      .emin = 0.0,
      .emax = 10000.0,
      .eini = 1000.0,
      .fmin = -1000.0,
      .fmax = 1000.0,
      .flow_conversion_rate = 1.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {{
      .uid = Uid {1},
      .name = "ddl1",
      .waterway = Uid {1},
      .reservoir = Uid {1},
      .segments =
          {
              {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
              {.volume = 5000.0, .slope = 2e-4, .intercept = 30.0},
          },
  }};

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1.0,
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

  const System system = {
      .name = "UpdateDDLDiffSegTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // eini=1000 → seg1. update_lp iter=0/phase=0 uses eini → same → 0
  const auto updated = system_lp.update_lp();
  CHECK(updated == 0);

  // Re-solve after update_lp
  auto result2 = lp.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);
}

TEST_CASE("ReservoirDischargeLimitLP - update_lp is a no-op without segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // ReservoirDischargeLimit with 1 segment → update_lp returns 0
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "d1",
      .bus = Uid {1},
      .capacity = 50.0,
  }};

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 500.0,
  }};

  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 10000.0,
      .emin = 0.0,
      .emax = 10000.0,
      .eini = 5000.0,
      .fmin = -1000.0,
      .fmax = 1000.0,
      .flow_conversion_rate = 1.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  // Only 1 segment → update_lp skips (needs < 2 segments guard)
  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {{
      .uid = Uid {1},
      .name = "ddl_single",
      .waterway = Uid {1},
      .reservoir = Uid {1},
      .segments =
          {
              {.volume = 0.0, .slope = 1e-4, .intercept = 50.0},
          },
  }};

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1.0,
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

  const System system = {
      .name = "DDLNoOpTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Single segment → update_lp is a no-op
  const auto updated = system_lp.update_lp();
  CHECK(updated == 0);
}

TEST_CASE("ReservoirDischargeLimitLP - add_to_output via write_out")  // NOLINT
{
  // Exercises ReservoirDischargeLimitLP::add_to_output (qeh_cols + vol_rows)
  // by calling system_lp.write_out() after the solve.
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j1"},
      {.uid = Uid {2}, .name = "j2", .drain = true},
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
                  {.volume = 5000.0, .slope = 2e-4, .intercept = 30.0},
              },
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_ddl_out";
  std::filesystem::create_directories(tmpdir);

  const System system = {
      .name = "DDLOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Objective should be non-negative: demand=80 MW served by a mix of
  // cheap hydro (gcost=5) and expensive thermal (gcost=100).
  CHECK(lp.get_obj_value() >= 0.0);

  // Verify the hydro generator's generation col has a non-negative value.
  // With production_factor=1 and the DDL limiting discharge, the hydro
  // gen produces some but not all the demand.
  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& hydro_gen_lp = gen_lps.front();
  const auto& hydro_gen_cols =
      hydro_gen_lp.generation_cols_at(scenario_lp, stage_lp);
  CHECK_FALSE(hydro_gen_cols.empty());
  const auto sol = lp.get_col_sol();
  for (const auto& [buid, col] : hydro_gen_cols) {
    CHECK(sol[col] >= 0.0);
  }

  // Exercises ReservoirDischargeLimitLP::add_to_output
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}
