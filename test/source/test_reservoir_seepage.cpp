#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ReservoirSeepage construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ReservoirSeepage seepage;

  CHECK(seepage.uid == Uid {unknown_uid});
  CHECK(seepage.name == Name {});
  CHECK_FALSE(seepage.active.has_value());

  CHECK(seepage.waterway == SingleId {unknown_uid});
  CHECK(seepage.reservoir == SingleId {unknown_uid});
  // slope and constant are OptTRealFieldSched — default is nullopt (→ 0.0)
  CHECK_FALSE(seepage.slope.has_value());
  CHECK_FALSE(seepage.constant.has_value());
}

TEST_CASE("ReservoirSeepage attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ReservoirSeepage seepage;

  seepage.uid = 8001;
  seepage.name = "TestReservoirSeepage";
  seepage.active = true;

  seepage.waterway = Uid {6001};
  seepage.reservoir = Uid {9001};
  seepage.slope = 0.15;
  seepage.constant = 2.5;

  CHECK(seepage.uid == 8001);
  CHECK(seepage.name == "TestReservoirSeepage");
  CHECK(std::get<IntBool>(seepage.active.value()) == 1);

  CHECK(std::get<Uid>(seepage.waterway) == Uid {6001});
  CHECK(std::get<Uid>(seepage.reservoir) == Uid {9001});
  CHECK(std::get<Real>(seepage.slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.15));
  CHECK(std::get<Real>(seepage.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(2.5));
}

TEST_CASE("ReservoirSeepage with zero slope")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ReservoirSeepage seepage;

  seepage.uid = 8002;
  seepage.name = "ConstantReservoirSeepage";
  seepage.slope = 0.0;
  seepage.constant = 5.0;

  CHECK(std::get<Real>(seepage.slope.value_or(TRealFieldSched {0.0})) == 0.0);
  CHECK(std::get<Real>(seepage.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(5.0));
}

TEST_CASE("ReservoirSeepageLP - basic seepage constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 1.0,
      },
  };

  const Array<ReservoirSeepage> reservoir_seepage_array = {
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
      .name = "ReservoirSeepageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

TEST_CASE("ReservoirSeepageLP - multi-block seepage")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
      },
  };

  const Array<ReservoirSeepage> reservoir_seepage_array = {
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
      .name = "MultiReservoirSeepageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

TEST_CASE("ReservoirSeepageSegment construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ReservoirSeepageSegment seg;

  CHECK(seg.volume == 0.0);
  CHECK(seg.slope == 0.0);
  CHECK(seg.constant == 0.0);
}

TEST_CASE("evaluate_seepage with empty segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<ReservoirSeepageSegment> segments {};
  CHECK(evaluate_seepage(segments, 100.0) == 0.0);
}

TEST_CASE("evaluate_seepage with single segment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<ReservoirSeepageSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
  };

  // At V=0: constant + slope * V = 2.0 + 0.001 * 0 = 2.0
  CHECK(evaluate_seepage(segments, 0.0) == doctest::Approx(2.0));

  // At V=500: 2.0 + 0.001 * 500 = 2.5
  CHECK(evaluate_seepage(segments, 500.0) == doctest::Approx(2.5));
}

TEST_CASE("evaluate_seepage with two segments (piecewise linear)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two segments modelling a piecewise-linear seepage curve:
  //   seg1: constant=2.0 (y-intercept), slope=0.001, breakpoint=0.0
  //         → seepage = 2.0 + 0.001 * V  (applies when V < 500)
  //   seg2: constant=2.7 (y-intercept), slope=0.0002, breakpoint=500.0
  //         → seepage = 2.7 + 0.0002 * V  (applies when V ≥ 500)
  //
  // Note: constants are y-intercepts (PLP Fortran FiltConst), matching the
  // format stored by filemb_parser.py.  The function applies the segment whose
  // breakpoint is the largest that is ≤ the current volume (range selection,
  // matching PLP FFiltracionesi).
  //
  // Values at key volumes:
  //   V=0:    seg1 → 2.0 + 0.001*0   = 2.0
  //   V=500:  seg2 → 2.7 + 0.0002*500 = 2.8
  //   V=1500: seg2 → 2.7 + 0.0002*1500 = 3.0
  const std::vector<ReservoirSeepageSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.7},
  };

  CHECK(evaluate_seepage(segments, 0.0) == doctest::Approx(2.0));
  CHECK(evaluate_seepage(segments, 500.0) == doctest::Approx(2.8));
  CHECK(evaluate_seepage(segments, 1500.0) == doctest::Approx(3.0));
}

