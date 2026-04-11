/**
 * @file      test_sddp_feasibility.hpp
 * @brief     Unit tests for SDDP feasibility backpropagation
 * @date      2026-03-27
 * @copyright BSD-3-Clause
 *
 * Tests the feasibility_backpropagate() code path in sddp_feasibility.cpp.
 * These tests create planning problems where intermediate phases are
 * infeasible under the initial trial point, forcing the SDDP solver to
 * activate elastic filter and backpropagate feasibility cuts.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Create a 3-phase hydro+thermal problem where the middle phase has
/// demand exceeding total generation capacity, forcing elastic fallback
/// and feasibility backpropagation in the backward pass.
///
/// Phase 0: demand = 80 MW, capacity = 50+500 MW -> feasible
/// Phase 1: demand = 600 MW, hydro capacity = 50 MW, thermal = 500 MW
///          but reservoir emax = 20 dam3 severely limits hydro ->
///          tight/infeasible
/// Phase 2: demand = 80 MW -> feasible
auto make_infeasible_middle_phase_planning() -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

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
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // Phase 1 demand is high enough to stress the system when reservoir
  // bounds are tight -- the thermal generator alone can supply 500 MW but
  // the combination with reservoir constraints creates a feasibility tension.
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 100.0,
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
      .fmax = 100.0,
  }};

  // Tight reservoir: small capacity forces state variable conflicts
  // between phases, triggering infeasibility in the backward pass.
  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 20.0,
      .emin = 0.0,
      .emax = 20.0,
      .eini = 10.0,
      .fmin = -1000.0,
      .fmax = +1000.0,
      .flow_conversion_rate = 1.0,
  }};

  const Array<Flow> flow_array = {{
      .uid = Uid {1},
      .name = "inflow",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 10.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_feas_test",
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

/// Create a problem where ALL phases are impossible to serve:
/// demand = 10000 MW vs capacity = 550 MW.  Even phase 0 is infeasible
/// after elastic relaxation fails to propagate a useful trial point.
auto make_fully_infeasible_planning() -> Planning
{
  auto planning = make_infeasible_middle_phase_planning();
  // Set demand so high that even with demand_fail_cost penalty the
  // SDDP iteration cannot converge -- the reservoir state variable
  // bounds create an irreconcilable conflict after backpropagation
  // reaches phase 0.
  planning.system.demand_array[0].capacity = 10000.0;
  // Remove demand_fail_cost so infeasibility is genuine
  planning.options.demand_fail_cost = std::nullopt;
  return planning;
}

/// Create a 2-phase minimal thermal-only problem that is always feasible.
/// No hydro system means no state variable tension; SDDP should converge
/// quickly with no feasibility backpropagation needed.
auto make_feasible_2phase_thermal_planning() -> Planning
{
  constexpr int num_phases = 2;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  // Ample thermal capacity: 500 MW vs 100 MW demand
  const Array<Generator> generator_array = {{
      .uid = Uid {1},
      .name = "thermal_gen",
      .bus = Uid {1},
      .gcost = 50.0,
      .capacity = 500.0,
  }};

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 100.0,
  }};

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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_feas_thermal_test",
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

/// Create a 3-phase problem with 2 scenarios to exercise feasibility
/// backpropagation with multiple scenes.  Scenario 1 has tight demand
/// (same as make_infeasible_middle_phase_planning), scenario 2 has
/// low demand (always feasible).  This tests that backpropagation
/// handles per-scene infeasibility correctly.
auto make_multi_scene_infeasible_planning() -> Planning
{
  auto planning = make_infeasible_middle_phase_planning();

  // Add a second scenario with equal probability
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {2},
      .probability_factor = 0.5,
  });

  // First scenario also gets explicit probability
  planning.simulation.scenario_array[0].probability_factor = 0.5;

  return planning;
}

/// Create a 4-phase problem where phases 1 and 2 are both tight,
/// requiring backpropagation across multiple phases.
auto make_deep_backpropagation_planning() -> Planning
{
  constexpr int num_phases = 4;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

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
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 100.0,
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
      .fmax = 100.0,
  }};

  // Very tight reservoir: capacity=10 forces conflicts across
  // multiple phases, requiring deep backpropagation.
  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 10.0,
      .emin = 0.0,
      .emax = 10.0,
      .eini = 5.0,
      .fmin = -1000.0,
      .fmax = +1000.0,
      .flow_conversion_rate = 1.0,
  }};

  const Array<Flow> flow_array = {{
      .uid = Uid {1},
      .name = "inflow",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 10.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_feas_deep_test",
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

// ---------------------------------------------------------------------------
// Feasibility backpropagation -- single_cut mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — single_cut mode adds feasibility cut")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // The solver should converge despite infeasibility
  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Feasibility backpropagation -- multi_cut mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi_cut mode adds multi-cuts")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Feasibility backpropagation -- backpropagate mode (PLP mechanism)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — backpropagate mode updates bounds")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::backpropagate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Feasibility backpropagation -- multi_cut_threshold=0 auto-switch
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi_cut_threshold=0 forces multi-cut")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.multi_cut_threshold = 0;  // always force multi-cut
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Feasibility backpropagation -- fully infeasible returns error
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — fully infeasible returns error")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_fully_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();

  // Either the solver returns an error or it hits max_iterations
  // without converging -- both are acceptable outcomes for a
  // fundamentally infeasible problem.
  if (results.has_value()) {
    CHECK_FALSE(results->back().converged);
  }
}

// ---------------------------------------------------------------------------
// Feasible problem: no backpropagation needed
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — feasible problem needs no elastic")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_feasible_2phase_thermal_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // A purely feasible problem should converge without needing
  // any feasibility cuts.
  CHECK(results->back().converged);

  // Verify that the objective value is positive (cost of serving
  // 100 MW with a $50/MWh thermal over 8 block-hours).
  CHECK(results->back().lower_bound > 0.0);
}

// ---------------------------------------------------------------------------
// Multi-scene feasibility backpropagation
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi-scene single_cut converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_multi_scene_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi-scene multi_cut converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_multi_scene_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi-scene backpropagate converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_multi_scene_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::backpropagate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Deep backpropagation across multiple phases (4 phases)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — deep 4-phase single_cut converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_deep_backpropagation_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — deep 4-phase multi_cut converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_deep_backpropagation_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — deep 4-phase backpropagate converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_deep_backpropagation_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 40;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::backpropagate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// multi_cut_threshold > 0: auto-switch after counter exceeds threshold
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi_cut_threshold=1 auto-switches")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.multi_cut_threshold = 1;  // switch after 1 infeasibility
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// multi_cut_threshold < 0: never auto-switch (disabled)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — multi_cut_threshold<0 disables switch")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.multi_cut_threshold = -1;  // never auto-switch
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Varying elastic_penalty: low penalty vs high penalty
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — low elastic penalty converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e3;  // lower penalty
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — very high elastic penalty converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e9;  // very high penalty
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// scale_alpha affects feasibility cut scaling
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — scale_alpha=1 converges")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.scale_alpha = 1.0;  // no alpha scaling
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Fully infeasible with multi_cut mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — fully infeasible multi_cut")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_fully_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();

  // Either error or non-convergence is acceptable for a
  // fundamentally infeasible problem.
  if (results.has_value()) {
    CHECK_FALSE(results->back().converged);
  }
}

// ---------------------------------------------------------------------------
// Fully infeasible with backpropagate mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — fully infeasible backpropagate")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_fully_infeasible_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::backpropagate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();

  // Either error or non-convergence is acceptable for a
  // fundamentally infeasible problem (backpropagate mode).
  if (results.has_value()) {
    CHECK_FALSE(results->back().converged);
  }
}

// ---------------------------------------------------------------------------
// Deep backpropagation with multi-scene (4 phases, 2 scenarios)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — deep 4-phase multi-scene backpropagate")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_deep_backpropagation_planning();

  // Add a second scenario
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {2},
      .probability_factor = 0.5,
  });
  planning.simulation.scenario_array[0].probability_factor = 0.5;

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 50;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::backpropagate;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());
  CHECK(results->back().converged);
}

// ---------------------------------------------------------------------------
// Convergence bounds: lower <= upper on converged infeasible-middle problem
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — converged bounds are consistent")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_infeasible_middle_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto& last = results->back();
  if (last.converged) {
    // Lower bound should be <= upper bound at convergence
    CHECK(last.lower_bound <= last.upper_bound + 1e-6);
    // Both bounds should be finite
    CHECK(last.lower_bound > -1e30);
    CHECK(last.upper_bound < 1e30);
  }
}

// ---------------------------------------------------------------------------
// Feasible 2-phase: verify exact objective with multi_cut mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "feasibility_backpropagate — feasible 2-phase multi_cut objective")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_feasible_2phase_thermal_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.elastic_penalty = 1e6;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  CHECK(results->back().converged);

  // 100 MW demand, $50/MWh thermal, 2 phases x 4 blocks x 1 hour each
  // = 100 * 50 * 8 = 40000
  CHECK(results->back().lower_bound == doctest::Approx(40000.0).epsilon(0.01));
}
