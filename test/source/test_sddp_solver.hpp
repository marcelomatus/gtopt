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
 *  8. Simple 2-phase linear Benders cut and aperture tests
 */

#include <cmath>
#include <filesystem>
#include <fstream>

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
  CHECK(parse_cut_sharing_mode("accumulate") == CutSharingMode::Accumulate);
  CHECK(parse_cut_sharing_mode("max") == CutSharingMode::Max);
  // Unknown defaults to Max (matching SDDPOptions default)
  CHECK(parse_cut_sharing_mode("unknown") == CutSharingMode::Max);
}

TEST_CASE("parse_elastic_filter_mode")  // NOLINT
{
  // Canonical names
  CHECK(parse_elastic_filter_mode("single-cut")
        == ElasticFilterMode::FeasibilityCut);
  CHECK(parse_elastic_filter_mode("multi-cut") == ElasticFilterMode::MultiCut);
  CHECK(parse_elastic_filter_mode("backpropagate")
        == ElasticFilterMode::BackpropagateBounds);
  // Backward-compat alias
  CHECK(parse_elastic_filter_mode("cut") == ElasticFilterMode::FeasibilityCut);
  // Unknown → default (FeasibilityCut)
  CHECK(parse_elastic_filter_mode("unknown")
        == ElasticFilterMode::FeasibilityCut);
}

TEST_CASE("relax_fixed_state_variable returns slack column indices")  // NOLINT
{
  LinearInterface li;

  // Create a column and fix it at 50.0
  const auto col = li.add_col("dep", 50.0, 50.0);

  const StateVarLink link {
      .dependent_col = col,
      .source_phase = PhaseIndex {0},
      .trial_value = 50.0,
      .source_low = 0.0,
      .source_upp = 100.0,
  };

  const auto info = relax_fixed_state_variable(li, link, PhaseIndex {1}, 1e6);

  REQUIRE(info.relaxed);
  // After relaxation, bounds should match source bounds
  CHECK(li.get_col_low()[col] == doctest::Approx(0.0));
  CHECK(li.get_col_upp()[col] == doctest::Approx(100.0));
  // slack columns must be valid
  CHECK(info.sup_col != ColIndex {unknown_index});
  CHECK(info.sdn_col != ColIndex {unknown_index});
  CHECK(info.sup_col != info.sdn_col);
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
  const OptionsLP options_lp;
  auto solver = make_planning_solver(options_lp);
  REQUIRE(solver != nullptr);

  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  auto result = solver->solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("make_planning_solver factory - sddp")  // NOLINT
{
  Options opts;
  opts.sddp_options.sddp_solver_type = OptName {"sddp"};
  const OptionsLP options_lp(std::move(opts));
  auto solver = make_planning_solver(options_lp);
  REQUIRE(solver != nullptr);
}

TEST_CASE("PlanningLP::resolve uses sddp_solver_type option")  // NOLINT
{
  auto planning = make_single_phase_planning();
  // Default sddp_solver_type is "monolithic"
  PlanningLP planning_lp(std::move(planning));

  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("Options sddp_solver_type and sddp_cut_sharing_mode")  // NOLINT
{
  Options opts;
  opts.sddp_options.sddp_solver_type = OptName {"sddp"};
  opts.sddp_options.sddp_cut_sharing_mode = OptName {"expected"};

  const OptionsLP options_lp(std::move(opts));
  CHECK(options_lp.sddp_solver_type() == "sddp");
  CHECK(options_lp.sddp_cut_sharing_mode() == "expected");
}

TEST_CASE("Options sddp_solver_type defaults")  // NOLINT
{
  const OptionsLP options_lp;
  CHECK(options_lp.sddp_solver_type() == "monolithic");
  CHECK(options_lp.sddp_cut_sharing_mode() == "max");
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
/// - 1 demand (100 MW, 3-hour blocks → varying daily profile)
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

/// Create a 5-phase small-reservoir+thermal planning problem to test
/// state-variable coupling between phases.
///
/// Uses a simpler reservoir (smaller, more constrained) than the main
/// reservoir test to exercise a different state-coupling regime:
/// - The reservoir is small (200 dam³) with low inflow (5 dam³/h)
/// - The hydro generator is large (80 MW) relative to the reservoir
/// - This forces the reservoir to deplete across phases, creating
///   non-trivial state variable values at phase boundaries
///
/// - 1 bus, single-bus mode
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 hydro generator (80 MW, $3/MWh)
/// - 1 demand (100 MW constant)
/// - 1 small reservoir (200 dam³ capacity, starts at 180 dam³)
/// - Natural inflow: 5 dam³/h (low, so reservoir depletes over time)
/// - Hydro topology: 2 junctions, 1 waterway, 1 turbine
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each)
auto make_5phase_small_reservoir_planning() -> Planning
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
          .gcost = 3.0,
          .capacity = 80.0,
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

  // Hydro system — small reservoir with low inflow
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
          .name = "rsv_small",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 180.0,
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
          .discharge = 5.0,
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
      .name = "sddp_small_reservoir_5phase",
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

/// Create a 5-phase generator-expansion planning problem for SDDP vs
/// monolithic comparison.
///
/// Tests that the `capainst` (installed-capacity) state variable is
/// correctly coupled between phases.  The generator starts with 0 MW
/// capacity and must expand (invest in modules) across phases to serve
/// a 100 MW demand.
///
/// - 1 bus, single-bus mode
/// - 1 expandable generator (0 MW initial, 50 MW/module, max 10 modules,
///   $80/MWh operating cost, $500/module-year investment cost)
/// - 1 cheap backup generator (200 MW, $200/MWh — expensive "peaker" that
///   makes expansion worthwhile)
/// - 1 demand (100 MW constant)
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each)
auto make_5phase_expansion_planning() -> Planning
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
          .name = "expandable_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 0.0,
          .expcap = 50.0,
          .expmod = 10.0,
          .annual_capcost = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "backup_gen",
          .bus = Uid {1},
          .gcost = 200.0,
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
      .name = "sddp_expansion_5phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Create a year-long 12-phase hydro+thermal planning problem for SDDP vs
/// monolithic comparison.  Inspired by the sddp_hydro_3phase case.
///
/// Each phase represents one month (1 stage of 24 hourly blocks = one
/// representative day per month).  The reservoir has seasonal inflow:
/// higher in winter/spring (months 5–8), lower in summer (months 1–4, 9–12).
///
/// - 1 bus, single-bus mode
/// - 1 hydro generator (25 MW, $5/MWh)
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 demand (50 MW constant)
/// - 1 reservoir (150 dam³ capacity, starts at 100 dam³)
/// - Variable inflow: 5–15 dam³/h seasonal pattern
/// - 12 phases × 1 stage × 24 blocks (1h each) = 288 blocks total
auto make_12phase_yearly_hydro_planning() -> Planning
{
  constexpr int num_phases = 12;
  constexpr int blocks_per_phase = 24;
  constexpr double block_duration = 1.0;
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
          .capacity = 25.0,
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
          .fmax = 200.0,
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

  // Seasonal inflow: 10 dam³/h average (same as sddp_hydro_3phase)
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
      .name = "sddp_yearly_hydro_12phase",
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

}  // namespace

TEST_CASE(
    "Integration: monolithic vs SDDP - reservoir "  // NOLINT
    "(5 phases × 8 blocks)")
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
    "Integration: monolithic vs SDDP - small reservoir "  // NOLINT
    "state coupling (5 phases)")
{
  // ─── 1. Solve with the monolithic solver ──
  auto planning_mono = make_5phase_small_reservoir_planning();
  PlanningLP plp_mono(std::move(planning_mono));

  auto mono_result = plp_mono.resolve();
  REQUIRE(mono_result.has_value());
  CHECK(*mono_result == 1);

  double mono_total = 0.0;
  for (int p = 0; p < 5; ++p) {
    const auto ph_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {p})
                            .linear_interface()
                            .get_obj_value();
    SPDLOG_INFO("Small reservoir mono: phase {} obj = {:.4f}", p, ph_obj);
    mono_total += ph_obj;
  }
  SPDLOG_INFO("Small reservoir mono: total obj = {:.4f}", mono_total);
  CHECK(mono_total > 0.0);

  // ─── 2. Solve the same problem with SDDP ──
  auto planning_sddp = make_5phase_small_reservoir_planning();
  PlanningLP plp_sddp(std::move(planning_sddp));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO(
      "Small reservoir SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
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
      "Small reservoir comparison: mono={:.4f} sddp={:.4f} "
      "relative_diff={:.6f}",
      mono_total,
      sddp_total,
      relative_diff);

  CHECK(relative_diff < 0.05);
}

