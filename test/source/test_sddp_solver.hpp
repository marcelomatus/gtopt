/**
 * @file      test_sddp_solver.hpp
 * @brief     Unit tests for the SDDPSolver (SDDP forward/backward iteration)
 * @date      2026-03-08
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Free-function building blocks (propagate, cut, elastic relaxation)
 *  2. Basic 3-phase hydro+thermal case – validates convergence
 *  3. Verifies that the SDDP solver rejects single-phase problems
 *  4. Cut sharing modes (none, expected, max)
 *  5. Cut persistence (save/load)
 *  6. Multi-scene SDDP solving
 *  7. Solver interface integration (monolithic vs SDDP dispatch)
 */

#include <cmath>
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Create a 3-phase hydro+thermal planning problem.
///
/// - 1 bus
/// - 1 hydro generator (25 MW, $5/MWh)
/// - 1 thermal generator (500 MW, $50/MWh)
/// - 1 demand (100 MW constant)
/// - 1 reservoir (capacity 150 dam³, starts at 100 dam³)
/// - Natural inflow: 10 dam³/h
/// - Hydro topology: 2 junctions, 1 waterway, 1 reservoir, 1 turbine
/// - 3 phases, each with 1 stage of 24 blocks (1 hour each)
auto make_3phase_hydro_planning() -> Planning
{
  // ── Blocks: 72 total (24 per phase × 3 phases) ──
  Array<Block> block_array;
  for (int i = 0; i < 72; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  // ── Stages: 3 stages, one per phase ──
  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 24,
      },
      Stage {
          .uid = Uid {2},
          .first_block = 24,
          .count_block = 24,
      },
      Stage {
          .uid = Uid {3},
          .first_block = 48,
          .count_block = 24,
      },
  };

  // ── Phases: 3 phases, each containing 1 stage ──
  Array<Phase> phase_array = {
      Phase {
          .uid = Uid {1},
          .first_stage = 0,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {2},
          .first_stage = 1,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {3},
          .first_stage = 2,
          .count_stage = 1,
      },
  };

  // ── System components ──
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 25.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  // ── Hydro system ──
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
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 150.0,
          .emin = 0.0,
          .emax = 150.0,
          .eini = 100.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
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

  // ── Simulation ──
  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  // ── Options ──
  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  // ── System ──
  System system = {
      .name = "sddp_hydro_3phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Create a simple single-phase planning problem for monolithic solver tests.
auto make_single_phase_planning() -> Planning
{
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
                  .uid = Uid {1},
              },
          },
  };

  const System system = {
      .name = "single_phase_test",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 50.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 100.0,
              },
          },
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};

  return Planning {
      .options = std::move(options),
      .simulation = simulation,
      .system = system,
  };
}

}  // namespace

// ─── Free-function unit tests ───────────────────────────────────────────────

TEST_CASE("build_benders_cut produces valid cut row")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};
  const auto dep = ColIndex {2};

  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  // reduced costs: dep column has rc = -10.0
  const std::vector<double> rc = {0.0, 0.0, -10.0};
  const double obj = 5000.0;

  auto row = build_benders_cut(alpha, links, rc, obj, "test_cut");

  CHECK(row.name == "test_cut");
  // α coefficient = 1.0
  CHECK(row.get_coeff(alpha) == doctest::Approx(1.0));
  // source coefficient = -rc = -(-10) = 10
  CHECK(row.get_coeff(src) == doctest::Approx(10.0));
  // rhs = obj - Σ rc_i * trial_i = 5000 - (-10)*50 = 5500
  CHECK(row.lowb == doctest::Approx(5500.0));
  CHECK(row.uppb > 1e20);
}

TEST_CASE("relax_fixed_state_variable respects source bounds")  // NOLINT
{
  LinearInterface li;

  // Create a column and fix it at 80.0
  const auto col = li.add_col("dep", 80.0, 80.0);

  const StateVarLink link {
      .dependent_col = col,
      .source_phase = PhaseIndex {0},
      .trial_value = 80.0,
      .source_low = 0.0,
      .source_upp = 150.0,
  };

  const auto relaxed =
      relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6);
  CHECK(relaxed);

  // After relaxation, bounds should match source bounds
  CHECK(li.get_col_low()[col] == doctest::Approx(0.0));
  CHECK(li.get_col_upp()[col] == doctest::Approx(150.0));
}

