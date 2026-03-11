#include <doctest/doctest.h>
#include <gtopt/filtration.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Filtration construction and default values")
{
  const Filtration filtration;

  CHECK(filtration.uid == Uid {unknown_uid});
  CHECK(filtration.name == Name {});
  CHECK_FALSE(filtration.active.has_value());

  CHECK(filtration.waterway == SingleId {unknown_uid});
  CHECK(filtration.reservoir == SingleId {unknown_uid});
  CHECK(filtration.slope == 0.0);
  CHECK(filtration.constant == 0.0);
}

TEST_CASE("Filtration attribute assignment")
{
  Filtration filtration;

  filtration.uid = 8001;
  filtration.name = "TestFiltration";
  filtration.active = true;

  filtration.waterway = Uid {6001};
  filtration.reservoir = Uid {9001};
  filtration.slope = 0.15;
  filtration.constant = 2.5;

  CHECK(filtration.uid == 8001);
  CHECK(filtration.name == "TestFiltration");
  CHECK(std::get<IntBool>(filtration.active.value()) == 1);

  CHECK(std::get<Uid>(filtration.waterway) == Uid {6001});
  CHECK(std::get<Uid>(filtration.reservoir) == Uid {9001});
  CHECK(filtration.slope == doctest::Approx(0.15));
  CHECK(filtration.constant == doctest::Approx(2.5));
}

TEST_CASE("Filtration with zero slope")
{
  Filtration filtration;

  filtration.uid = 8002;
  filtration.name = "ConstantFiltration";
  filtration.slope = 0.0;
  filtration.constant = 5.0;

  CHECK(filtration.slope == 0.0);
  CHECK(filtration.constant == doctest::Approx(5.0));
}

TEST_CASE("FiltrationLP - basic filtration constraint")
{
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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_upstream"},
      {.uid = Uid {2}, .name = "j_downstream", .drain = true},
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
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = 0.001,
          .constant = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "FiltrationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
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

TEST_CASE("FiltrationLP - multi-block filtration")
{
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
          .conversion_rate = 2.0,
      },
  };

  const Array<Filtration> filtration_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = 0.0005,
          .constant = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
              {.uid = Uid {2}, .first_block = 2, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "MultiFiltrationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("FiltrationSegment construction")
{
  const FiltrationSegment seg;

  CHECK(seg.volume == 0.0);
  CHECK(seg.slope == 0.0);
  CHECK(seg.constant == 0.0);
}

TEST_CASE("evaluate_filtration with empty segments")
{
  const std::vector<FiltrationSegment> segments {};
  CHECK(evaluate_filtration(segments, 100.0) == 0.0);
}

TEST_CASE("evaluate_filtration with single segment")
{
  const std::vector<FiltrationSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
  };

  // At V=0: 2.0 + 0.001 * (0 - 0) = 2.0
  CHECK(evaluate_filtration(segments, 0.0) == doctest::Approx(2.0));

  // At V=500: 2.0 + 0.001 * 500 = 2.5
  CHECK(evaluate_filtration(segments, 500.0) == doctest::Approx(2.5));
}

TEST_CASE("evaluate_filtration with two segments (concave envelope)")
{
  // Two segments creating a concave envelope:
  // seg1: constant=2.0, slope=0.001, volume=0.0 → 2.0 + 0.001 * V
  // seg2: constant=2.8, slope=0.0002, volume=500.0 → 2.8 + 0.0002*(V-500)
  //
  // At V=0:   seg1=2.0, seg2=2.8+0.0002*(-500)=2.7 → min=2.0
  // At V=500: seg1=2.5, seg2=2.8 → min=2.5
  // At V=1500: seg1=3.5, seg2=2.8+0.0002*1000=3.0 → min=3.0
  const std::vector<FiltrationSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.8},
  };

  CHECK(evaluate_filtration(segments, 0.0) == doctest::Approx(2.0));
  CHECK(evaluate_filtration(segments, 500.0) == doctest::Approx(2.5));
  CHECK(evaluate_filtration(segments, 1500.0) == doctest::Approx(3.0));
}

TEST_CASE("select_filtration_coeffs with two segments")
{
  const std::vector<FiltrationSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.8},
  };

  // At V=0: seg1 gives min → slope=0.001, intercept=2.0-0.001*0=2.0
  auto c0 = select_filtration_coeffs(segments, 0.0);
  CHECK(c0.slope == doctest::Approx(0.001));
  CHECK(c0.intercept == doctest::Approx(2.0));

  // At V=1500: seg2 gives min → slope=0.0002, intercept=2.8-0.0002*500=2.7
  auto c1500 = select_filtration_coeffs(segments, 1500.0);
  CHECK(c1500.slope == doctest::Approx(0.0002));
  CHECK(c1500.intercept == doctest::Approx(2.7));
}

TEST_CASE("select_filtration_coeffs empty segments")
{
  const std::vector<FiltrationSegment> segments {};
  auto c = select_filtration_coeffs(segments, 100.0);
  CHECK(c.slope == 0.0);
  CHECK(c.intercept == 0.0);
}

TEST_CASE("Filtration with segments default")
{
  Filtration filtration;
  CHECK(filtration.segments.empty());

  filtration.segments = {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.8},
  };
  CHECK(filtration.segments.size() == 2);
}

TEST_CASE("FiltrationLP - piecewise segments LP constraint")
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
          .conversion_rate = 1.0,
      },
  };

  // Filtration with piecewise-linear segments
  const Array<Filtration> filtration_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = 0.001,
          .constant = 1.0,
          .segments =
              {
                  {.volume = 0.0, .slope = 0.0003, .constant = 0.5},
                  {.volume = 5000.0, .slope = 0.0001, .constant = 2.0},
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
      .name = "PiecewiseFiltrationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
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