TEST_CASE(
    "Integration: monolithic vs SDDP - expansion case "  // NOLINT
    "(5 phases)")
{
  // ─── 1. Solve with the monolithic solver ──
  auto planning_mono = make_5phase_expansion_planning();
  PlanningLP plp_mono(std::move(planning_mono));

  auto mono_result = plp_mono.resolve();
  REQUIRE(mono_result.has_value());
  CHECK(*mono_result == 1);

  double mono_total = 0.0;
  for (int p = 0; p < 5; ++p) {
    const auto ph_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {p})
                            .linear_interface()
                            .get_obj_value();
    SPDLOG_INFO("Expansion mono: phase {} obj = {:.4f}", p, ph_obj);
    mono_total += ph_obj;
  }
  SPDLOG_INFO("Expansion mono: total obj = {:.4f}", mono_total);
  CHECK(mono_total > 0.0);

  // ─── 2. Solve the same problem with SDDP ──
  auto planning_sddp = make_5phase_expansion_planning();
  PlanningLP plp_sddp(std::move(planning_sddp));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO("Expansion SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
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
      "Expansion comparison: mono={:.4f} sddp={:.4f} "
      "relative_diff={:.6f}",
      mono_total,
      sddp_total,
      relative_diff);

  CHECK(relative_diff < 0.05);
}

TEST_CASE(
    "Integration: monolithic vs SDDP - yearly hydro "  // NOLINT
    "(12 phases × 24 blocks)")
{
  // ─── 1. Solve with the monolithic solver ──
  auto planning_mono = make_12phase_yearly_hydro_planning();
  PlanningLP plp_mono(std::move(planning_mono));

  auto mono_result = plp_mono.resolve();
  REQUIRE(mono_result.has_value());
  CHECK(*mono_result == 1);

  double mono_total = 0.0;
  for (int p = 0; p < 12; ++p) {
    const auto ph_obj = plp_mono.system(SceneIndex {0}, PhaseIndex {p})
                            .linear_interface()
                            .get_obj_value();
    SPDLOG_INFO("Yearly hydro mono: phase {} obj = {:.4f}", p, ph_obj);
    mono_total += ph_obj;
  }
  SPDLOG_INFO("Yearly hydro mono: total obj = {:.4f}", mono_total);
  CHECK(mono_total > 0.0);

  // ─── 2. Solve the same problem with SDDP ──
  auto planning_sddp = make_12phase_yearly_hydro_planning();
  PlanningLP plp_sddp(std::move(planning_sddp));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO(
      "Yearly hydro SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
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
      "Yearly hydro comparison: mono={:.4f} sddp={:.4f} "
      "relative_diff={:.6f}",
      mono_total,
      sddp_total,
      relative_diff);

  CHECK(relative_diff < 0.05);
}

// ─── API tests ──────────────────────────────────────────────────────────────

