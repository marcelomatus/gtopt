/**
 * @file      test_state_variable_loading.hpp
 * @brief     Unit tests for state variable loading and initialization in
 *            multi-phase SDDP contexts
 * @date      2026-04-01
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. State variable registration: efin keys in state_variables map
 *  2. Cross-phase dependent variable links (efin → sini)
 *  3. Save/load round-trip preserves actual column values
 *  4. Physical eini/efin warm-start fallback after load_state
 *  5. Battery multi-phase SDDP with use_state_variable=true
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_state_io.hpp>
#include <gtopt/system_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── 1. State variable registration in multi-phase planning ─────────────────

TEST_CASE(  // NOLINT
    "State variables: reservoir efin registered for each phase")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto& sim = planning_lp.simulation();
  const auto num_phases = static_cast<int>(sim.phases().size());
  REQUIRE(num_phases == 3);

  // The reservoir (uid=1, class "rsv") should have an efin state
  // variable registered for each (scene=0, phase) combination because
  // the default reservoir use_state_variable is true.
  const auto scene = first_scene_index();
  int efin_found = 0;

  for (int pi = 0; pi < num_phases; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
    for (const auto& [key, svar] : sv_map) {
      if (key.col_name == "efin" && key.class_name == "Reservoir"
          && key.uid == Uid {1})
      {
        ++efin_found;
        // The column index should be valid (non-negative)
        CHECK(svar.col() >= 0);
      }
    }
  }

  // efin should be registered in all 3 phases
  CHECK(efin_found == 3);
}

// ─── 2. Cross-phase dependent variable links ────────────────────────────────

TEST_CASE(  // NOLINT
    "State variables: efin has dependent variable linking to next phase sini")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto& sim = planning_lp.simulation();
  const auto scene = first_scene_index();

  // Phase 0's efin state variable should have a dependent variable that
  // points to Phase 1's sini column. Similarly for Phase 1 → Phase 2.
  for (int pi = 0; pi < 2; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});

    bool found_efin_with_dep = false;
    for (const auto& [key, svar] : sv_map) {
      if (key.col_name == "efin" && key.class_name == "Reservoir"
          && key.uid == Uid {1})
      {
        const auto deps = svar.dependent_variables();
        // Should have at least one dependent variable pointing to next phase
        CHECK_FALSE(deps.empty());
        if (!deps.empty()) {
          // The dependent variable should target the next phase
          CHECK(deps[0].phase_index() == PhaseIndex {pi + 1});
          CHECK(deps[0].scene_index() == scene);
          CHECK(deps[0].col() >= 0);
          found_efin_with_dep = true;
        }
      }
    }
    CHECK(found_efin_with_dep);
  }

  // Phase 2 (last phase) efin should have NO dependent variables
  // (there is no phase 3 to link to)
  {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {2});
    for (const auto& [key, svar] : sv_map) {
      if (key.col_name == "efin" && key.class_name == "Reservoir"
          && key.uid == Uid {1})
      {
        CHECK(svar.dependent_variables().empty());
      }
    }
  }
}

// ─── 3. Save/load round-trip preserves actual column values ─────────────────

TEST_CASE(  // NOLINT
    "State variable loading: round-trip preserves reservoir efin values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Solve with SDDP to get non-trivial state variable values
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto dir =
      std::filesystem::temp_directory_path() / "gtopt_test_sv_loading_rt";
  std::filesystem::create_directories(dir);
  const auto filepath = (dir / "state_values.csv").string();

  // Save state with solved values
  auto save_result = save_state_csv(planning_lp, filepath, IterationIndex {0});
  REQUIRE(save_result.has_value());

  // Collect reference values from the solved LP before loading
  const auto& sim = planning_lp.simulation();
  struct RefValue
  {
    SceneIndex scene_index;
    PhaseIndex phase_index;
    std::string name;
    double value;
  };
  std::vector<RefValue> ref_values;

  for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
      const auto& li = planning_lp.system(si, pi).linear_interface();
      if (!li.is_optimal()) {
        continue;
      }
      const auto col_sol = li.get_col_sol();
      const auto& names = li.col_index_to_name();
      const auto ncols = static_cast<size_t>(li.get_numcols());

      for (size_t c = 0; c < ncols && c < names.size(); ++c) {
        const auto ci = ColIndex {static_cast<int>(c)};
        if (names[ci].empty()) {
          continue;
        }
        const auto phys_val = col_sol[ci];
        // Only track non-zero values for comparison
        if (std::abs(phys_val) > 1e-12) {
          ref_values.push_back(RefValue {
              .scene_index = si,
              .phase_index = pi,
              .name = names[ci],
              .value = phys_val,
          });
        }
      }
    }
  }
  REQUIRE_FALSE(ref_values.empty());

  // Load into a fresh PlanningLP
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  auto load_result = load_state_csv(planning_lp2, filepath);
  REQUIRE(load_result.has_value());

  // Verify: for each reference value, the warm column solution in the
  // fresh LP should have the same value (in LP units)
  int matched = 0;
  for (const auto& ref : ref_values) {
    const auto& li = planning_lp2.system(ref.scene_index, ref.phase_index)
                         .linear_interface();
    const auto& warm = li.warm_col_sol();
    if (warm.empty()) {
      continue;
    }
    const auto& col_map = li.col_name_map();
    auto cit = col_map.find(ref.name);
    if (cit == col_map.end()) {
      continue;
    }

    const auto col_idx = static_cast<size_t>(cit->second);
    if (col_idx >= warm.size()) {
      continue;
    }

    const auto scale = li.get_col_scale(ColIndex {static_cast<int>(col_idx)});
    const double loaded_phys =
        warm[ColIndex {static_cast<int>(col_idx)}] * scale;

    // Physical values should match within tolerance
    CHECK(loaded_phys == doctest::Approx(ref.value).epsilon(1e-6));
    ++matched;
  }

  // At least some values should have matched
  CHECK(matched > 0);

  std::filesystem::remove_all(dir);
}

// ─── 4. Physical eini/efin warm-start fallback ──────────────────────────────

TEST_CASE(  // NOLINT
    "State variable loading: physical_efin uses warm solution when LP unsolved")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Solve to get values
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // Capture the physical efin from the solved LP for phase 0
  const auto scene = first_scene_index();
  const auto phase0 = first_phase_index();
  const auto& sys0 = planning_lp.system(scene, phase0);
  const auto& rsv_lp = sys0.elements<ReservoirLP>().front();
  const auto& scens = planning_lp.simulation().scenarios();
  const auto& phase0_stages = planning_lp.simulation().phases()[0].stages();
  REQUIRE_FALSE(scens.empty());
  REQUIRE_FALSE(phase0_stages.empty());

  const double solved_efin = rsv_lp.physical_efin(
      sys0.linear_interface(), scens[0], phase0_stages.back(), -1.0);
  // The solved efin should be a non-trivial value (reservoir has capacity 500,
  // starts at 250 with inflow)
  CHECK(solved_efin >= 0.0);
  CHECK(solved_efin <= 500.0);

  // Save and load into fresh LP
  const auto dir =
      std::filesystem::temp_directory_path() / "gtopt_test_sv_loading_warm";
  std::filesystem::create_directories(dir);
  const auto filepath = (dir / "state_warm.csv").string();

  auto save_result = save_state_csv(planning_lp, filepath, IterationIndex {0});
  REQUIRE(save_result.has_value());

  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  auto load_result = load_state_csv(planning_lp2, filepath);
  REQUIRE(load_result.has_value());

  // The fresh LP is NOT solved, so physical_efin should use the warm solution
  const auto& sys0_fresh = planning_lp2.system(scene, phase0);
  const auto& rsv_lp_fresh = sys0_fresh.elements<ReservoirLP>().front();
  const auto& li_fresh = sys0_fresh.linear_interface();

  CHECK_FALSE(li_fresh.is_optimal());
  CHECK_FALSE(li_fresh.warm_col_sol().empty());

  const auto& scens2 = planning_lp2.simulation().scenarios();
  const auto& ph0_stages2 = planning_lp2.simulation().phases()[0].stages();
  REQUIRE_FALSE(scens2.empty());
  REQUIRE_FALSE(ph0_stages2.empty());

  const double warm_efin =
      rsv_lp_fresh.physical_efin(li_fresh, scens2[0], ph0_stages2.back(), -1.0);

  // The warm efin should match the solved value (within tolerance)
  CHECK(warm_efin == doctest::Approx(solved_efin).epsilon(1e-6));

  std::filesystem::remove_all(dir);
}

// ─── 5. Battery with use_state_variable=true in multi-phase ──────────────────

TEST_CASE(  // NOLINT
    "State variable loading: battery with use_state_variable=true registers "
    "efin and links across phases")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Build a 2-phase planning with a battery that has use_state_variable=true
  Array<Block> block_array;
  for (int i = 0; i < 4; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 2,
      },
      Stage {
          .uid = Uid {2},
          .first_block = 2,
          .count_block = 2,
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
          .lmax = 50.0,
      },
  };

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat1",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 50.0,
          .use_state_variable = true,
          .daily_cycle = false,
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

  PlanningOptions options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};

  System system = {
      .name = "bat_sv_test",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  Planning planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };

  PlanningLP planning_lp(std::move(planning));

  const auto& sim = planning_lp.simulation();
  const auto scene = first_scene_index();

  // Verify battery efin state variable is registered in both phases
  int bat_efin_count = 0;
  for (int pi = 0; pi < 2; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
    for (const auto& [key, svar] : sv_map) {
      if (key.col_name == "efin" && key.class_name == "Battery"
          && key.uid == Uid {1})
      {
        ++bat_efin_count;
        CHECK(svar.col() >= 0);
      }
    }
  }
  CHECK(bat_efin_count == 2);

  // Verify phase 0 efin has dependent variable pointing to phase 1
  {
    const auto& sv_map0 = sim.state_variables(scene, first_phase_index());
    for (const auto& [key, svar] : sv_map0) {
      if (key.col_name == "efin" && key.class_name == "Battery"
          && key.uid == Uid {1})
      {
        const auto deps = svar.dependent_variables();
        CHECK_FALSE(deps.empty());
        if (!deps.empty()) {
          CHECK(deps[0].phase_index() == PhaseIndex {1});
        }
      }
    }
  }

  // Phase 1 (last phase) efin should have no dependent variables
  {
    const auto& sv_map1 = sim.state_variables(scene, PhaseIndex {1});
    for (const auto& [key, svar] : sv_map1) {
      if (key.col_name == "efin" && key.class_name == "Battery"
          && key.uid == Uid {1})
      {
        CHECK(svar.dependent_variables().empty());
      }
    }
  }
}

// ─── 6. Malformed state file gracefully ignored ──────────────────────────────

TEST_CASE(  // NOLINT
    "State variable loading: malformed CSV lines are silently skipped")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto dir =
      std::filesystem::temp_directory_path() / "gtopt_test_sv_malformed";
  std::filesystem::create_directories(dir);
  const auto filepath = (dir / "state_bad.csv").string();

  // Write a file with valid header but invalid data lines
  {
    std::ofstream ofs(filepath);
    ofs << "name,phase,scene,value,rcost\n";
    // Missing fields
    ofs << "partial_col,1\n";
    // Non-numeric value
    ofs << "bad_col,1,1,not_a_number,0.0\n";
    // Unknown phase UID
    ofs << "some_col,999,1,100.0,0.0\n";
    // Unknown scene UID
    ofs << "some_col,1,999,100.0,0.0\n";
    // Empty line
    ofs << "\n";
    // Comment line
    ofs << "# this is a comment\n";
  }

  auto load_result = load_state_csv(planning_lp, filepath);
  // Loading should succeed (malformed lines are skipped)
  REQUIRE(load_result.has_value());

  // No warm solutions should be injected (no valid data)
  const auto& sim = planning_lp.simulation();
  for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
      const auto& li = planning_lp.system(si, pi).linear_interface();
      CHECK(li.warm_col_sol().empty());
    }
  }

  std::filesystem::remove_all(dir);
}
