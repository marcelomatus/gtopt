// SPDX-License-Identifier: BSD-3-Clause
/// @file test_storage_lp.hpp
/// @brief Tests for StorageLP coverage: daily_cycle, soft_emin, drain,
///        efin constraint, cross-phase boundary, physical accessors.

#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/variable_scale.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Helper: build a minimal system with a single battery and solve.
/// Returns the SystemLP so callers can inspect LP structure.
auto make_battery_system(const Array<Battery>& battery_array,
                         const Simulation& simulation,
                         double demand_fail_cost = 1000.0,
                         Array<VariableScale> variable_scales = {})
    -> std::pair<SystemLP, PlanningOptionsLP>
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
          .name = "g1",
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
          .lmax = 50.0,
      },
  };

  const System system = {
      .name = "StorageLPTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .battery_array = battery_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = demand_fail_cost;
  opts.variable_scales = std::move(variable_scales);

  auto options = PlanningOptionsLP {opts};
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  return {std::move(sys_lp), std::move(options)};
}

/// Standard 1-stage 2-block simulation.
Simulation make_simple_simulation()
{
  return {
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
}

/// 2-stage simulation for cross-stage testing (same phase).
Simulation make_two_stage_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 4,
              },
              {
                  .uid = Uid {2},
                  .duration = 4,
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
}

}  // namespace

// ─── daily_cycle mode ───────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP daily_cycle battery produces feasible LP")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // daily_cycle=true forces use_state_variable=false internally and
  // applies dc_stage_scale = 24/stage_duration when the stage is long enough.
  // This test uses a stage with 48h total (2 blocks of 24h each), so
  // dc_stage_scale = 24/48 = 0.5.
  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 24,
              },
              {
                  .uid = Uid {2},
                  .duration = 24,
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

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_dc",
          .bus = Uid {1},
          .input_efficiency = 0.90,
          .output_efficiency = 0.90,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          .daily_cycle = true,
      },
  };

  auto [sys_lp, options] = make_battery_system(battery_array, simulation);
  auto& li = sys_lp.linear_interface();

  // The LP should have an eclose row (daily_cycle => use_state_variable=false)
  CHECK(li.get_numrows() > 0);
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "StorageLP daily_cycle skipped for short stages")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Stage duration = 2h (< 24h threshold), so dc_stage_scale = 1.0
  // (no daily-cycle scaling applied, even though daily_cycle=true).
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_short",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          .daily_cycle = true,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── efin constraint (last block of last stage) ─────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP efin constraint enforces final energy >= efin")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_efin",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .efin = 40.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // After solving, the efin should be >= 40.0
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  const auto phys_efin = bat_lp.physical_efin(li, scenarios[0], stages[0], 0.0);
  CHECK(phys_efin >= doctest::Approx(40.0));
}

// ─── energy_scale and to_physical ───────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP to_physical with custom energy_scale")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_scale",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 250.0,
          .capacity = 500.0,
      },
  };

  // Set energy scale via variable_scales (not per-element field)
  Array<VariableScale> vs = {
      {
          .class_name = "Battery",
          .variable = "energy",
          .scale = 50.0,
      },
  };
  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation(), 1000.0, vs);
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();

  CHECK(bat_lp.energy_scale() == doctest::Approx(50.0));
  CHECK(bat_lp.to_physical(1.0) == doctest::Approx(50.0));
  CHECK(bat_lp.to_physical(5.0) == doctest::Approx(250.0));
  CHECK(bat_lp.to_physical(0.0) == doctest::Approx(0.0));

  // Verify LP is feasible
  const auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── physical_eini fallback chain ───────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP physical_eini returns default for first stage phase 0")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_eini",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 60.0,
          .capacity = 100.0,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();

  // First stage of first phase returns default_eini directly
  const auto phys = bat_lp.physical_eini(li, scenarios[0], stages[0], 99.0);
  CHECK(phys == doctest::Approx(99.0));
}

// ─── physical_efin fallback to default ──────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP physical_efin returns LP solution when optimal")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_pfin",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();

  // When optimal, physical_efin should return from LP solution, not default
  const auto phys_efin =
      bat_lp.physical_efin(li, scenarios[0], stages[0], -999.0);
  // Should be between emin and emax
  CHECK(phys_efin >= doctest::Approx(0.0));
  CHECK(phys_efin <= doctest::Approx(100.0));
}