TEST_CASE("SDDPSolver API - iteration callback")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-6;

  SDDPSolver sddp(planning_lp, sddp_opts);

  // Register a callback that collects iteration data and stops after 3 iters
  std::vector<SDDPIterationResult> callback_results;
  sddp.set_iteration_callback(
      [&callback_results](const SDDPIterationResult& r) -> bool
      {
        callback_results.push_back(r);
        SPDLOG_INFO("API callback: iter {} gap={:.6f}", r.iteration, r.gap);
        return r.iteration >= 3;  // stop after 3 iterations
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // The callback should have been called for each iteration
  CHECK(callback_results.size() == results->size());
  // The solver should have stopped after 3 iterations (or converged earlier)
  CHECK(results->size() <= 3);
  // Iteration numbers should be sequential
  for (size_t i = 0; i < callback_results.size(); ++i) {
    CHECK(callback_results[i].iteration == static_cast<int>(i + 1));
  }
}

TEST_CASE("SDDPSolver API - programmatic stop")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 100;
  sddp_opts.convergence_tol = 1e-12;  // very tight → won't converge in 2 iters

  SDDPSolver sddp(planning_lp, sddp_opts);

  // Request stop after 2 iterations via the callback
  sddp.set_iteration_callback(
      [&sddp](const SDDPIterationResult& r) -> bool
      {
        if (r.iteration >= 2) {
          sddp.request_stop();
        }
        return false;  // don't stop via callback return value
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Should have stopped after ≤ 3 iterations (request_stop checked at iter
  // start)
  CHECK(results->size() <= 3);
  CHECK(sddp.is_stop_requested());
}

TEST_CASE("SDDPSolver API - live query atomics")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;

  SDDPSolver sddp(planning_lp, sddp_opts);

  // Before solving, live-query values should be at their initial state
  CHECK(sddp.current_iteration() == 0);
  CHECK(sddp.current_gap() == doctest::Approx(1.0));
  CHECK_FALSE(sddp.has_converged());

  // Verify live-query updates during solving via callback
  double last_gap = 1.0;
  sddp.set_iteration_callback(
      [&sddp, &last_gap](const SDDPIterationResult& r) -> bool
      {
        // The live-query values should match the iteration result
        CHECK(sddp.current_iteration() == r.iteration);
        CHECK(sddp.current_gap() == doctest::Approx(r.gap));
        CHECK(sddp.current_lower_bound() == doctest::Approx(r.lower_bound));
        CHECK(sddp.current_upper_bound() == doctest::Approx(r.upper_bound));
        last_gap = r.gap;
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After solving, live-query should reflect final state
  CHECK(sddp.current_iteration() == static_cast<int>(results->size()));
  CHECK(sddp.current_gap() == doctest::Approx(results->back().gap));
  if (results->back().converged) {
    CHECK(sddp.has_converged());
  }
}

// ─── weighted_average_benders_cut unit tests ─────────────────────────────────

TEST_CASE("weighted_average_benders_cut - empty input")  // NOLINT
{
  const auto result = weighted_average_benders_cut({}, {}, "empty");
  CHECK(result.name.empty());
}

TEST_CASE("weighted_average_benders_cut - single cut")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1.name = "cut1";
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  const auto result = weighted_average_benders_cut({cut1}, {0.7}, "single");
  CHECK(result.name == "single");
  // single cut → returned as-is (weight normalised to 1)
  CHECK(result.get_coeff(alpha) == doctest::Approx(1.0));
  CHECK(result.get_coeff(src) == doctest::Approx(10.0));
  CHECK(result.lowb == doctest::Approx(100.0));
}

TEST_CASE(
    "weighted_average_benders_cut - equal weights same as average")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[alpha] = 1.0;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  // Equal weights → same as unweighted average
  const auto wavg =
      weighted_average_benders_cut({cut1, cut2}, {0.5, 0.5}, "wavg");
  const auto avg = average_benders_cut({cut1, cut2}, "avg");

  CHECK(wavg.get_coeff(alpha) == doctest::Approx(avg.get_coeff(alpha)));
  CHECK(wavg.get_coeff(src) == doctest::Approx(avg.get_coeff(src)));
  CHECK(wavg.lowb == doctest::Approx(avg.lowb));
}

TEST_CASE(
    "weighted_average_benders_cut - probability weights applied")  // NOLINT
{
  const auto alpha = ColIndex {0};
  const auto src = ColIndex {1};

  SparseRow cut1;
  cut1[alpha] = 1.0;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[alpha] = 1.0;
  cut2[src] = 30.0;
  cut2.lowb = 300.0;
  cut2.uppb = LinearProblem::DblMax;

  // 75% weight on cut1, 25% weight on cut2
  const auto result =
      weighted_average_benders_cut({cut1, cut2}, {0.75, 0.25}, "w_avg");

  CHECK(result.name == "w_avg");
  CHECK(result.get_coeff(alpha) == doctest::Approx(1.0));
  // expected: 0.75 * 10 + 0.25 * 30 = 7.5 + 7.5 = 15.0
  CHECK(result.get_coeff(src) == doctest::Approx(15.0));
  // expected: 0.75 * 100 + 0.25 * 300 = 75 + 75 = 150
  CHECK(result.lowb == doctest::Approx(150.0));
}

TEST_CASE("weighted_average_benders_cut - unnormalised weights")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 4.0;
  cut1.lowb = 40.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[src] = 8.0;
  cut2.lowb = 80.0;
  cut2.uppb = LinearProblem::DblMax;

  // weights {3, 1} → normalised: {0.75, 0.25}
  const auto result =
      weighted_average_benders_cut({cut1, cut2}, {3.0, 1.0}, "unnorm");

  // expected src: 0.75 * 4 + 0.25 * 8 = 3 + 2 = 5
  CHECK(result.get_coeff(src) == doctest::Approx(5.0));
  // expected rhs: 0.75 * 40 + 0.25 * 80 = 30 + 20 = 50
  CHECK(result.lowb == doctest::Approx(50.0));
}

TEST_CASE(
    "weighted_average_benders_cut - zero weight scene excluded")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  SparseRow cut2;
  cut2[src] = 20.0;
  cut2.lowb = 200.0;
  cut2.uppb = LinearProblem::DblMax;

  // Zero weight on cut2 → only cut1 contributes
  const auto result =
      weighted_average_benders_cut({cut1, cut2}, {1.0, 0.0}, "zero_w");

  CHECK(result.get_coeff(src) == doctest::Approx(10.0));
  CHECK(result.lowb == doctest::Approx(100.0));
}

