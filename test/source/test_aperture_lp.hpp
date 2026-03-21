/**
 * @file      test_aperture_lp.hpp
 * @brief     Unit tests for aperture LP update process (FlowLP::
 *            update_aperture_lp and aperture scenario file mechanism)
 * @date      2026-03-17
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Aperture struct construction, defaults, and is_active
 *  2. Aperture JSON parsing and round-trip
 *  3. FlowLP::update_aperture_lp – bound updates per-block
 *  4. update_aperture_lp – inactive flow returns true (no-op)
 *  5. update_aperture_lp – missing scenario/stage returns true
 *  6. OptionsLP::sddp_aperture_directory accessor
 *  7. End-to-end aperture LP update with multi-scenario hydro system
 *  8. Explicit aperture_array in SDDP planning
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/aperture.hpp>
#include <gtopt/json/json_aperture.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── 1. Aperture struct ─────────────────────────────────────────────────────

TEST_CASE("Aperture struct defaults")  // NOLINT
{
  const Aperture ap;

  SUBCASE("default uid is unknown")
  {
    CHECK(ap.uid == unknown_uid);
  }

  SUBCASE("default source_scenario is unknown")
  {
    CHECK(ap.source_scenario == unknown_uid);
  }

  SUBCASE("default probability_factor is 1")
  {
    REQUIRE(ap.probability_factor.has_value());
    CHECK(ap.probability_factor.value_or(0.0) == doctest::Approx(1.0));
  }

  SUBCASE("default is_active returns true")
  {
    CHECK(ap.is_active());
  }

  SUBCASE("explicit active=false makes is_active return false")
  {
    const Aperture inactive {
        .uid = Uid {1},
        .active = false,
        .source_scenario = Uid {1},
    };
    CHECK_FALSE(inactive.is_active());
  }

  SUBCASE("name is empty by default")
  {
    CHECK_FALSE(ap.name.has_value());
  }

  SUBCASE("class_name is aperture")
  {
    CHECK(Aperture::class_name == "aperture");
  }
}

// ─── 2. Aperture JSON parsing ───────────────────────────────────────────────

TEST_CASE("Aperture JSON parsing")  // NOLINT
{
  SUBCASE("minimal fields")
  {
    constexpr std::string_view json = R"({
      "uid": 1,
      "source_scenario": 5
    })";
    const auto ap = daw::json::from_json<Aperture>(json);
    CHECK(ap.uid == 1);
    CHECK(ap.source_scenario == 5);
    // probability_factor is json_number_null — absent means nullopt
    CHECK_FALSE(ap.probability_factor.has_value());
    // value_or(1.0) matches the solver's default behaviour
    CHECK(ap.probability_factor.value_or(1.0) == doctest::Approx(1.0));
    CHECK(ap.is_active());
  }

  SUBCASE("all fields")
  {
    constexpr std::string_view json = R"({
      "uid": 3,
      "name": "wet_hydro",
      "active": 1,
      "source_scenario": 10,
      "probability_factor": 0.4
    })";
    const auto ap = daw::json::from_json<Aperture>(json);
    CHECK(ap.uid == 3);
    CHECK(ap.name.value_or("") == "wet_hydro");
    CHECK(ap.source_scenario == 10);
    CHECK(ap.probability_factor.value_or(0.0) == doctest::Approx(0.4));
    CHECK(ap.is_active());
  }

  SUBCASE("inactive aperture")
  {
    constexpr std::string_view json = R"({
      "uid": 7,
      "active": 0,
      "source_scenario": 2,
      "probability_factor": 0.0
    })";
    const auto ap = daw::json::from_json<Aperture>(json);
    CHECK(ap.uid == 7);
    CHECK_FALSE(ap.is_active());
    CHECK(ap.probability_factor.value_or(1.0) == doctest::Approx(0.0));
  }

  SUBCASE("array parsing")
  {
    constexpr std::string_view json = R"([
      {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
      {"uid": 2, "source_scenario": 5, "probability_factor": 0.3},
      {"uid": 3, "source_scenario": 10, "probability_factor": 0.2}
    ])";
    const auto apertures = daw::json::from_json_array<Aperture>(json);
    REQUIRE(apertures.size() == 3);
    CHECK(apertures[0].uid == 1);
    CHECK(apertures[1].source_scenario == 5);
    CHECK(apertures[2].probability_factor.value_or(0.0)
          == doctest::Approx(0.2));
  }

  SUBCASE("round-trip")
  {
    const Aperture original {
        .uid = Uid {42},
        .name = OptName {"test_aperture"},
        .active = true,
        .source_scenario = Uid {7},
        .probability_factor = 0.75,
    };
    const auto json_str = daw::json::to_json(original);
    const auto restored = daw::json::from_json<Aperture>(json_str);

    CHECK(restored.uid == original.uid);
    CHECK(restored.name.value_or("") == "test_aperture");
    CHECK(restored.source_scenario == original.source_scenario);
    CHECK(restored.probability_factor.value_or(0.0) == doctest::Approx(0.75));
  }
}

// ─── 3. FlowLP::update_aperture_lp with multi-scenario system ───────────

TEST_CASE("FlowLP update_aperture_lp updates bounds correctly")  // NOLINT
{
  // Build a hydro system with 2 scenarios and different discharge per scenario.
  // Scenario 1: discharge = 10 m³/s, Scenario 2: discharge = 30 m³/s.
  // After building the LP for scenario 1 (base), calling
  // update_aperture_lp with scenario 2 should change the flow column
  // bounds to 30.

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
          .capacity = 200.0,
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
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
          .drain = true,
      },
  };

  // Use a 3D vector for discharge: [scenario][stage][block]
  // Scenario 1 (index 0): discharge = 10.0 for all blocks
  // Scenario 2 (index 1): discharge = 30.0 for all blocks
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  10.0,
                  10.0,
              },
          },  // scenario 1, stage 1, blocks 1-2
          {
              {
                  30.0,
                  30.0,
              },
          },  // scenario 2, stage 1, blocks 1-2
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
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
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
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
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
  };

  const System system = {
      .name = "ApertureFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  // The LP was built for the first scenario.
  // Get the linear interface and verify the LP is solvable.
  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  // Get scenario and stage references from simulation
  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 2);

  const auto& base_scenario = scenarios[0];  // scenario uid=1
  const auto& aperture_scenario = scenarios[1];  // scenario uid=2

  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stages = phase.stages();
  REQUIRE_FALSE(stages.empty());
  const auto& stage = stages[0];

  // Access FlowLP elements
  const auto& flow_elements = sys_lp.elements<FlowLP>();
  REQUIRE_FALSE(flow_elements.empty());
  const auto& flow_lp = flow_elements[0];

  SUBCASE("original bounds are base scenario discharge values")
  {
    // The flow columns should have been built with scenario 1 discharge = 10
    const auto col_low = li.get_col_low();
    const auto col_upp = li.get_col_upp();

    const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
    for (const auto& [buid, col] : fcols) {
      CHECK(col_low[col] == doctest::Approx(10.0));
      CHECK(col_upp[col] == doctest::Approx(10.0));
    }
  }

  SUBCASE("update_aperture_lp changes bounds to aperture scenario values")
  {
    // Clone the LP and update bounds for scenario 2
    auto clone = li.clone();

    const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, aperture_scenario, stage);
    CHECK(ok);

    // Verify that bounds were changed to scenario 2 discharge = 30
    const auto col_low = clone.get_col_low();
    const auto col_upp = clone.get_col_upp();

    const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
    for (const auto& [buid, col] : fcols) {
      CHECK(col_low[col] == doctest::Approx(30.0));
      CHECK(col_upp[col] == doctest::Approx(30.0));
    }
  }

  SUBCASE("cloned LP affluent values are retrievable per block after update")
  {
    // Verify that each block's flow column in the clone has been updated
    // to the aperture scenario's discharge and that get_col_low / get_col_upp
    // return the new values.
    auto clone = li.clone();

    const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, aperture_scenario, stage);
    REQUIRE(ok);

    const auto clone_low = clone.get_col_low();
    const auto clone_upp = clone.get_col_upp();

    const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
    REQUIRE(fcols.size() == 2);  // 2 blocks

    // Each block should have the aperture scenario's discharge (30.0)
    for (const auto& [buid, col] : fcols) {
      CHECK(clone_low[col] == doctest::Approx(30.0));
      CHECK(clone_upp[col] == doctest::Approx(30.0));
    }

    // Cross-check: the original LP should still have 10.0
    const auto orig_low = li.get_col_low();
    const auto orig_upp = li.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      CHECK(orig_low[col] == doctest::Approx(10.0));
      CHECK(orig_upp[col] == doctest::Approx(10.0));
    }
  }

  SUBCASE("cloned LP solves after aperture update")
  {
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, aperture_scenario, stage);
    CHECK(ok);

    auto clone_result = clone.resolve();
    REQUIRE(clone_result.has_value());
    CHECK(clone_result.value() == 0);
  }

  SUBCASE(
      "no variable scaling correction needed — bounds equal physical "
      "discharge")
  {
    // FlowLP::add_to_lp creates columns with scale = 1.0 (default).
    // That means LP variable == physical value (m³/s).  Verify that the
    // bounds set by update_aperture_lp are the raw discharge values
    // without any scaling factor applied.  This is important because other
    // LP components (e.g. ReservoirLP with energy_scale, BusLP with
    // scale_theta) DO apply scaling — FlowLP intentionally does not.

    // Verify flow columns have scale = 1.0 via LinearInterface
    const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
    for (const auto& [buid, col] : fcols) {
      CHECK(li.get_col_scale(col) == doctest::Approx(1.0));
    }

    // Verify original bounds equal physical discharge for base scenario
    const auto base_low = li.get_col_low();
    const auto base_upp = li.get_col_upp();

    for (const auto& [buid, col] : fcols) {
      // Physical discharge for scenario 1 = 10.0 — bounds should match
      // exactly (no inv_energy_scale, no 1/scale_theta, etc.)
      CHECK(base_low[col] == doctest::Approx(10.0));
      CHECK(base_upp[col] == doctest::Approx(10.0));
    }

    // After aperture update, bounds should be the raw aperture discharge
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, aperture_scenario, stage);
    REQUIRE(ok);

    // Clone should also have scale = 1.0 (preserved through clone)
    for (const auto& [buid, col] : fcols) {
      CHECK(clone.get_col_scale(col) == doctest::Approx(1.0));
    }

    const auto ap_low = clone.get_col_low();
    const auto ap_upp = clone.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      // Physical discharge for scenario 2 = 30.0 — no scaling needed
      CHECK(ap_low[col] == doctest::Approx(30.0));
      CHECK(ap_upp[col] == doctest::Approx(30.0));
    }

    // Solve the clone and verify the solution value matches the bound
    // (fixed variable: low == upp, so solution == bound == physical value)
    auto res = clone.resolve();
    REQUIRE(res.has_value());
    const auto sol = clone.get_col_sol();
    for (const auto& [buid, col] : fcols) {
      CHECK(sol[col] == doctest::Approx(30.0));
    }
  }

  SUBCASE("original LP bounds unchanged after aperture update on clone")
  {
    auto clone = li.clone();
    [[maybe_unused]] const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, aperture_scenario, stage);

    // Original LP should still have scenario 1 bounds
    const auto orig_col_low = li.get_col_low();
    const auto orig_col_upp = li.get_col_upp();

    const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
    for (const auto& [buid, col] : fcols) {
      CHECK(orig_col_low[col] == doctest::Approx(10.0));
      CHECK(orig_col_upp[col] == doctest::Approx(10.0));
    }
  }
}

// ─── 3b. Verify clone bound retrieval after aperture update ─────────────────

TEST_CASE(
    "FlowLP clone bound retrieval — two apertures independently"
    " updated")  // NOLINT
{
  // Create a hydro system with 3 scenarios and verify that two independent
  // clones can each be updated to a different aperture scenario, and that
  // the new affluent (discharge) values are independently retrievable from
  // each clone's column bounds.

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
          .capacity = 200.0,
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
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
          .drain = true,
      },
  };

  // 3 scenarios: dry (5), normal (20), wet (50) — per-block discharge
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  5.0,
                  5.0,
                  5.0,
              },
          },  // scenario 1 (dry)
          {
              {
                  20.0,
                  20.0,
                  20.0,
              },
          },  // scenario 2 (normal)
          {
              {
                  50.0,
                  50.0,
                  50.0,
              },
          },  // scenario 3 (wet)
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 200.0,
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
              {
                  .uid = Uid {3},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 3,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.3,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.4,
              },
              {
                  .uid = Uid {3},
                  .probability_factor = 0.3,
              },
          },
  };

  const System system = {
      .name = "CloneBoundRetrievalTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 3);

  const auto& base_scenario = scenarios[0];  // scenario 1 (dry, 5)
  const auto& normal_scenario = scenarios[1];  // scenario 2 (normal, 20)
  const auto& wet_scenario = scenarios[2];  // scenario 3 (wet, 50)

  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& fcols = flow_lp.flow_cols_at(base_scenario, stage);
  REQUIRE(fcols.size() == 3);  // 3 blocks

  SUBCASE("original LP has base scenario bounds (dry = 5)")
  {
    const auto low = li.get_col_low();
    const auto upp = li.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      CHECK(low[col] == doctest::Approx(5.0));
      CHECK(upp[col] == doctest::Approx(5.0));
    }
  }

  SUBCASE("clone updated to normal scenario — bounds retrievable as 20")
  {
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(
        clone, base_scenario, normal_scenario, stage);
    REQUIRE(ok);

    const auto low = clone.get_col_low();
    const auto upp = clone.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      CHECK(low[col] == doctest::Approx(20.0));
      CHECK(upp[col] == doctest::Approx(20.0));
    }
  }

  SUBCASE("clone updated to wet scenario — bounds retrievable as 50")
  {
    auto clone = li.clone();
    const auto ok =
        flow_lp.update_aperture_lp(clone, base_scenario, wet_scenario, stage);
    REQUIRE(ok);

    const auto low = clone.get_col_low();
    const auto upp = clone.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      CHECK(low[col] == doctest::Approx(50.0));
      CHECK(upp[col] == doctest::Approx(50.0));
    }
  }

  SUBCASE("two clones updated independently — each has correct bounds")
  {
    auto clone_normal = li.clone();
    auto clone_wet = li.clone();

    const auto ok_normal = flow_lp.update_aperture_lp(
        clone_normal, base_scenario, normal_scenario, stage);
    const auto ok_wet = flow_lp.update_aperture_lp(
        clone_wet, base_scenario, wet_scenario, stage);
    REQUIRE(ok_normal);
    REQUIRE(ok_wet);

    const auto normal_low = clone_normal.get_col_low();
    const auto normal_upp = clone_normal.get_col_upp();
    const auto wet_low = clone_wet.get_col_low();
    const auto wet_upp = clone_wet.get_col_upp();
    const auto orig_low = li.get_col_low();
    const auto orig_upp = li.get_col_upp();

    for (const auto& [buid, col] : fcols) {
      // Normal clone: 20
      CHECK(normal_low[col] == doctest::Approx(20.0));
      CHECK(normal_upp[col] == doctest::Approx(20.0));
      // Wet clone: 50
      CHECK(wet_low[col] == doctest::Approx(50.0));
      CHECK(wet_upp[col] == doctest::Approx(50.0));
      // Original: still 5 (base scenario)
      CHECK(orig_low[col] == doctest::Approx(5.0));
      CHECK(orig_upp[col] == doctest::Approx(5.0));
    }
  }

  SUBCASE("both clones solve optimally after aperture update")
  {
    auto clone_normal = li.clone();
    auto clone_wet = li.clone();

    [[maybe_unused]] const auto ok1 = flow_lp.update_aperture_lp(
        clone_normal, base_scenario, normal_scenario, stage);
    [[maybe_unused]] const auto ok2 = flow_lp.update_aperture_lp(
        clone_wet, base_scenario, wet_scenario, stage);

    auto res_normal = clone_normal.resolve();
    auto res_wet = clone_wet.resolve();
    REQUIRE(res_normal.has_value());
    REQUIRE(res_wet.has_value());
    CHECK(res_normal.value() == 0);
    CHECK(res_wet.value() == 0);
  }

  SUBCASE("clone bounds are correct after solve")
  {
    // After solving the clone, re-read the bounds to verify they are still
    // the aperture values (the solver does not modify column bounds).
    auto clone = li.clone();
    const auto ok =
        flow_lp.update_aperture_lp(clone, base_scenario, wet_scenario, stage);
    REQUIRE(ok);

    auto res = clone.resolve();
    REQUIRE(res.has_value());

    // Bounds should still be 50.0 after solve
    const auto low = clone.get_col_low();
    const auto upp = clone.get_col_upp();
    for (const auto& [buid, col] : fcols) {
      CHECK(low[col] == doctest::Approx(50.0));
      CHECK(upp[col] == doctest::Approx(50.0));
    }
  }
}

// ─── 4. update_aperture_lp edge cases ───────────────────────────────────

TEST_CASE("FlowLP update_aperture_lp with inactive flow")  // NOLINT
{
  // An inactive flow should return true immediately (no-op).
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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
          .name = "j1",
          .drain = true,
      },
  };

  // Inactive flow
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inactive_flow",
          .active = OptActive {IntBool {0}},
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
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
      .name = "InactiveFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  const auto& base_scenario = scenarios[0];
  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];

  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];

  // update_aperture_lp on inactive flow should return true (no-op)
  auto clone = li.clone();
  const auto ok =
      flow_lp.update_aperture_lp(clone, base_scenario, base_scenario, stage);
  CHECK(ok);
}

// ─── 5. update_aperture_lp with non-matching scenario ───────────────────

TEST_CASE("FlowLP update_aperture_lp with non-matching scenario key")  // NOLINT
{
  // When the base_scenario UID doesn't match any registered flow_cols key,
  // update_aperture_lp should return true (no columns to update).

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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
          .name = "j1",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
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
      .name = "NonMatchScenarioTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];

  // Create a scenario with a different UID that won't match the flow_cols key
  const ScenarioLP fake_base(
      Scenario {
          .uid = Uid {999},
      },
      ScenarioIndex {0},
      SceneIndex {0});
  const ScenarioLP fake_aperture(
      Scenario {
          .uid = Uid {888},
      },
      ScenarioIndex {1},
      SceneIndex {0});

  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];

  auto clone = li.clone();
  // Should return true because no columns are registered for scenario 999
  const auto ok =
      flow_lp.update_aperture_lp(clone, fake_base, fake_aperture, stage);
  CHECK(ok);
}

// ─── 6. OptionsLP::sddp_aperture_directory ──────────────────────────────────

TEST_CASE("OptionsLP sddp_aperture_directory accessor")  // NOLINT
{
  SUBCASE("default is empty string")
  {
    const OptionsLP options;
    CHECK(options.sddp_aperture_directory().empty());
  }

  SUBCASE("set via SddpOptions")
  {
    Options opts;
    opts.sddp_options = SddpOptions {
        .aperture_directory = OptName {"/data/apertures"},
    };
    const OptionsLP options(opts);
    CHECK(options.sddp_aperture_directory() == "/data/apertures");
  }

  SUBCASE("empty when sddp_options present but directory not set")
  {
    Options opts;
    opts.sddp_options = SddpOptions {};
    const OptionsLP options(opts);
    CHECK(options.sddp_aperture_directory().empty());
  }
}

// ─── 7. End-to-end: multi-scenario flow with LP solve ───────────────────────

TEST_CASE("FlowLP aperture bound update affects LP objective value")  // NOLINT
{
  // Build a small hydro system where changing flow discharge via aperture
  // update measurably changes the LP objective value.
  // Higher inflow → more hydro generation → lower cost (hydro is cheaper).

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
          .capacity = 100.0,
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
          .name = "j1",
          .drain = true,
      },
  };

  // Two scenarios: low inflow (5 m³/s) and high inflow (50 m³/s)
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  5.0,
              },
          },  // scenario 1, stage 1, block 1
          {
              {
                  50.0,
              },
          },  // scenario 2, stage 1, block 1
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 200.0,
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
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
  };

  const System system = {
      .name = "ApertureObjTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};
  opts.scale_objective = OptReal {1.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto base_result = li.resolve();
  REQUIRE(base_result.has_value());
  const double base_obj = li.get_obj_value();

  // Access scenarios and stages
  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 2);

  const auto& base_scenario = scenarios[0];  // low inflow
  const auto& high_inflow_scenario = scenarios[1];  // high inflow

  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];

  // Clone and update to high inflow scenario
  auto clone = li.clone();
  const auto ok = flow_lp.update_aperture_lp(
      clone, base_scenario, high_inflow_scenario, stage);
  CHECK(ok);

  auto clone_result = clone.resolve();
  REQUIRE(clone_result.has_value());
  const double high_inflow_obj = clone.get_obj_value();

  // With higher inflow, more cheap hydro is available → lower cost
  // (or at minimum same cost if hydro capacity isn't binding)
  CHECK(high_inflow_obj <= base_obj + 1e-6);
}

// ─── 8. Explicit aperture_array in SDDP planning ───────────────────────────

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Create a 2-phase planning with 2 scenarios and explicit aperture_array.
auto make_2phase_aperture_planning() -> Planning
{
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = 2 * blocks_per_phase;

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

  // Two phases with aperture_set referencing aperture UIDs
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
          .aperture_set =
              {
                  1,
                  2,
              },
      },
  };

  // Two scenarios with different inflows
  // Scenario 1: 5 m³/s, Scenario 2: 15 m³/s
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

  // Explicit aperture definitions
  Array<Aperture> aperture_array = {
      {
          .uid = Uid {1},
          .name = OptName {"dry"},
          .source_scenario = Uid {1},
          .probability_factor = 0.6,
      },
      {
          .uid = Uid {2},
          .name = OptName {"wet"},
          .source_scenario = Uid {2},
          .probability_factor = 0.4,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
      .phase_array = std::move(phase_array),
      .aperture_array = std::move(aperture_array),
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  System system = {
      .name = "sddp_aperture_explicit",
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

TEST_CASE("SDDPSolver with explicit aperture_array converges")  // NOLINT
{
  auto planning = make_2phase_aperture_planning();
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 15;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = false;
  // num_apertures is not set — the solver should use the explicit
  // aperture_array from the simulation.

  SDDPSolver sddp(plp, sddp_opts);
  auto results = sddp.solve();

  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  SUBCASE("convergence within allowed iterations")
  {
    CHECK(results->back().converged);
  }

  SUBCASE("cuts were generated from apertures")
  {
    int total_cuts = 0;
    for (const auto& r : *results) {
      total_cuts += r.cuts_added;
    }
    CHECK(total_cuts > 0);
  }

  SUBCASE("lower and upper bounds are consistent")
  {
    const auto& last = results->back();
    CHECK(last.lower_bound > 0.0);
    CHECK(last.upper_bound > 0.0);
    if (last.converged) {
      CHECK(last.gap < 1e-3);
    }
  }
}

TEST_CASE(
    "SDDPSolver explicit aperture vs legacy num_apertures both work")  // NOLINT
{
  // Both approaches should produce valid results with cuts added.
  auto planning_explicit = make_2phase_aperture_planning();
  auto planning_legacy = make_2phase_aperture_planning();
  // Remove the explicit aperture_array from the legacy one
  planning_legacy.simulation.aperture_array.clear();

  SUBCASE("explicit aperture_array")
  {
    PlanningLP plp(std::move(planning_explicit));
    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 10;
    sddp_opts.convergence_tol = 1e-3;
    sddp_opts.enable_api = false;

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

  SUBCASE("legacy num_apertures=-1 (all scenarios)")
  {
    PlanningLP plp(std::move(planning_legacy));
    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 10;
    sddp_opts.convergence_tol = 1e-3;
    sddp_opts.enable_api = false;
    sddp_opts.num_apertures = -1;

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

// ─── 10. Aperture clone feasibility diagnostics ──────────────────────────────

TEST_CASE("Aperture clone LP feasibility diagnostics")  // NOLINT
{
  // Verify that aperture-updated clones are feasible by construction.
  // The LP is built with columns for ALL scenarios (each scenario has its own
  // flow columns in the junction balance).  update_aperture_lp only
  // changes the base scenario's flow column bounds — the other scenario
  // columns are unchanged.  The clone must remain feasible after the update,
  // because:
  //  1. Flow columns are fixed variables (lowb == uppb == discharge).
  //  2. Each scenario's junction balance sees only its own flow columns.
  //  3. Changing the base scenario's flow to any positive discharge should
  //     not violate the junction balance for OTHER scenarios.
  //
  // If the clone is infeasible, it indicates a structural issue: either the
  // junction balance constraint references wrong scenario columns, or
  // additional constraints (reservoir, turbine) are inconsistently scaled.

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
          .capacity = 200.0,
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
          .capacity = 100.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j1",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 200.0,
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

  // 3 scenarios with very different discharges
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  2.0,
                  2.0,
              },
          },  // scenario 1 (extreme dry)
          {
              {
                  50.0,
                  50.0,
              },
          },  // scenario 2 (normal)
          {
              {
                  150.0,
                  150.0,
              },
          },  // scenario 3 (extreme wet)
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
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
                  .duration = 1.0,
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
                  .uid = Uid {1},
                  .probability_factor = 0.25,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.50,
              },
              {
                  .uid = Uid {3},
                  .probability_factor = 0.25,
              },
          },
  };

  const System system = {
      .name = "ApertureFeasibilityDiag",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 3);

  const auto& base = scenarios[0];  // dry (2)
  const auto& normal = scenarios[1];  // normal (50)
  const auto& wet = scenarios[2];  // wet (150)

  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];

  SUBCASE("clone remains feasible after update to normal scenario")
  {
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(clone, base, normal, stage);
    REQUIRE(ok);

    auto res = clone.resolve();
    REQUIRE(res.has_value());
    CHECK(clone.is_optimal());
  }

  SUBCASE("clone remains feasible after update to wet scenario")
  {
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(clone, base, wet, stage);
    REQUIRE(ok);

    auto res = clone.resolve();
    REQUIRE(res.has_value());
    CHECK(clone.is_optimal());
  }

  SUBCASE("col_scale preserved through clone for all flow columns")
  {
    auto clone = li.clone();
    const auto& fcols_base = flow_lp.flow_cols_at(base, stage);
    const auto& fcols_norm = flow_lp.flow_cols_at(normal, stage);
    const auto& fcols_wet = flow_lp.flow_cols_at(wet, stage);

    // All flow columns should have scale = 1.0 in both original and clone
    for (const auto& [buid, col] : fcols_base) {
      CHECK(li.get_col_scale(col) == doctest::Approx(1.0));
      CHECK(clone.get_col_scale(col) == doctest::Approx(1.0));
    }
    for (const auto& [buid, col] : fcols_norm) {
      CHECK(li.get_col_scale(col) == doctest::Approx(1.0));
      CHECK(clone.get_col_scale(col) == doctest::Approx(1.0));
    }
    for (const auto& [buid, col] : fcols_wet) {
      CHECK(li.get_col_scale(col) == doctest::Approx(1.0));
      CHECK(clone.get_col_scale(col) == doctest::Approx(1.0));
    }
  }

  SUBCASE(
      "aperture update only changes base scenario columns — "
      "other scenarios untouched")
  {
    auto clone = li.clone();

    // Record original bounds for normal and wet scenario columns
    const auto orig_low = li.get_col_low();
    const auto orig_upp = li.get_col_upp();

    const auto& fcols_norm = flow_lp.flow_cols_at(normal, stage);
    const auto& fcols_wet = flow_lp.flow_cols_at(wet, stage);

    // Update base scenario columns to wet discharge
    const auto ok = flow_lp.update_aperture_lp(clone, base, wet, stage);
    REQUIRE(ok);

    // Base scenario columns should be updated to 150.0
    const auto clone_low = clone.get_col_low();
    const auto clone_upp = clone.get_col_upp();
    const auto& fcols_base = flow_lp.flow_cols_at(base, stage);
    for (const auto& [buid, col] : fcols_base) {
      CHECK(clone_low[col] == doctest::Approx(150.0));
      CHECK(clone_upp[col] == doctest::Approx(150.0));
    }

    // Normal scenario columns should be unchanged (still 50.0)
    for (const auto& [buid, col] : fcols_norm) {
      CHECK(clone_low[col] == doctest::Approx(orig_low[col]));
      CHECK(clone_upp[col] == doctest::Approx(orig_upp[col]));
    }

    // Wet scenario columns should be unchanged (still 150.0)
    for (const auto& [buid, col] : fcols_wet) {
      CHECK(clone_low[col] == doctest::Approx(orig_low[col]));
      CHECK(clone_upp[col] == doctest::Approx(orig_upp[col]));
    }
  }
}