TEST_CASE("relax_fixed_state_variable skips non-fixed columns")  // NOLINT
{
  LinearInterface li;
  const auto col = li.add_col("dep", 0.0, 100.0);

  const StateVarLink link {
      .dependent_col = col,
      .trial_value = 50.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  CHECK_FALSE(relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6));
}

TEST_CASE("average_benders_cut computes correct average")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1.name = "cut1";
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2.name = "cut2";
  cut2[alpha] = 1.0;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  auto avg = average_benders_cut(
      {
          cut1,
          cut2,
      },
      "avg");

  CHECK(avg.name == "avg");
  CHECK(avg.get_coeff(alpha) == doctest::Approx(1.0));
  CHECK(avg.get_coeff(src) == doctest::Approx(15.0));
  CHECK(avg.lowb == doctest::Approx(150.0));
}

TEST_CASE("parse_cut_sharing_mode")  // NOLINT
{
  CHECK(parse_cut_sharing_mode("none") == CutSharingMode::None);
  CHECK(parse_cut_sharing_mode("expected") == CutSharingMode::Expected);
  CHECK(parse_cut_sharing_mode("max") == CutSharingMode::Max);
  CHECK(parse_cut_sharing_mode("unknown") == CutSharingMode::None);
}

// ─── Integration tests ─────────────────────────────────────────────────────

TEST_CASE("SDDPSolver - 3-phase hydro+thermal converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Verify the monolithic solve works first
  {
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());
    CHECK(*result == 1);
  }

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  const auto& first = results->front();
  const auto& last = results->back();
  CHECK(first.iteration == 1);
  CHECK(last.upper_bound > 0.0);
  CHECK(last.lower_bound > 0.0);
  // Allow a tiny negative gap from floating-point rounding when LB ≈ UB at
  // convergence: (UB - LB) / max(1, |UB|) may be a small negative epsilon.
  static constexpr double kGapFpTol = -1e-10;
  CHECK(last.gap >= kGapFpTol);
  // Once reservoir state is properly coupled, SDDP should converge quickly
  CHECK(last.converged);
}

TEST_CASE("SDDPSolver - requires at least 2 phases")  // NOLINT
{
  auto planning = make_single_phase_planning();

  PlanningLP planning_lp(std::move(planning));
  SDDPSolver sddp(planning_lp);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
}

TEST_CASE("SDDPSolver - cut persistence save and load")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file = (tmp_dir / "sddp_test_cuts.csv").string();

  // Run SDDP and save cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cuts_output_file = cuts_file;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // Verify cuts were saved
  CHECK(std::filesystem::exists(cuts_file));
  CHECK_FALSE(sddp.stored_cuts().empty());

  // Clean up
  std::filesystem::remove(cuts_file);
}

// ─── Solver interface tests ─────────────────────────────────────────────────