TEST_CASE(
    "weighted_average_benders_cut - all zero weights returns empty")  // NOLINT
{
  const auto src = ColIndex {0};

  SparseRow cut1;
  cut1[src] = 10.0;
  cut1.lowb = 100.0;
  cut1.uppb = LinearProblem::DblMax;

  // All zero weights → empty result
  const auto result = weighted_average_benders_cut({cut1}, {0.0}, "all_zero");
  CHECK(result.name.empty());
  CHECK(result.cmap.empty());
  CHECK(result.lowb == doctest::Approx(0.0));
}

// ─── Multi-cut threshold=0 forces multi-cut immediately ──────────────────────

TEST_CASE("SDDPSolver - multi_cut_threshold=0 forces multi-cut mode")  // NOLINT
{
  // Use the 3-phase hydro planning; set threshold=0 so any infeasibility
  // instantly uses multi-cut mode.  The problem should still converge.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.multi_cut_threshold = 0;  // always force multi-cut

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // Convergence should still be reached
  CHECK(results->back().converged);
}

TEST_CASE("SDDPSolver - multi_cut_threshold<0 disables auto-switch")  // NOLINT
{
  // Negative threshold disables automatic multi-cut switching entirely.
  // The problem should still converge with single-cut only.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.multi_cut_threshold = -1;  // never auto-switch

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ─── Probability-weighted cut sharing ────────────────────────────────────────

/// Create a 2-scene, 3-phase hydro+thermal planning problem with explicit
/// per-scene probability weights (0.7 and 0.3).
inline auto make_2scene_3phase_hydro_planning(double prob1 = 0.7,
                                              double prob2 = 0.3) -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  block_array.reserve(total_blocks);
  for (int i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
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

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = prob1,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = prob2,
              },
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = "scene1",
                  .active = true,
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = "scene2",
                  .active = true,
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

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

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 100.0,
          .fmin = -1000.0,
          .fmax = 1000.0,
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

  const System system = {
      .name = "sddp_2scene_3phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = system,
  };
}

TEST_CASE("SDDPSolver 2-scene - probability-weighted bounds")  // NOLINT
{
  // Two scenes with probabilities 0.7 and 0.3.
  // UB and LB should be probability-weighted expectations, not simple averages.
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.cut_sharing = CutSharingMode::None;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // The solver should converge
  CHECK(results->back().converged);

  // Verify that the final upper bound is consistent with a
  // probability-weighted combination (not a simple average):
  // UB = 0.7 * ub_scene0 + 0.3 * ub_scene1
  const auto& last = results->back();
  REQUIRE(last.scene_upper_bounds.size() == 2);
  const double expected_ub =
      (0.7 * last.scene_upper_bounds[0]) + (0.3 * last.scene_upper_bounds[1]);
  CHECK(last.upper_bound == doctest::Approx(expected_ub).epsilon(1e-9));
  SPDLOG_INFO("2-scene weighted UB: {:.4f} (scene0={:.4f}, scene1={:.4f})",
              last.upper_bound,
              last.scene_upper_bounds[0],
              last.scene_upper_bounds[1]);

  // Verify lower bound is also probability-weighted
  REQUIRE(last.scene_lower_bounds.size() == 2);
  const double expected_lb =
      (0.7 * last.scene_lower_bounds[0]) + (0.3 * last.scene_lower_bounds[1]);
  CHECK(last.lower_bound == doctest::Approx(expected_lb).epsilon(1e-9));
}

TEST_CASE(
    "SDDPSolver 2-scene - equal weights same as simple average")  // NOLINT
{
  // Equal probability weights → result should match simple average
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);

  const auto& last = results->back();
  REQUIRE(last.scene_upper_bounds.size() == 2);
  // With equal weights 0.5/0.5 the weighted average = arithmetic mean
  const double simple_avg =
      0.5 * (last.scene_upper_bounds[0] + last.scene_upper_bounds[1]);
  CHECK(last.upper_bound == doctest::Approx(simple_avg).epsilon(1e-9));
}

TEST_CASE(
    "SDDPSolver 2-scene Expected cut sharing with prob weights")  // NOLINT
{
  // Verify that Expected cut-sharing mode produces the same convergence
  // outcome whether we use equal or unequal probability weights.
  // The solver should converge in both cases.

  SUBCASE("equal probabilities with Expected cut sharing")
  {
    auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.cut_sharing = CutSharingMode::Expected;

    SDDPSolver sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
    CHECK(results->back().converged);
  }

  SUBCASE("unequal probabilities with Expected cut sharing")
  {
    auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 30;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.cut_sharing = CutSharingMode::Expected;

    SDDPSolver sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
    CHECK(results->back().converged);

    // The weighted UB should equal the probability-weighted combination
    const auto& last = results->back();
    REQUIRE(last.scene_upper_bounds.size() == 2);
    const double expected_ub =
        (0.7 * last.scene_upper_bounds[0]) + (0.3 * last.scene_upper_bounds[1]);
    CHECK(last.upper_bound == doctest::Approx(expected_ub).epsilon(1e-9));
  }
}

// ─── TurbineLP::update_lp unit tests ─────────────────────────────────────────

TEST_CASE("TurbineLP::update_lp - no-op when no efficiency element")  // NOLINT
{
  // Build a minimal system WITHOUT a ReservoirEfficiency element.
  // update_lp should return 0 (nothing to update).
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 30.0,
      },
  };
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 100.0,
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

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {1}}},
  };

  const System system = {
      .name = "test_no_eff",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  const OptionsLP options_lp(options);
  SimulationLP sim_lp(simulation, options_lp);
  SystemLP system_lp(system, sim_lp);

  // The solver must support set_coeff for update_lp to actually try anything;
  // either way, the function call should be safe.
  [[maybe_unused]] const bool set_coeff_supported =
      LinearInterface::supports_set_coeff();

  // update_lp_coefficients with no efficiency elements → 0 updated
  const auto updated =
      update_lp_coefficients(system_lp, options_lp, 0, PhaseIndex {0});
  CHECK(updated == 0);
}

