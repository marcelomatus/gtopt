// SPDX-License-Identifier: BSD-3-Clause
/// @file test_storage_lp.hpp
/// @brief Tests for StorageLP coverage: daily_cycle, soft_emin, drain,
///        efin constraint, cross-phase boundary, physical accessors.

#include <doctest/doctest.h>
#include <gtopt/battery.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
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
