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
  // slope and constant are OptTRealFieldSched — default is nullopt (→ 0.0)
  CHECK_FALSE(filtration.slope.has_value());
  CHECK_FALSE(filtration.constant.has_value());
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
  CHECK(std::get<Real>(filtration.slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.15));
  CHECK(std::get<Real>(filtration.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(2.5));
}

TEST_CASE("Filtration with zero slope")
{
  Filtration filtration;

  filtration.uid = 8002;
  filtration.name = "ConstantFiltration";
  filtration.slope = 0.0;
  filtration.constant = 5.0;

  CHECK(std::get<Real>(filtration.slope.value_or(TRealFieldSched {0.0}))
        == 0.0);
  CHECK(std::get<Real>(filtration.constant.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(5.0));
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

  // At V=0: constant + slope * V = 2.0 + 0.001 * 0 = 2.0
  CHECK(evaluate_filtration(segments, 0.0) == doctest::Approx(2.0));

  // At V=500: 2.0 + 0.001 * 500 = 2.5
  CHECK(evaluate_filtration(segments, 500.0) == doctest::Approx(2.5));
}

TEST_CASE("evaluate_filtration with two segments (piecewise linear)")
{
  // Two segments modelling a piecewise-linear filtration curve:
  //   seg1: constant=2.0 (y-intercept), slope=0.001, breakpoint=0.0
  //         → filtration = 2.0 + 0.001 * V  (applies when V < 500)
  //   seg2: constant=2.7 (y-intercept), slope=0.0002, breakpoint=500.0
  //         → filtration = 2.7 + 0.0002 * V  (applies when V ≥ 500)
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
  const std::vector<FiltrationSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.7},
  };

  CHECK(evaluate_filtration(segments, 0.0) == doctest::Approx(2.0));
  CHECK(evaluate_filtration(segments, 500.0) == doctest::Approx(2.8));
  CHECK(evaluate_filtration(segments, 1500.0) == doctest::Approx(3.0));
}

TEST_CASE("select_filtration_coeffs with two segments")
{
  // Constants are y-intercepts (PLP format: filtration = constant + slope * V).
  // seg1: y-intercept=2.0, slope=0.001, breakpoint=0.0
  // seg2: y-intercept=2.7, slope=0.0002, breakpoint=500.0
  //       (at V=500: 2.7 + 0.0002*500 = 2.8)
  //
  // Segment selection follows PLP range logic: pick segment whose breakpoint
  // is the largest ≤ current volume.
  const std::vector<FiltrationSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.7},
  };

  // At V=0: seg1 selected (breakpoint 0 ≤ 0 < 500)
  // LP: filt - 0.001*0.5*eini - 0.001*0.5*efin = 2.0
  //     → filt = 2.0 + 0.001 * V_avg  (positive at any V_avg ≥ 0)
  auto c0 = select_filtration_coeffs(segments, 0.0);
  CHECK(c0.slope == doctest::Approx(0.001));
  CHECK(c0.intercept == doctest::Approx(2.0));

  // At V=1500: seg2 selected (breakpoint 500 ≤ 1500)
  // LP: filt - 0.0002*0.5*eini - 0.0002*0.5*efin = 2.7
  //     → filt = 2.7 + 0.0002 * V_avg  (positive at any V_avg ≥ 0)
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