TEST_CASE("select_seepage_coeffs with two segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Constants are y-intercepts (PLP format: seepage = constant + slope * V).
  // seg1: y-intercept=2.0, slope=0.001, breakpoint=0.0
  // seg2: y-intercept=2.7, slope=0.0002, breakpoint=500.0
  //       (at V=500: 2.7 + 0.0002*500 = 2.8)
  //
  // Segment selection follows PLP range logic: pick segment whose breakpoint
  // is the largest ≤ current volume.
  const std::vector<ReservoirSeepageSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.7},
  };

  // At V=0: seg1 selected (breakpoint 0 ≤ 0 < 500)
  // LP: filt - 0.001*0.5*eini - 0.001*0.5*efin = 2.0
  //     → filt = 2.0 + 0.001 * V_avg  (positive at any V_avg ≥ 0)
  auto c0 = select_seepage_coeffs(segments, 0.0);
  CHECK(c0.slope == doctest::Approx(0.001));
  CHECK(c0.intercept == doctest::Approx(2.0));

  // At V=1500: seg2 selected (breakpoint 500 ≤ 1500)
  // LP: filt - 0.0002*0.5*eini - 0.0002*0.5*efin = 2.7
  //     → filt = 2.7 + 0.0002 * V_avg  (positive at any V_avg ≥ 0)
  auto c1500 = select_seepage_coeffs(segments, 1500.0);
  CHECK(c1500.slope == doctest::Approx(0.0002));
  CHECK(c1500.intercept == doctest::Approx(2.7));
}

TEST_CASE("select_seepage_coeffs empty segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const std::vector<ReservoirSeepageSegment> segments {};
  auto c = select_seepage_coeffs(segments, 100.0);
  CHECK(c.slope == 0.0);
  CHECK(c.intercept == 0.0);
}

TEST_CASE("ReservoirSeepage with segments default")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ReservoirSeepage seepage;
  CHECK(seepage.segments.empty());

  seepage.segments = {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.8},
  };
  CHECK(seepage.segments.size() == 2);
}

TEST_CASE("ReservoirSeepageLP - piecewise segments LP constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 1.0,
      },
  };

  // ReservoirSeepage with piecewise-linear segments
  const Array<ReservoirSeepage> reservoir_seepage_array = {
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
      .name = "PiecewiseReservoirSeepageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

TEST_CASE("ReservoirSeepageLP - per-stage slope/constant schedule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Test that slope and constant can be provided as per-stage arrays
  // (simulating what plpmanfi.dat Parquet files would provide).

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
          .capacity = 60.0,
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

  // ReservoirSeepage with per-stage slope/constant (as inline vectors).
  // Stage 1 (uid=1): slope=0.001, constant=1.0
  // Stage 2 (uid=2): slope=0.002, constant=2.0
  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = TRealFieldSched {std::vector<Real> {0.001, 0.002}},
          .constant = TRealFieldSched {std::vector<Real> {1.0, 2.0}},
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
      .name = "PerStageReservoirSeepageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

// ── Coverage tests for uncovered code paths in reservoir_seepage_lp.cpp
// ────────────

TEST_CASE("ReservoirSeepageLP - seepage_cols_at accessor")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Verify seepage_cols_at() returns valid per-block column indices.
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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
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

  const System system = {
      .name = "FiltColsAtTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP opts;
  SimulationLP simulation_lp(simulation, opts);
  SystemLP system_lp(system, simulation_lp);

  auto& filts = system_lp.elements<ReservoirSeepageLP>();
  REQUIRE(filts.size() == 1);

  auto& filt = filts.front();
  const auto& scenarios = simulation_lp.scenarios();
  const auto& stages = simulation_lp.stages();

  // seepage_cols_at should return a map with 2 blocks
  const auto& cols = filt.seepage_cols_at(scenarios.front(), stages.front());
  CHECK(cols.size() == 2);
}

TEST_CASE("ReservoirSeepageLP - add_to_output via write_out")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Solve then call write_out() which exercises
  // ReservoirSeepageLP::add_to_output.
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "test_reservoir_seepage_output";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const System system = {
      .name = "FiltOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // write_out exercises add_to_output for all elements incl. ReservoirSeepageLP
  system_lp.write_out();

  // Verify ReservoirSeepage output directory was created
  CHECK(std::filesystem::exists(tmpdir / "ReservoirSeepage"));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("ReservoirSeepageLP - update_lp with piecewise segments")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Piecewise segments with eini=5000 -> seg2 at construction.
  // update_lp iter=0/phase=0 uses eini -> same segment -> no change.
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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 0.0,
                      .slope = 0.002,
                      .constant = 0.5,
                  },
                  {
                      .volume = 3000.0,
                      .slope = 0.0005,
                      .constant = 3.0,
                  },
              },
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
      .name = "UpdateReservoirSeepageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

  SUBCASE("seepage_cols_at with segments returns 1 block")
  {
    auto& filts = system_lp.elements<ReservoirSeepageLP>();
    REQUIRE(filts.size() == 1);
    auto& filt = filts.front();
    const auto& cols = filt.seepage_cols_at(simulation_lp.scenarios().front(),
                                            simulation_lp.stages().front());
    CHECK(cols.size() == 1);
  }
}