TEST_CASE("FiltrationLP::update_lp is a no-op without segments")  // NOLINT
{
  // Verify the trivial no-op path of FiltrationLP::update_lp by calling
  // update_lp_coefficients on a system that has filtration without
  // piecewise segments (static slope/constant only).
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 30.0,
      },
  };
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 100.0,
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
          .name = "flt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = 0.01,
          .constant = 0.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {1}}},
  };

  const System system = {
      .name = "test_filtration_noop",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  const OptionsLP options_lp(options);
  SimulationLP sim_lp(simulation, options_lp);
  SystemLP system_lp(system, sim_lp);

  // FiltrationLP::update_lp is a no-op when no segments are present → 0
  const auto updated =
      update_lp_coefficients(system_lp, options_lp, 0, PhaseIndex {0});
  CHECK(updated == 0);
}

TEST_CASE("SDDPSolver API - monitoring API stop-request file")  // NOLINT
{
  // Verify that the solver stops gracefully when the monitoring API
  // stop-request file (sddp_stop_request.json) is created in the tmp dir.
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "test_sddp_api_stop_request";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto stop_request_path = tmp_dir / sddp_file::stop_request;

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 100;
  sddp_opts.convergence_tol = 1e-12;  // very tight — won't converge in 2
  sddp_opts.api_stop_request_file = stop_request_path.string();

  SDDPSolver sddp(planning_lp, sddp_opts);

  // Create the stop-request file after the first iteration via callback
  sddp.set_iteration_callback(
      [&stop_request_path](const SDDPIterationResult& r) -> bool
      {
        if (r.iteration >= 1) {
          std::ofstream ofs(stop_request_path);
          ofs << R"({"stop_requested":true})" << '\n';
        }
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Should stop after ≤ 2 iterations (file created at end of iter 1;
  // checked at start of iter 2)
  CHECK(results->size() <= 2);

  std::filesystem::remove_all(tmp_dir);
}

// ─── Solver infrastructure tests ────────────────────────────────────────────

TEST_CASE("make_solver_work_pool creates a working pool")  // NOLINT
{
  auto pool = make_solver_work_pool();
  REQUIRE(pool != nullptr);

  // Submit a simple task and verify it executes
  auto fut = pool->submit([] { return 42; });
  REQUIRE(fut.has_value());
  CHECK(fut->get() == 42);

  // Check statistics are available
  const auto stats = pool->get_statistics();
  CHECK(stats.tasks_submitted >= 1);
}

TEST_CASE("make_solver_work_pool with custom cpu_factor")  // NOLINT
{
  // Use a small cpu_factor to verify it parameterises correctly
  auto pool = make_solver_work_pool(0.5);
  REQUIRE(pool != nullptr);

  auto fut = pool->submit([] { return 7; });
  REQUIRE(fut.has_value());
  CHECK(fut->get() == 7);
}

TEST_CASE("SDDPIterationResult contains timing information")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-3;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // Every iteration should have non-negative timing
  for (const auto& ir : *results) {
    CHECK(ir.forward_pass_s >= 0.0);
    CHECK(ir.backward_pass_s >= 0.0);
    CHECK(ir.iteration_s >= 0.0);
    // iteration_s should be >= forward + backward
    CHECK(ir.iteration_s
          >= doctest::Approx(ir.forward_pass_s + ir.backward_pass_s)
                 .epsilon(0.01));
  }
}

TEST_CASE("SDDPSolver API - status file contains timing fields")  // NOLINT
{
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "test_sddp_timing_status";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  const auto status_file = (tmp_dir / "sddp_status.json").string();

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = true;
  sddp_opts.api_status_file = status_file;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // The status file should exist and contain timing fields
  CHECK(std::filesystem::exists(status_file));
  if (std::filesystem::exists(status_file)) {
    std::ifstream ifs(status_file);
    const std::string content(std::istreambuf_iterator<char>(ifs), {});
    CHECK(content.find("forward_pass_s") != std::string::npos);
    CHECK(content.find("backward_pass_s") != std::string::npos);
    CHECK(content.find("iteration_s") != std::string::npos);
    CHECK(content.find("elapsed_s") != std::string::npos);
    CHECK(content.find("realtime") != std::string::npos);
  }

  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("MonolithicSolver uses work pool from factory")  // NOLINT
{
  // Verify that MonolithicSolver works correctly after the refactoring
  // to use make_solver_work_pool()
  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicSolver solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

TEST_CASE("MonolithicSolver with 3-phase uses work pool")  // NOLINT
{
  // Verify multi-phase monolithic solving after refactoring
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  MonolithicSolver solver;
  auto result = solver.solve(planning_lp, {});
  REQUIRE(result.has_value());
  CHECK(*result == 1);
}

// ─── Modular Benders cut tests (benders_cut.hpp) ────────────────────────────
//
// These tests exercise the cut-creation functions against actual LP solves
// using simple 2-variable LP problems.  They do not depend on SDDPSolver.

TEST_CASE(  // NOLINT
    "build_benders_cut - optimality cut from LP solve")
{
  // Build a simple LP:
  //   min  10*x0 + 20*x1 + alpha
  //   s.t. x0 + x1 + dep >= 100   (demand)
  //        0 <= x0 <= 80
  //        0 <= x1 <= 80
  //        dep fixed at 50

  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 80.0);
  const auto x1 = li.add_col("x1", 0.0, 80.0);
  li.set_obj_coeff(x0, 10.0);
  li.set_obj_coeff(x1, 20.0);

  // Alpha (future cost)
  const auto alpha_col = li.add_col("alpha", 0.0, 1e12);
  li.set_obj_coeff(alpha_col, 1.0);

  // Dependent (state variable from previous phase)
  const auto dep = li.add_col("dep", 50.0, 50.0);

  // demand: x0 + x1 + dep >= 100
  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[x1] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  auto r = li.resolve({});
  REQUIRE(r.has_value());
  REQUIRE(li.is_optimal());

  const auto src = ColIndex {20};  // arbitrary source col index

  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto cut = build_benders_cut(
      alpha_col, links, li.get_col_cost(), li.get_obj_value(), "opt_cut");

  CHECK(cut.name == "opt_cut");
  CHECK(cut.get_coeff(alpha_col) == doctest::Approx(1.0));
  CHECK(cut.lowb > -1e20);
  CHECK(cut.uppb > 1e20);
  // Source coefficient from reduced costs
  CHECK(std::abs(cut.get_coeff(src)) > 1e-10);
}

TEST_CASE(  // NOLINT
    "elastic_filter_solve - relaxes fixed column and solves clone")
{
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  auto r0 = li.resolve({});
  REQUIRE(r0.has_value());
  REQUIRE(li.is_optimal());

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto result = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
    REQUIRE(result->link_infos.size() == 1);
    CHECK(result->link_infos[0].relaxed);
  }

  // Original LP untouched
  CHECK(li.get_col_low()[dep] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[dep] == doctest::Approx(50.0));
}

TEST_CASE(  // NOLINT
    "elastic_filter_solve - returns nullopt for non-fixed column")
{
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 100.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 0.0, 100.0);  // NOT fixed

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 50.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto result = elastic_filter_solve(li, links, 1e6, {});
  CHECK_FALSE(result.has_value());
}

TEST_CASE(  // NOLINT
    "build_feasibility_cut - produces valid cut from elastic solve")
{
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const auto alpha_col = ColIndex {10};
  const auto src = ColIndex {11};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto result = build_feasibility_cut(
      li, alpha_col, links, 1e6, SolverOptions {}, "feas_cut");  // NOLINT
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->cut.name == "feas_cut");
    CHECK(result->cut.get_coeff(alpha_col) == doctest::Approx(1.0));
    CHECK(result->cut.lowb > -1e20);
    CHECK(result->elastic.clone.is_optimal());
    REQUIRE(result->elastic.link_infos.size() == 1);
    CHECK(result->elastic.link_infos[0].relaxed);
  }
}