TEST_CASE("FiltrationLP - per-stage slope/constant schedule")
{
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
          .conversion_rate = 1.0,
      },
  };

  // Filtration with per-stage slope/constant (as inline vectors).
  // Stage 1 (uid=1): slope=0.001, constant=1.0
  // Stage 2 (uid=2): slope=0.002, constant=2.0
  const Array<Filtration> filtration_array = {
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
      .name = "PerStageFiltrationTest",
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

// ── Coverage tests for uncovered code paths in filtration_lp.cpp ────────────

TEST_CASE("FiltrationLP - filtration_cols_at accessor")
{
  // Verify filtration_cols_at() returns valid per-block column indices.
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
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  const OptionsLP opts;
  SimulationLP simulation_lp(simulation, opts);
  SystemLP system_lp(system, simulation_lp);

  auto& filts = system_lp.elements<FiltrationLP>();
  REQUIRE(filts.size() == 1);

  auto& filt = filts.front();
  const auto& scenarios = simulation_lp.scenarios();
  const auto& stages = simulation_lp.stages();

  // filtration_cols_at should return a map with 2 blocks
  const auto& cols = filt.filtration_cols_at(scenarios.front(), stages.front());
  CHECK(cols.size() == 2);
}

TEST_CASE("FiltrationLP - add_to_output via write_out")
{
  // Solve then call write_out() which exercises FiltrationLP::add_to_output.
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "test_filtration_output";
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
  opts.output_directory = tmpdir.string();
  opts.output_format = std::string {"parquet"};

  const System system = {
      .name = "FiltOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // write_out exercises add_to_output for all elements incl. FiltrationLP
  system_lp.write_out();

  // Verify Filtration output directory was created
  CHECK(std::filesystem::exists(tmpdir / "Filtration"));

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("FiltrationLP - update_lp with piecewise segments")
{
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
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
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
      .name = "UpdateFiltrationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.demand_fail_cost = OptReal {1000.0};
  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  SUBCASE("update_lp iteration 0 phase 0 uses eini -- no change")
  {
    // eini=5000 -> seg2 both at add_to_lp and update_lp -> 0
    const auto updated =
        update_lp_coefficients(system_lp, options_lp, 0, PhaseIndex {0});
    CHECK(updated == 0);
  }

  SUBCASE("filtration_cols_at with segments returns 1 block")
  {
    auto& filts = system_lp.elements<FiltrationLP>();
    REQUIRE(filts.size() == 1);
    auto& filt = filts.front();
    const auto& cols = filt.filtration_cols_at(
        simulation_lp.scenarios().front(), simulation_lp.stages().front());
    CHECK(cols.size() == 1);
  }
}

TEST_CASE("FiltrationLP - update_lp with different eini segment")
{
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
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
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
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.demand_fail_cost = OptReal {1000.0};
  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // eini=1000 -> seg1. update_lp iter=0/phase=0 uses eini -> same -> 0
  const auto updated0 =
      update_lp_coefficients(system_lp, options_lp, 0, PhaseIndex {0});
  CHECK(updated0 == 0);

  // Re-solve remains feasible
  auto result2 = lp.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);
}

TEST_CASE("FiltrationLP - zero-slope segment edge case")
{
  // Single segment with zero slope -- pure constant filtration.
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
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
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
      .name = "ZeroSlopeFiltration",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.demand_fail_cost = OptReal {1000.0};
  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // update_lp with zero-slope segment is no-op (same coefficients)
  const auto updated =
      update_lp_coefficients(system_lp, options_lp, 0, PhaseIndex {0});
  CHECK(updated == 0);

  // Verify the filtration flow equals the constant (3.0) with zero slope
  const auto col_sol = lp.get_col_sol();
  auto& filts = system_lp.elements<FiltrationLP>();
  REQUIRE(filts.size() == 1);
  auto& filt = filts.front();
  const auto& cols = filt.filtration_cols_at(simulation_lp.scenarios().front(),
                                             simulation_lp.stages().front());
  for (const auto& [buid, col] : cols) {
    CHECK(col_sol[static_cast<size_t>(col)] == doctest::Approx(3.0));
  }
}

TEST_CASE("FiltrationLP - multi-stage segments with output")
{
  // Two stages with piecewise segments -- exercises add_to_lp across stages,
  // filtration_cols_at for multiple keys, and add_to_output.
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
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
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

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = std::string {"parquet"};

  const System system = {
      .name = "MultiStagePiecewiseFilt",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  const OptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify filtration_cols_at works for both stages
  auto& filts = system_lp.elements<FiltrationLP>();
  REQUIRE(filts.size() == 1);
  auto& filt = filts.front();
  const auto& scenarios = simulation_lp.scenarios();
  const auto& stages = simulation_lp.stages();

  const auto& cols_s1 = filt.filtration_cols_at(scenarios.front(), stages[0]);
  const auto& cols_s2 = filt.filtration_cols_at(scenarios.front(), stages[1]);
  CHECK(cols_s1.size() == 1);
  CHECK(cols_s2.size() == 1);

  // write_out exercises add_to_output
  system_lp.write_out();
  CHECK(std::filesystem::exists(tmpdir / "Filtration"));

  std::filesystem::remove_all(tmpdir);
}
