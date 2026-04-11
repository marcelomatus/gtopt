// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_update_lp.cpp
 * @brief     Unit tests for update_lp, SystemLP dispatch, and
 *            monolithic-vs-SDDP integration comparisons
 * @date      2026-04-05
 */

#include <cmath>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/json/json_monolithic_options.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/validate_planning.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Integration: monolithic vs SDDP comparison ────────────────────────────

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

  const auto mono_obj =
      plp_mono.system(first_scene_index(), first_phase_index())
          .linear_interface()
          .get_obj_value();
  SPDLOG_INFO("Reservoir mono: phase-0 obj = {:.4f}", mono_obj);

  // Compute total monolithic cost across all phases
  double mono_total = 0.0;
  for (int p = 0; p < 5; ++p) {
    const auto ph_obj = plp_mono.system(first_scene_index(), PhaseIndex {p})
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

  SDDPMethod sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO("Reservoir SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
              last.iteration_index,
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
    const auto ph_obj = plp_mono.system(first_scene_index(), PhaseIndex {p})
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

  SDDPMethod sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO(
      "Small reservoir SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
      last.iteration_index,
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
    const auto ph_obj = plp_mono.system(first_scene_index(), PhaseIndex {p})
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

  SDDPMethod sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO("Expansion SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
              last.iteration_index,
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
    const auto ph_obj = plp_mono.system(first_scene_index(), PhaseIndex {p})
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

  SDDPMethod sddp(plp_sddp, sddp_opts);
  auto sddp_results = sddp.solve();
  REQUIRE(sddp_results.has_value());
  CHECK_FALSE(sddp_results->empty());

  const auto& last = sddp_results->back();
  SPDLOG_INFO(
      "Yearly hydro SDDP: {} iterations, LB={:.4f} UB={:.4f} gap={:.6f}",
      last.iteration_index,
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

TEST_CASE("SDDPMethod API - iteration callback")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);

  // Register a callback that collects iteration data and stops after 3 iters
  std::vector<SDDPIterationResult> callback_results;
  sddp.set_iteration_callback(
      [&callback_results](const SDDPIterationResult& r) -> bool
      {
        callback_results.push_back(r);
        SPDLOG_INFO(
            "API callback: iter {} gap={:.6f}", r.iteration_index, r.gap);
        return r.iteration_index >= 3;  // stop after 3 iterations
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // The callback should have been called for each iteration (not the final
  // forward pass).  results includes the final forward pass as the last entry.
  CHECK(callback_results.size() + 1 == results->size());
  // The solver should have stopped after 3 iterations + 1 final forward pass
  CHECK(results->size() <= 4);
  // Callback iteration numbers should be sequential
  for (size_t i = 0; i < callback_results.size(); ++i) {
    CHECK(callback_results[i].iteration_index
          == IterationIndex {static_cast<int>(i)});
  }
}

TEST_CASE("SDDPMethod API - programmatic stop")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 100;
  sddp_opts.convergence_tol = 1e-12;  // very tight → won't converge in 2 iters

  SDDPMethod sddp(planning_lp, sddp_opts);

  // Request stop after 2 iterations via the callback
  sddp.set_iteration_callback(
      [&sddp](const SDDPIterationResult& r) -> bool
      {
        if (r.iteration_index >= 2) {
          sddp.request_stop();
        }
        return false;  // don't stop via callback return value
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Should have stopped after ≤ 3 iterations + 1 final forward pass
  CHECK(results->size() <= 4);
  // The stop flag remains set (it was honoured by exiting the loop)
  CHECK(sddp.is_stop_requested());
}

TEST_CASE("SDDPMethod API - live query atomics")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;

  SDDPMethod sddp(planning_lp, sddp_opts);

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
        CHECK(sddp.current_iteration() == r.iteration_index);
        CHECK(sddp.current_gap() == doctest::Approx(r.gap));
        CHECK(sddp.current_lower_bound() == doctest::Approx(r.lower_bound));
        CHECK(sddp.current_upper_bound() == doctest::Approx(r.upper_bound));
        last_gap = r.gap;
        return false;
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After solving, live-query should reflect final state (0-based iteration)
  CHECK(sddp.current_iteration() == static_cast<int>(results->size() - 1));
  CHECK(sddp.current_gap() == doctest::Approx(results->back().gap));
  if (results->back().converged) {
    CHECK(sddp.has_converged());
  }
}

// ─── update_lp unit tests ───────────────────────────────────────

TEST_CASE("update_lp - no-op when no updatable elements")  // NOLINT
{
  // Build a minimal system WITHOUT a ReservoirProductionFactor element.
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
          .production_factor = 1.0,
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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(options);
  SimulationLP sim_lp(simulation, options_lp);
  SystemLP system_lp(system, sim_lp);

  // The solver must support set_coeff for update_lp to actually try anything;
  // either way, the function call should be safe.
  [[maybe_unused]] const bool set_coeff_supported =
      system_lp.linear_interface().supports_set_coeff();

  // update_lp with no production factor elements → 0 updated
  const auto updated = system_lp.update_lp();
  CHECK(updated == 0);
}

TEST_CASE(
    "ReservoirSeepageLP::update_lp is a no-op without segments")  // NOLINT
{
  // Verify the trivial no-op path of ReservoirSeepageLP::update_lp by calling
  // update_lp on a system that has seepage without
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
          .production_factor = 1.0,
      },
  };
  const Array<ReservoirSeepage> reservoir_seepage_array = {
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
      .name = "test_reservoir_seepage_noop",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(options);
  SimulationLP sim_lp(simulation, options_lp);
  SystemLP system_lp(system, sim_lp);

  // ReservoirSeepageLP::update_lp is a no-op when no segments are present → 0
  const auto updated = system_lp.update_lp();
  CHECK(updated == 0);
}

// ─── SystemLP::update_lp dispatches to all HasUpdateLP elements ─────────────

TEST_CASE(
    "SystemLP::update_lp dispatches to all HasUpdateLP elements")  // NOLINT
{
  // Build a system with all three HasUpdateLP element types:
  //   ReservoirProductionFactorLP, ReservoirSeepageLP,
  //   ReservoirDischargeLimitLP
  // All have piecewise segments so their update_lp actually modifies
  // coefficients.  A single call to system_lp.update_lp() must dispatch
  // to all three.

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
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
  };

  // ww1: turbine waterway (also used by discharge limit)
  // ww2: seepage waterway
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "ww_seep",
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
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 500.0,
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
          .main_reservoir = Uid {1},
      },
  };

  // ProductionFactor with 2 segments → update_lp modifies turbine coeff
  const Array<ReservoirProductionFactor> reservoir_production_factor_array = {
      {
          .uid = Uid {1},
          .name = "eff1",
          .turbine = Uid {1},
          .reservoir = Uid {1},
          .mean_production_factor = 1.5,
          .segments =
              {
                  {.volume = 0.0, .slope = 0.001, .constant = 1.0},
                  {.volume = 800.0, .slope = 0.0001, .constant = 1.5},
              },
      },
  };

  // Seepage with 2 segments → update_lp modifies seepage constraint
  const Array<ReservoirSeepage> reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {2},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 0.0003, .constant = 0.5},
                  {.volume = 1000.0, .slope = 0.0001, .constant = 0.7},
              },
      },
  };

  // DischargeLimit with 2 segments → update_lp modifies DDL constraint
  const Array<ReservoirDischargeLimit> reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {.volume = 0.0, .slope = 7e-5, .intercept = 15.0},
                  {.volume = 1000.0, .slope = 1.4e-4, .intercept = 57.0},
              },
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1.0},
              {.uid = Uid {2}, .duration = 2.0},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const System system = {
      .name = "test_update_lp_dispatch",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .reservoir_discharge_limit_array = reservoir_discharge_limit_array,
      .turbine_array = turbine_array,
      .reservoir_production_factor_array = reservoir_production_factor_array,
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options_lp(options);
  SimulationLP sim_lp(simulation, options_lp);
  SystemLP system_lp(system, sim_lp);

  auto& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  SUBCASE("update_lp dispatches to all three element types")
  {
    // First solve to establish a baseline solution
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Call system_lp.update_lp() — should dispatch to all three elements
    const auto updated = system_lp.update_lp();

    // ProductionFactor always updates (initial coeff was mean=1.5,
    // update sets it from segments at eini=500).
    // Seepage and DDL update only if segment selection changes.
    // At minimum, ProductionFactor contributes > 0 updates.
    CHECK(updated > 0);

    // Verify each element type is present in the system
    CHECK(system_lp.elements<ReservoirProductionFactorLP>().size() == 1);
    CHECK(system_lp.elements<ReservoirSeepageLP>().size() == 1);
    CHECK(system_lp.elements<ReservoirDischargeLimitLP>().size() == 1);

    // Verify ProductionFactor coefficient was actually updated:
    // The initial production_factor was 1.0 from the Turbine.  After
    // update_lp, the coefficient should differ (set from piecewise curve
    // at the solution volume, which is near eini=500).
    auto& eff = system_lp.elements<ReservoirProductionFactorLP>().front();
    const auto& bmap =
        eff.coeff_indices_at(make_uid<Scenario>(0), StageUid {1});
    CHECK_FALSE(bmap.empty());
    for (const auto& [buid, ci] : bmap) {
      const auto coeff = lp.get_coeff(ci.row, ci.col);
      // Coefficient is -rate; original was -1.0, updated value should
      // be in the range of the piecewise segments (between -1.0 and -2.0)
      CHECK(coeff < -1.0);
      CHECK(coeff > -2.0);
    }
  }

  SUBCASE("system solves after update_lp")
  {
    auto result1 = lp.resolve();
    REQUIRE(result1.has_value());
    CHECK(result1.value() == 0);

    std::ignore = system_lp.update_lp();

    auto result2 = lp.resolve();
    REQUIRE(result2.has_value());
    CHECK(result2.value() == 0);
  }

  SUBCASE("second update_lp is idempotent when volume unchanged")
  {
    auto result = lp.resolve();
    REQUIRE(result.has_value());

    const auto updated1 = system_lp.update_lp();
    const auto updated2 = system_lp.update_lp();

    // Second call with same solution → no additional changes for
    // seepage/DDL (they track state). ProductionFactor always writes
    // the coefficient, so it may still count.
    CHECK(updated2 <= updated1);
  }
}