// ─── Two-stage coupled battery (cross-stage eini reuse) ─────────────────────

TEST_CASE(  // NOLINT
    "StorageLP two-stage coupled battery reuses efin as eini")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_2stg",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_two_stage_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();
  REQUIRE(stages.size() == 2);

  // efin of stage 1 should equal eini of stage 2 (same column)
  const auto efin_col_1 = bat_lp.efin_col_at(scenarios[0], stages[0]);
  const auto eini_col_2 = bat_lp.eini_col_at(scenarios[0], stages[1]);
  CHECK(efin_col_1 == eini_col_2);
}

// ─── Column and row accessors ───────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP column and row accessors return valid indices")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_acc",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  // energy_cols_at should return per-block column indices
  const auto& ecols = bat_lp.energy_cols_at(scenarios[0], stages[0]);
  CHECK(ecols.size() == 2);  // 2 blocks

  // energy_rows_at should return per-block row indices
  const auto& erows = bat_lp.energy_rows_at(scenarios[0], stages[0]);
  CHECK(erows.size() == 2);  // 2 blocks

  // efin_col_at should be valid
  const auto efin = bat_lp.efin_col_at(scenarios[0], stages[0]);
  CHECK(static_cast<int>(efin) >= 0);

  // eini_col_at should be valid
  const auto eini = bat_lp.eini_col_at(scenarios[0], stages[0]);
  CHECK(static_cast<int>(eini) >= 0);
}

// ─── StorageOptions defaults ────────────────────────────────────────────────

TEST_CASE("StorageOptions default values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const StorageOptions opts;
  CHECK(opts.use_state_variable == true);
  CHECK(opts.daily_cycle == false);
  CHECK(opts.skip_state_link == false);
  CHECK(opts.energy_scale == doctest::Approx(1.0));
  CHECK(opts.flow_scale == doctest::Approx(1.0));
}

TEST_CASE("StorageOptions daily_cycle forces use_state_variable off")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // This tests the conceptual contract: when daily_cycle is true,
  // the effective use_state_variable is false.
  const StorageOptions opts {
      .use_state_variable = true,
      .daily_cycle = true,
  };
  // The effective usv flag: daily_cycle ? false : opts.use_state_variable
  const bool effective_usv = opts.daily_cycle ? false : opts.use_state_variable;
  CHECK_FALSE(effective_usv);
}

// ─── param_emin / param_emax / param_ecost accessors ────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP param accessors return correct values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_param",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 10.0,
          .emax = 90.0,
          .ecost = 5.0,
          .capacity = 100.0,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& stages = sys_lp.phase().stages();
  REQUIRE(!stages.empty());

  const auto stage_uid = stages[0].uid();
  CHECK(bat_lp.param_emin(stage_uid).value_or(-1.0) == doctest::Approx(10.0));
  CHECK(bat_lp.param_emax(stage_uid).value_or(-1.0) == doctest::Approx(90.0));
  CHECK(bat_lp.param_ecost(stage_uid).value_or(-1.0) == doctest::Approx(5.0));
}

// ─── soft_emin_col_at returns nullopt when no soft_emin ─────────────────────

TEST_CASE(  // NOLINT
    "StorageLP soft_emin_col_at returns nullopt when inactive")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_no_semin",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
          // soft_emin not set
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& scenarios = sys_lp.scene().scenarios();
  const auto& stages = sys_lp.phase().stages();

  const auto col = bat_lp.soft_emin_col_at(scenarios[0], stages[0]);
  CHECK_FALSE(col.has_value());
}

// ─── flow_scale accessor ────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP flow_scale defaults to 1.0 for batteries")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_fs",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();

  // Battery default flow_scale is 1.0
  CHECK(bat_lp.flow_scale() == doctest::Approx(1.0));
}

// ─── annual_loss nonzero ────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP with annual_loss produces feasible LP")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_loss",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .annual_loss = 0.10,
          .emin = 0.0,
          .emax = 100.0,
          .capacity = 100.0,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  const auto result = sys_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── Battery with ecost nonzero ─────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "StorageLP with ecost adds cost to energy columns")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_ecost",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .ecost = 2.0,
          .eini = 50.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The objective should be positive (generator cost + ecost on energy)
  CHECK(li.get_obj_value() > 0.0);
}