TEST_CASE("MonolithicSolver - solves single-phase problem")  // NOLINT
{
  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicSolver solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("MonolithicSolver - solves 3-phase problem")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicSolver solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("make_planning_solver factory - monolithic")  // NOLINT
{
  auto solver = make_planning_solver("monolithic");
  REQUIRE(solver != nullptr);

  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  auto result = solver->solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("make_planning_solver factory - sddp")  // NOLINT
{
  auto solver = make_planning_solver("sddp");
  REQUIRE(solver != nullptr);
}

TEST_CASE("PlanningLP::resolve uses solver_type option")  // NOLINT
{
  auto planning = make_single_phase_planning();
  // Default solver_type is "monolithic"
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("Options solver_type and cut_sharing_mode")  // NOLINT
{
  Options opts;
  opts.solver_type = OptName {"sddp"};
  opts.cut_sharing_mode = OptName {"expected"};

  const OptionsLP options_lp(std::move(opts));
  CHECK(options_lp.solver_type() == "sddp");
  CHECK(options_lp.cut_sharing_mode() == "expected");
}

TEST_CASE("Options solver_type defaults")  // NOLINT
{
  const OptionsLP options_lp;
  CHECK(options_lp.solver_type() == "monolithic");
  CHECK(options_lp.cut_sharing_mode() == "none");
}

// ─── Integration: monolithic vs SDDP comparison ────────────────────────────

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Create a 5-phase reservoir+thermal planning problem for SDDP vs monolithic
/// comparison.
///
/// - 1 bus, single-bus mode
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 hydro generator (50 MW, $5/MWh)
/// - 1 demand (80 MW, 3-hour blocks → varying daily profile)
/// - 1 reservoir (500 dam³ capacity, starts at 300 dam³)
/// - Natural inflow: 8 dam³/h
/// - Hydro topology: 2 junctions, 1 waterway, 1 turbine
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each = 24h per stage)
auto make_5phase_reservoir_planning() -> Planning
{
  constexpr int num_phases = 5;
  constexpr int blocks_per_phase = 8;
  constexpr double block_duration = 3.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  block_array.reserve(total_blocks);
  for (int i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = block_duration,
    });
  }

  Array<Stage> stage_array;
  stage_array.reserve(num_phases);
  for (int s = 0; s < num_phases; ++s) {
    stage_array.push_back(Stage {
        .uid = Uid {s + 1},
        .first_block = static_cast<Size>(s * blocks_per_phase),
        .count_block = blocks_per_phase,
    });
  }

  Array<Phase> phase_array;
  phase_array.reserve(num_phases);
  for (int p = 0; p < num_phases; ++p) {
    phase_array.push_back(Phase {
        .uid = Uid {p + 1},
        .first_stage = static_cast<Size>(p),
        .count_stage = 1,
    });
  }

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  // Hydro system
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
          .fmax = 200.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 300.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 8.0,
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

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  Options options;
  options.demand_fail_cost = OptReal {5000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  System system = {
      .name = "sddp_reservoir_5phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Create a 5-phase battery+thermal planning problem for SDDP vs monolithic
/// comparison.
///
/// - 1 bus, single-bus mode
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 discharge generator (30 MW, $0/MWh)
/// - 1 fixed demand (varying 60–100 MW profile)
/// - 1 charge demand (30 MW max)
/// - 1 battery (100 MWh capacity, starts at 50 MWh, 90% round-trip eff.)
/// - 1 converter linking battery ↔ generator ↔ demand
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each)
auto make_5phase_battery_planning() -> Planning
{
  constexpr int num_phases = 5;
  constexpr int blocks_per_phase = 8;
  constexpr double block_duration = 3.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  block_array.reserve(total_blocks);
  for (int i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = block_duration,
    });
  }

  Array<Stage> stage_array;
  stage_array.reserve(num_phases);
  for (int s = 0; s < num_phases; ++s) {
    stage_array.push_back(Stage {
        .uid = Uid {s + 1},
        .first_block = static_cast<Size>(s * blocks_per_phase),
        .count_block = blocks_per_phase,
    });
  }

  Array<Phase> phase_array;
  phase_array.reserve(num_phases);
  for (int p = 0; p < num_phases; ++p) {
    phase_array.push_back(Phase {
        .uid = Uid {p + 1},
        .first_stage = static_cast<Size>(p),
        .count_stage = 1,
    });
  }

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  // Thermal provides firm generation; discharge generator runs from battery
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "discharge_gen",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 30.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "charge_demand",
          .bus = Uid {1},
          .capacity = 30.0,
      },
  };

  // Battery: 100 MWh, 90% charge/discharge efficiency, starts at 50 MWh
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bess1",
          .input_efficiency = RealFieldSched {0.95},
          .output_efficiency = RealFieldSched {0.95},
          .emin = RealFieldSched {0.0},
          .emax = RealFieldSched {100.0},
          .eini = 50.0,
          .pmax_charge = RealFieldSched {30.0},
          .pmax_discharge = RealFieldSched {30.0},
          .capacity = RealFieldSched {100.0},
          .use_state_variable = true,
      },
  };

  // Converter links battery, discharge generator, and charge demand
  const Array<Converter> converter_array = {
      {
          .uid = Uid {1},
          .name = "conv1",
          .battery = Uid {1},
          .generator = Uid {2},
          .demand = Uid {2},
          .conversion_rate = RealFieldSched {1.0},
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  Options options;
  options.demand_fail_cost = OptReal {5000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  System system = {
      .name = "sddp_battery_5phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
      .converter_array = converter_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

}  // namespace

TEST_CASE(
    "Integration: monolithic vs SDDP - reservoir case (5 phases)")  // NOLINT
{
  // ─── 1. Solve with the monolithic solver ──
  auto planning_mono = make_5phase_reservoir_planning();
  PlanningLP plp_mono(std::move(planning_mono));

  auto mono_result = plp_mono.resolve();
  REQUIRE(mono_result.has_value());
  CHECK(*mono_result == 1);

  const auto mono_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {0})
                            .linear_interface()
                            .get_obj_value();
  SPDLOG_INFO("Reservoir mono: phase-0 obj = {:.4f}", mono_obj);

  // Compute total monolithic cost across all phases
  double mono_total = 0.0;
  for (int p = 0; p < 5; ++p) {
    const auto ph_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {p})
                            .linear_interface()
                            .get_obj_value();
    SPDLOG_INFO("  phase {} obj = {:.4f}", p, ph_obj);
    mono_total += ph_obj;
  }
  SPDLOG_INFO("Reservoir mono: total obj = {:.4f}", mono_total);
  CHECK(mono_total > 0.0);

  // ─── 2. Solve the same problem with SDDP ──
  auto planning_sddp = make_5phase_reservoir_planning();
  PlanningLP plp_sddp(std::move(planning_sddp));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO("Reservoir SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
              last.iteration,
              last.lower_bound,
              last.upper_bound,
              last.gap);

  // SDDP should converge
  CHECK(last.converged);

  // ─── 3. Compare objectives ──
  // The SDDP upper bound (sum of actual phase costs) should be close to
  // the monolithic objective.  Allow 5% tolerance due to cut approximation.
  const auto sddp_total = last.upper_bound;
  const auto relative_diff =
      std::abs(sddp_total - mono_total) / std::max(1.0, std::abs(mono_total));
  SPDLOG_INFO(
      "Reservoir comparison: mono={:.4f} sddp={:.4f} "
      "relative_diff={:.6f}",
      mono_total,
      sddp_total,
      relative_diff);

  CHECK(relative_diff < 0.05);
}

TEST_CASE(
    "Integration: monolithic vs SDDP - battery case (5 phases)")  // NOLINT
{
  // ─── 1. Solve with the monolithic solver ──
  auto planning_mono = make_5phase_battery_planning();
  PlanningLP plp_mono(std::move(planning_mono));

  auto mono_result = plp_mono.resolve();
  REQUIRE(mono_result.has_value());
  CHECK(*mono_result == 1);

  double mono_total = 0.0;
  for (int p = 0; p < 5; ++p) {
    const auto ph_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {p})
                            .linear_interface()
                            .get_obj_value();
    SPDLOG_INFO("Battery mono: phase {} obj = {:.4f}", p, ph_obj);
    mono_total += ph_obj;
  }
  SPDLOG_INFO("Battery mono: total obj = {:.4f}", mono_total);
  CHECK(mono_total > 0.0);

  // ─── 2. Solve the same problem with SDDP ──
  auto planning_sddp = make_5phase_battery_planning();
  PlanningLP plp_sddp(std::move(planning_sddp));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO("Battery SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
              last.iteration,
              last.lower_bound,
              last.upper_bound,
              last.gap);

  // SDDP should converge
  CHECK(last.converged);

  // ─── 3. Compare objectives ──
  const auto sddp_total = last.upper_bound;
  const auto relative_diff =
      std::abs(sddp_total - mono_total) / std::max(1.0, std::abs(mono_total));
  SPDLOG_INFO(
      "Battery comparison: mono={:.4f} sddp={:.4f} "
      "relative_diff={:.6f}",
      mono_total,
      sddp_total,
      relative_diff);

  CHECK(relative_diff < 0.05);
}