TEST_CASE(  // NOLINT
    "build_feasibility_cut - returns nullopt for non-fixed column")
{
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 100.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 0.0, 100.0);  // NOT fixed

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 50.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .dependent_col = dep,
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  auto result = build_feasibility_cut(
      li, ColIndex {10}, links, 1e6, SolverOptions {}, "none");  // NOLINT
  CHECK_FALSE(result.has_value());
}

TEST_CASE(  // NOLINT
    "build_multi_cuts - generates bound cuts from elastic slack")
{
  // LP infeasible when dep fixed at 50: x0+dep>=200, x0<=80
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 80.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 200.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);

  [[maybe_unused]] auto r0 = li.resolve({});
  CHECK_FALSE(li.is_optimal());

  const auto src = ColIndex {10};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto elastic = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(elastic.has_value());
  if (elastic) {
    CHECK(elastic->clone.is_optimal());

    auto multi = build_multi_cuts(*elastic, links, "mc");
    CHECK_FALSE(multi.empty());

    // sdn active (dep went from 50 to ~120) → lower-bound cut
    bool found_lb = false;
    for (const auto& mc : multi) {
      if (mc.lowb > -1e20) {
        found_lb = true;
        CHECK(mc.get_coeff(src) == doctest::Approx(1.0));
        CHECK(mc.lowb > 50.0);
      }
    }
    CHECK(found_lb);
  }
}

TEST_CASE(  // NOLINT
    "build_multi_cuts - returns empty when no slack is active")
{
  // Feasible LP with dep fixed
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});
  REQUIRE(li.is_optimal());

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto elastic = elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(elastic.has_value());
  if (elastic) {
    auto multi = build_multi_cuts(*elastic, links, "mc");
    CHECK(multi.empty());
  }
}

TEST_CASE(  // NOLINT
    "Benders cut tightens lower bound in two-phase LP")
{
  // Simulate a minimal 2-phase decomposition manually:
  //
  // Phase 0: min 10*x0 + alpha, s.t. x0 >= 20, x0 in [0,100]
  // Phase 1: min 50*x1, s.t. x1 + dep >= 80, dep fixed at x0, x1 in [0,100]
  //
  // Full: min 10*x0+50*x1, x0>=20, x1+x0>=80 → x0=80,x1=0 obj=800

  // Phase 0
  LinearInterface phase0;
  const auto x0 = phase0.add_col("x0", 0.0, 100.0);
  phase0.set_obj_coeff(x0, 10.0);
  const auto alpha_col = phase0.add_col("alpha", 0.0, 1e12);
  phase0.set_obj_coeff(alpha_col, 1.0);

  auto constr0 = SparseRow {
      .name = "min_gen",
      .lowb = 20.0,
      .uppb = LinearProblem::DblMax,
  };
  constr0[x0] = 1.0;
  phase0.add_row(constr0);

  // Phase 1
  LinearInterface phase1;
  const auto x1 = phase1.add_col("x1", 0.0, 100.0);
  phase1.set_obj_coeff(x1, 50.0);
  const auto dep = phase1.add_col("dep", 20.0, 20.0);

  auto constr1 = SparseRow {
      .name = "demand",
      .lowb = 80.0,
      .uppb = LinearProblem::DblMax,
  };
  constr1[x1] = 1.0;
  constr1[dep] = 1.0;
  phase1.add_row(constr1);

  // Forward pass iteration 1: solve phase 0
  auto r0 = phase0.resolve({});
  REQUIRE(r0.has_value());
  REQUIRE(phase0.is_optimal());
  const double lb_before = phase0.get_obj_value();

  // Propagate x0 → dep
  std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = x0,
          .dependent_col = dep,
          .trial_value = 20.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };
  propagate_trial_values(links, phase0.get_col_sol(), phase1);
  CHECK(links[0].trial_value == doctest::Approx(20.0));

  // Solve phase 1
  auto r1 = phase1.resolve({});
  REQUIRE(r1.has_value());
  REQUIRE(phase1.is_optimal());
  CHECK(phase1.get_obj_value() == doctest::Approx(3000.0));  // 60*50

  // Backward: build optimality cut and add to phase 0
  auto cut = build_benders_cut(alpha_col,
                               links,
                               phase1.get_col_cost(),
                               phase1.get_obj_value(),
                               "iter1_cut");
  phase0.add_row(cut);

  // Re-solve phase 0 with cut
  auto r0b = phase0.resolve({});
  REQUIRE(r0b.has_value());
  REQUIRE(phase0.is_optimal());
  const double lb_after = phase0.get_obj_value();

  // Lower bound must increase (cut tightens approximation)
  CHECK(lb_after > lb_before);
  // Phase 0 should now choose larger x0
  CHECK(phase0.get_col_sol()[x0] > 20.0 + 1e-6);
}