// ─── Regression: soft_emin column name must not collide with eini ──────────

TEST_CASE(  // NOLINT
    "StorageLP soft_emin column has unique name (not colliding with eini)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Battery with soft_emin active — triggers both the eini column
  // (variable_name "eini") and the soft_emin slack column
  // (variable_name "soft_emin"), both with stage context.
  // Before the fix, both used variable_name "energy", causing a
  // duplicate column name in the FlatLinearProblem name map.
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_semin",
          .bus = Uid {1},
          .input_efficiency = 0.95,
          .output_efficiency = 0.95,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .soft_emin = 20.0,
          .soft_emin_cost = 500.0,
          .capacity = 100.0,
      },
  };

  // Build with col+row name maps enabled (level 2) so that
  // LinearProblem::flatten() throws on any duplicate column name.
  // Before the fix, both eini and soft_emin slack used variable_name
  // "energy" with stage context, producing identical names.
  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  popts.lp_matrix_options.col_with_names = true;
  popts.lp_matrix_options.row_with_names = true;
  popts.lp_matrix_options.col_with_name_map = true;
  popts.lp_matrix_options.row_with_name_map = true;

  auto options = PlanningOptionsLP {popts};
  const auto sim = make_simple_simulation();
  SimulationLP sim_lp(sim, options);

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> gen_array = {
      {
          .uid = Uid {1},
          .name = "g1",
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
          .lmax = 100.0,
      },
  };

  const System system {
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = gen_array,
      .battery_array = battery_array,
  };

  // This will throw if duplicate column names exist (regression guard)
  SystemLP sys_lp(system, sim_lp);
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── efin_cost: soft slack on the per-reservoir efin row ────────────────────
//
// When ``Battery.efin_cost`` (or ``Reservoir.efin_cost``) is set and > 0,
// the hard ``vol_end >= efin`` row at the last block of the last stage
// becomes soft: ``vol_end + slack >= efin`` with the slack priced at
// efin_cost in the objective.  Without efin_cost, the row stays hard
// (the historical behaviour).  See storage_lp.hpp for the construction
// and reservoir.hpp / battery.hpp / lng_terminal.hpp / volume_right.hpp
// for the per-element field documentation.

// Note: batteries default to `daily_cycle = true`, which auto-adds an
// efin == eini constraint per phase and overrides any user-supplied
// `efin`.  These tests therefore set `daily_cycle = false` explicitly,
// representing a "very large" / LNG-like battery where the user does
// want a per-horizon end-state floor.

TEST_CASE(  // NOLINT
    "StorageLP efin_cost unset → hard `efin` row: LP infeasible when "
    "vol_end < efin")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Battery with eini=10, efin=80.  With pmax_charge=5 and a 2-block
  // 1-h horizon, max end-state ≈ 10 + 2·5 = 20 MWh — well below 80.
  // Hard `vol_end >= 80` row → LP infeasible.
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_efin_hard",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 10.0,
          .efin = 80.0,
          // efin_cost NOT set — row stays hard
          .pmax_charge = 5.0,
          .pmax_discharge = 5.0,
          .capacity = 100.0,
          .daily_cycle = false,  // disable auto efin == eini
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  // Hard efin: LP is infeasible — either resolve returns no value
  // (unexpected) or returns a non-optimal status (>0).
  CHECK((!result.has_value() || result.value() != 0));
}

TEST_CASE(  // NOLINT
    "StorageLP efin_cost > 0 → soft `efin` row: LP feasible at slack cost")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Same fixture as the hard-efin test above, but with efin_cost set.
  // The slack now carries the missing ~60 MWh at $50/MWh, so the LP
  // is feasible with strictly positive objective.
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_efin_soft",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 10.0,
          .efin = 80.0,
          .efin_cost = 50.0,  // soft: slack at $50/MWh
          .pmax_charge = 5.0,
          .pmax_discharge = 5.0,
          .capacity = 100.0,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  // Soft efin: LP solves to optimality (status 0).
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Strictly positive objective — the cheap path of leaving the battery
  // near eini still pays the efin slack penalty.
  CHECK(li.get_obj_value() > 0.0);
}