TEST_CASE("ReservoirSeepageLP - update_lp with different eini segment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // eini=1000 -> seg1 (volume < 3000). update_lp iter=0/phase=0 uses eini.
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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .eini = 1000.0,
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 0.0,
                      .slope = 0.002,
                      .constant = 0.5,
                  },
                  {
                      .volume = 3000.0,
                      .slope = 0.0005,
                      .constant = 3.0,
                  },
              },
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
      .name = "UpdateFiltDiffEini",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

  // eini=1000 -> seg1.  As of `fix(reservoir): always re-issue update_lp
  // coefficients`, update_lp no longer short-circuits when the in-memory
  // `state.current_*` matches: it always re-issues `set_coeff` +
  // `set_rhs` to keep `LowMemoryMode::compress` consistent with `off`
  // after `load_flat` reverts the live LP to construction-time values.
  // Count is one row processed (this fixture has a single seepage row).
  const auto updated0 = system_lp.update_lp();
  CHECK(updated0 == 1);

  // Re-solve remains feasible
  auto result2 = lp.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);

  // ── Regression: simulate the compress-mode `load_flat` revert ──
  //
  // Under `LowMemoryMode::compress`, `release_backend` →
  // `reconstruct_backend` reloads matval/RHS from the snapshot —
  // every `update_lp_for_phase` mutation is wiped.  In-memory
  // `state.current_*` survives unchanged, so the previous bug was
  // an early-return based on a stale memory predicate, leaving the
  // live LP at construction-time values while cuts assumed updated
  // values → primal-infeasible backward re-solves.
  //
  // Mirror the bug pattern via the public solve API: solve once
  // (capture the solution that depends on the active seepage
  // segment), then RESOLVE without re-issuing update_lp — the
  // solution must remain consistent because update_lp is a no-op
  // when the segment hasn't changed.  The post-fix `update_lp`
  // ALWAYS issues the writes (to keep compress/off symmetric),
  // which is observable via `updated >= 1` even on a no-segment-
  // change call.  Two consecutive `update_lp` calls on the same
  // state must each report >=1 (always-re-issue contract).
  SUBCASE("update_lp always re-issues even when state is unchanged")
  {
    const auto updated_first = system_lp.update_lp();
    CHECK(updated_first == 1);
    // No state change since the previous call — pre-fix returned 0
    // here; post-fix still re-issues.
    const auto updated_second = system_lp.update_lp();
    CHECK(updated_second == 1);
    // Resolving after the no-op-state update_lp must remain feasible.
    auto resolved = lp.resolve();
    REQUIRE(resolved.has_value());
    CHECK(resolved.value() == 0);
  }
}

TEST_CASE("ReservoirSeepageLP - zero-slope segment edge case")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Single segment with zero slope -- pure constant seepage.
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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 0.0,
                      .slope = 0.0,
                      .constant = 3.0,
                  },
              },
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
      .name = "ZeroSlopeReservoirSeepage",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
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

  // update_lp with zero-slope segment now always re-issues writes:
  // see `fix(reservoir): always re-issue update_lp coefficients`.
  // The single seepage row counts as 1 update.  The `set_coeff` /
  // `set_rhs` calls invalidate the cached solution (per
  // `invalidate_cached_optimal_on_mutation`), so a subsequent
  // `lp.get_col_sol()` would return stale data — re-resolve to
  // restore an optimal cached solution before reading.
  const auto updated = system_lp.update_lp();
  CHECK(updated == 1);

  auto result_after_update = lp.resolve();
  REQUIRE(result_after_update.has_value());
  REQUIRE(lp.is_optimal());

  // Verify the seepage flow equals the constant (3.0) with zero slope
  const auto col_sol = lp.get_col_sol();
  auto& filts = system_lp.elements<ReservoirSeepageLP>();
  REQUIRE(filts.size() == 1);
  auto& filt = filts.front();
  const auto& cols = filt.seepage_cols_at(simulation_lp.scenarios().front(),
                                          simulation_lp.stages().front());
  for (const auto& [buid, col] : cols) {
    CHECK(col_sol[static_cast<size_t>(col)] == doctest::Approx(3.0));
  }
}

TEST_CASE("ReservoirSeepageLP - multi-stage segments with output")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two stages with piecewise segments -- exercises add_to_lp across stages,
  // seepage_cols_at for multiple keys, and add_to_output.
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "test_filt_multi_stage_out";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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

  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 0.0,
                      .slope = 0.001,
                      .constant = 1.0,
                  },
                  {
                      .volume = 5000.0,
                      .slope = 0.0002,
                      .constant = 4.0,
                  },
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const System system = {
      .name = "MultiStagePiecewiseFilt",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify seepage_cols_at works for both stages
  auto& filts = system_lp.elements<ReservoirSeepageLP>();
  REQUIRE(filts.size() == 1);
  auto& filt = filts.front();
  const auto& scenarios = simulation_lp.scenarios();
  const auto& stages = simulation_lp.stages();

  const auto& cols_s1 = filt.seepage_cols_at(scenarios.front(), stages[0]);
  const auto& cols_s2 = filt.seepage_cols_at(scenarios.front(), stages[1]);
  CHECK(cols_s1.size() == 1);
  CHECK(cols_s2.size() == 1);

  // write_out exercises add_to_output
  system_lp.write_out();
  CHECK(std::filesystem::exists(tmpdir / "ReservoirSeepage"));

  std::filesystem::remove_all(tmpdir);
}