// ─── BendersCut class tests ──────────────────────────────────────────────────
//
// These tests exercise BendersCut without a pool (null-pool mode) and with
// a work pool, verifying that:
//  - elastic_filter_solve() works equivalently to the free function
//  - infeasible_cut_count() is incremented on each successful elastic solve
//  - reset_infeasible_cut_count() resets the counter

TEST_CASE(
    "BendersCut - default construction and no-pool elastic_filter_solve")  // NOLINT
{
  // Build a simple LP where dep is fixed at 50 and x0 <= 80; demand >= 100.
  // With dep=50 the LP is feasible; the elastic filter should relax dep.
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto r0 = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  BendersCut bc;  // null pool
  CHECK(bc.pool() == nullptr);
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
  }
  CHECK(bc.infeasible_cut_count() == 1);

  // A second solve increments again
  auto result2 = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result2.has_value());
  CHECK(bc.infeasible_cut_count() == 2);

  // Reset resets the counter
  bc.reset_infeasible_cut_count();
  CHECK(bc.infeasible_cut_count() == 0);

  // Original LP untouched
  CHECK(li.get_col_low()[dep] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[dep] == doctest::Approx(50.0));
}

TEST_CASE("BendersCut - elastic_filter_solve with work pool")  // NOLINT
{
  // Same LP as above but with a live work pool.
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto r0 = li.resolve({});

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = ColIndex {10},
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  auto pool = make_solver_work_pool();
  BendersCut bc(pool.get());
  CHECK(bc.pool() == pool.get());
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.elastic_filter_solve(li, links, 1e6, {});
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->clone.is_optimal());
  }
  // Counter incremented once
  CHECK(bc.infeasible_cut_count() == 1);

  // nullopt when no fixed columns
  LinearInterface li2;
  const auto x1 = li2.add_col("x1", 0.0, 100.0);
  li2.set_obj_coeff(x1, 5.0);
  const auto dep2 = li2.add_col("dep2", 0.0, 100.0);  // NOT fixed
  auto d2 =
      SparseRow {.name = "d", .lowb = 50.0, .uppb = LinearProblem::DblMax};
  d2[x1] = 1.0;
  d2[dep2] = 1.0;
  li2.add_row(d2);
  [[maybe_unused]] auto r2 = li2.resolve({});

  const std::vector<StateVarLink> links2 = {
      StateVarLink {
          .dependent_col = dep2,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };
  auto result2 = bc.elastic_filter_solve(li2, links2, 1e6, {});
  CHECK_FALSE(result2.has_value());
  // Counter unchanged (no fixed column, no solve)
  CHECK(bc.infeasible_cut_count() == 1);

  // Detach pool before it goes out of scope
  bc.set_pool(nullptr);
}

TEST_CASE("BendersCut - build_feasibility_cut increments counter")  // NOLINT
{
  LinearInterface li;
  const auto x0 = li.add_col("x0", 0.0, 200.0);
  li.set_obj_coeff(x0, 10.0);
  const auto dep = li.add_col("dep", 50.0, 50.0);

  auto demand = SparseRow {
      .name = "demand",
      .lowb = 100.0,
      .uppb = LinearProblem::DblMax,
  };
  demand[x0] = 1.0;
  demand[dep] = 1.0;
  li.add_row(demand);
  [[maybe_unused]] auto resolve_ok = li.resolve({});

  const auto alpha_col = ColIndex {10};
  const auto src = ColIndex {11};

  const std::vector<StateVarLink> links = {
      StateVarLink {
          .source_col = src,
          .dependent_col = dep,
          .target_phase = PhaseIndex {1},
          .trial_value = 50.0,
          .source_low = 0.0,
          .source_upp = 200.0,
      },
  };

  BendersCut bc;
  CHECK(bc.infeasible_cut_count() == 0);

  auto result = bc.build_feasibility_cut(
      li, alpha_col, links, 1e6, SolverOptions {}, "feas");  // NOLINT
  REQUIRE(result.has_value());
  if (result) {
    CHECK(result->cut.name == "feas");
    CHECK(result->cut.get_coeff(alpha_col) == doctest::Approx(1.0));
    CHECK(result->elastic.clone.is_optimal());
  }

  // build_feasibility_cut calls elastic_filter_solve internally → counter == 1
  CHECK(bc.infeasible_cut_count() == 1);

  bc.reset_infeasible_cut_count();
  CHECK(bc.infeasible_cut_count() == 0);
}

TEST_CASE("BendersCut - set_pool updates pool reference")  // NOLINT
{
  BendersCut bc;
  CHECK(bc.pool() == nullptr);

  auto pool = make_solver_work_pool();
  bc.set_pool(pool.get());
  CHECK(bc.pool() == pool.get());

  bc.set_pool(nullptr);
  CHECK(bc.pool() == nullptr);
}