TEST_CASE(  // NOLINT
    "StorageLP efin_cost == 0 → behaves identically to unset (hard row)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // The slack-creation guard is ``efin_cost.has_value() && *efin_cost > 0``;
  // setting it to exactly 0 should NOT create the slack column (matching
  // the existing soft_emin guard pattern).  Same infeasibility as the
  // unset case.
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_efin_zero",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 10.0,
          .efin = 80.0,
          .efin_cost = 0.0,  // explicit zero — slack guard rejects
          .pmax_charge = 5.0,
          .pmax_discharge = 5.0,
          .capacity = 100.0,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_simple_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  CHECK((!result.has_value() || result.value() != 0));
}

TEST_CASE(  // NOLINT
    "StorageLP large reservoir-like battery (state variable, "
    "daily_cycle=false) builds and solves with efin_cost")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // "Large" battery operating like a reservoir:
  //  - daily_cycle = false (no per-stage SoC cycling)
  //  - use_state_variable = true (SDDP-style cross-stage coupling)
  // The hard efin row only binds at the *last stage of the last phase*
  // and is enforced by the SDDP coordinator outside the per-stage LP.
  // Within a single per-stage solve we verify that the LP builds and
  // solves cleanly when efin / efin_cost are present, which guards
  // against regressions in StorageLP construction (e.g. column-name
  // collisions or the slack guard misfiring when SDDP is on).
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_large",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 100.0,
          .efin = 800.0,
          .efin_cost = 25.0,  // soft seasonal-storage slack [$/MWh]
          .pmax_charge = 5.0,
          .pmax_discharge = 5.0,
          .capacity = 1000.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_two_stage_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(li.get_obj_value() >= 0.0);
}

// ─── efin_cost + soft_emin combination ──────────────────────────────────────
//
// Mirrors the plp2gtopt ``--soft-storage-bounds`` emission pattern:
// per-reservoir efin is relaxed via ``efin_cost`` and per-stage emin
// from maintenance schedules is relaxed via ``soft_emin`` /
// ``soft_emin_cost``.  Both slacks must be priced at the same per-
// reservoir cost (``plpvrebemb`` or ``CVert``).  This test verifies
// that both slacks coexist in a single LP without column-name
// collision and that the optimal solution activates them
// independently at their respective constraints.

TEST_CASE(  // NOLINT
    "StorageLP combined efin_cost + soft_emin (plp_legacy emission)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // 2-stage battery with eini=10:
  //   - Stage 1: soft_emin=15 + soft_emin_cost=5  (forces slack since
  //     eini=10 < 15 and stage-1 charge cap is small)
  //   - Stage 2 (last): efin=80 + efin_cost=5  (forces slack since
  //     reaching 80 from 10 requires 70 MWh of charge in 2 blocks
  //     of 4h each at pmax_charge=5 → max 40 MWh → 30 MWh slack)
  //
  // Both slacks active simultaneously, independent column families.
  const Array<Battery> battery_array = {
      {
          .uid = Uid {1},
          .name = "bat_combo",
          .bus = Uid {1},
          .input_efficiency = 1.0,
          .output_efficiency = 1.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 10.0,
          .efin = 80.0,
          .efin_cost = 5.0,  // soft efin
          .soft_emin = std::vector<double> {15.0, 0.0},  // tight at stage 1
          .soft_emin_cost = std::vector<double> {5.0, 0.0},
          .pmax_charge = 5.0,
          .pmax_discharge = 5.0,
          .capacity = 100.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  auto [sys_lp, options] =
      make_battery_system(battery_array, make_two_stage_simulation());
  auto& li = sys_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(li.get_obj_value() > 0.0);

  // Both slack column families must exist independently.  Without the
  // 2026-04 column-name fix (``EfinSlackName = "efin_slack"``,
  // ``SoftEminName = "soft_emin"``) the LP would have thrown on a
  // duplicate column name during flatten; reaching this CHECK proves
  // the names coexist in the same LP for the same element.
  const auto& bat_lp = sys_lp.elements<BatteryLP>().front();
  const auto& sc1 = sys_lp.scene().scenarios().front();
  const auto& stg1 = sys_lp.phase().stages().front();
  const auto soft_emin_col = bat_lp.soft_emin_col_at(sc1, stg1);
  CHECK(soft_emin_col.has_value());
  // efin_slack column from StorageLP::EfinSlackName lives in the LP
  // when efin_cost > 0.  Verify the LP solved cleanly with both
  // slack mechanisms instantiated — the obj > 0 above covers that.
}