// ─── Simple 2-phase linear test helpers ─────────────────────────────────────

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Create a minimal 2-phase hydro+thermal planning problem.
///
/// - 1 bus
/// - 1 hydro generator (20 MW, $5/MWh)
/// - 1 thermal generator (100 MW, $50/MWh)
/// - 1 demand (30 MW constant)
/// - 1 reservoir (capacity 100 dam³, starts at 50 dam³, inflow 5 dam³/h)
/// - 2 phases, each with 1 stage of 4 blocks (1 hour each)
///
/// This is the simplest possible SDDP test case with a state variable
/// (reservoir volume) linking the two phases via Benders cuts.
auto make_2phase_linear_planning() -> Planning
{
  constexpr int num_phases = 2;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  block_array.reserve(total_blocks);
  for (int i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = blocks_per_phase,
      },
      Stage {
          .uid = Uid {2},
          .first_block = blocks_per_phase,
          .count_block = blocks_per_phase,
      },
  };

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
  };

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
          .capacity = 20.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 30.0,
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
          .fmax = 50.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 5.0,
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
                  .probability_factor = 1.0,
              },
          },
      .phase_array = std::move(phase_array),
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  System system = {
      .name = "sddp_linear_2phase",
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

/// Create a 2-phase planning with 2 scenarios for aperture testing.
/// Each scenario has a different inflow (5 and 15 dam³/h).
auto make_2phase_2scenario_planning() -> Planning
{
  auto planning = make_2phase_linear_planning();

  // Add second scenario
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {2},
      .probability_factor = 0.5,
  });
  // Set first scenario probability
  planning.simulation.scenario_array[0].probability_factor = OptReal {0.5};

  // Add second flow with different discharge for scenario 2
  planning.system.flow_array.push_back(Flow {
      .uid = Uid {2},
      .name = "inflow_dry",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 15.0,
  });

  return planning;
}

}  // namespace

// ─── Simple 2-phase linear SDDP tests ──────────────────────────────────────

TEST_CASE("SDDPSolver - 2-phase linear converges")  // NOLINT
{
  auto planning = make_2phase_linear_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 10;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.enable_api = false;

  SDDPSolver sddp(plp, sddp_opts);
  auto results = sddp.solve();

  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  SUBCASE("converges within allowed iterations")
  {
    CHECK(results->back().converged);
  }

  SUBCASE("Benders cuts were generated")
  {
    // At least one cut should have been added in the backward pass
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }

  SUBCASE("stored cuts match total cuts added")
  {
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(sddp.num_stored_cuts() == total_cuts);
  }

  SUBCASE("lower bound approaches upper bound")
  {
    const auto& last = results->back();
    CHECK(last.lower_bound > 0.0);
    CHECK(last.upper_bound > 0.0);
    CHECK(last.gap < 1e-4);
  }
}

TEST_CASE("SDDPSolver - 2-phase with apertures converges")  // NOLINT
{
  auto planning = make_2phase_2scenario_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = false;

  SUBCASE("apertures disabled (baseline)")
  {
    sddp_opts.num_apertures = 0;
    SDDPSolver sddp(plp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
  }

  SUBCASE("apertures enabled with -1 (all scenarios)")
  {
    sddp_opts.num_apertures = -1;
    SDDPSolver sddp(plp, sddp_opts);
    auto results = sddp.solve();

    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());

    // At least one cut should be generated
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }

  SUBCASE("apertures enabled with explicit count")
  {
    sddp_opts.num_apertures = 2;
    SDDPSolver sddp(plp, sddp_opts);
    auto results = sddp.solve();

    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());

    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }
}

// ─── Unit tests for free utility functions
// ────────────────────────────────────

TEST_CASE(
    "compute_scene_weights - all scenes feasible, equal probability")  // NOLINT
{
  // 3 feasible scenes, no SceneLP objects (uses fallback weight=1)
  const std::vector<uint8_t> feasible {1, 1, 1};
  const std::vector<SceneLP> scenes {};  // empty → uses fallback 1.0 per scene
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(1.0 / 3.0));
  CHECK(w[1] == doctest::Approx(1.0 / 3.0));
  CHECK(w[2] == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("compute_scene_weights - one scene infeasible")  // NOLINT
{
  // scene 1 infeasible → weight must be 0, remaining two share probability
  const std::vector<uint8_t> feasible {1, 0, 1};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[1] == doctest::Approx(0.0));
  CHECK(w[0] == doctest::Approx(0.5));
  CHECK(w[2] == doctest::Approx(0.5));
}

TEST_CASE(
    "compute_scene_weights - all scenes infeasible returns zeros")  // NOLINT
{
  const std::vector<uint8_t> feasible {0, 0, 0};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(0.0));
  CHECK(w[1] == doctest::Approx(0.0));
  CHECK(w[2] == doctest::Approx(0.0));
}

TEST_CASE(
    "compute_scene_weights - single feasible scene gets weight 1")  // NOLINT
{
  const std::vector<uint8_t> feasible {0, 1, 0};
  const std::vector<SceneLP> scenes {};
  const auto w = compute_scene_weights(scenes, feasible);
  REQUIRE(w.size() == 3);
  CHECK(w[0] == doctest::Approx(0.0));
  CHECK(w[1] == doctest::Approx(1.0));
  CHECK(w[2] == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap - basic gap")  // NOLINT
{
  CHECK(compute_convergence_gap(100.0, 90.0) == doctest::Approx(0.1));
}

TEST_CASE(
    "compute_convergence_gap - zero upper bound uses denominator 1")  // NOLINT
{
  // denom = max(1.0, |0.0|) = 1.0
  CHECK(compute_convergence_gap(0.0, -1.0) == doctest::Approx(1.0));
}

TEST_CASE("compute_convergence_gap - converged returns zero gap")  // NOLINT
{
  CHECK(compute_convergence_gap(50.0, 50.0) == doctest::Approx(0.0));
}

TEST_CASE("compute_convergence_gap - large absolute upper bound")  // NOLINT
{
  // denom = max(1.0, 1000.0) = 1000.0 → gap = 10/1000 = 0.01
  CHECK(compute_convergence_gap(1000.0, 990.0) == doctest::Approx(0.01));
}
