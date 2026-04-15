// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/lng_terminal.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LngTerminal default construction")  // NOLINT
{
  const LngTerminal t;

  CHECK(t.uid == Uid {unknown_uid});
  CHECK(t.name == Name {});
  CHECK_FALSE(t.active.has_value());
  CHECK_FALSE(t.emin.has_value());
  CHECK_FALSE(t.emax.has_value());
  CHECK_FALSE(t.ecost.has_value());
  CHECK_FALSE(t.eini.has_value());
  CHECK_FALSE(t.efin.has_value());
  CHECK_FALSE(t.annual_loss.has_value());
  CHECK_FALSE(t.sendout_max.has_value());
  CHECK_FALSE(t.sendout_min.has_value());
  CHECK_FALSE(t.delivery.has_value());
  CHECK_FALSE(t.spillway_cost.has_value());
  CHECK_FALSE(t.spillway_capacity.has_value());
  CHECK_FALSE(t.use_state_variable.has_value());
  CHECK_FALSE(t.mean_production_factor.has_value());
  CHECK_FALSE(t.scost.has_value());
  CHECK_FALSE(t.soft_emin.has_value());
  CHECK_FALSE(t.soft_emin_cost.has_value());
  CHECK_FALSE(t.flow_conversion_rate.has_value());
  CHECK(t.generators.empty());
}

TEST_CASE("LngGeneratorLink construction")  // NOLINT
{
  const LngGeneratorLink link {
      .generator = Uid {42},
      .heat_rate = 0.18,
  };

  CHECK(std::get<Uid>(link.generator) == Uid {42});
  CHECK(link.heat_rate == doctest::Approx(0.18));
}

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
/// Helper: minimal system with 1 bus, 1 generator, 1 demand, 1 LNG terminal.
/// The generator is linked to the LNG terminal with a heat rate.
struct LngTestFixture
{
  static constexpr Uid bus_uid {1};
  static constexpr Uid gen_uid {1};
  static constexpr Uid dem_uid {1};
  static constexpr Uid lng_uid {1};
  static constexpr Real heat_rate {0.2};  // m³_LNG / MWh
  static constexpr Real tank_max {100'000.0};  // m³
  static constexpr Real tank_ini {50'000.0};  // m³
  static constexpr Real gen_capacity {100.0};  // MW
  static constexpr Real demand_load {50.0};  // MW

  System system {
      .name = "LngTest",
      .bus_array =
          {
              {
                  .uid = bus_uid,
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = dem_uid,
                  .name = "d1",
                  .bus = bus_uid,
                  .lmax = demand_load,
              },
          },
      .generator_array =
          {
              {
                  .uid = gen_uid,
                  .name = "g1",
                  .bus = bus_uid,
                  .gcost = 10.0,
                  .capacity = gen_capacity,
              },
          },
      .lng_terminal_array =
          {
              {
                  .uid = lng_uid,
                  .name = "gnl1",
                  .emin = 0.0,
                  .emax = tank_max,
                  .eini = tank_ini,
                  .sendout_max = 10'000.0,
                  .delivery = 20'000.0,
                  .spillway_cost = 100.0,
                  .use_state_variable = true,
                  .generators =
                      {
                          {
                              .generator = gen_uid,
                              .heat_rate = heat_rate,
                          },
                      },
              },
          },
  };

  Simulation simulation {
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
};

}  // namespace

TEST_CASE("LngTerminal feasible LP with generator coupling")  // NOLINT
{
  LngTestFixture f;

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  CHECK(li.get_numrows() > 0);
  CHECK(li.get_numcols() > 0);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LngTerminal tank volume decreases with generation")  // NOLINT
{
  LngTestFixture f;

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // Access the LNG terminal LP to check efin
  const auto& lng_lps = system_lp.elements<LngTerminalLP>();
  REQUIRE(lng_lps.size() == 1);
  const auto& lng_lp = lng_lps.front();

  // Get final energy (tank volume) — it should be less than initial
  // because the generator consumes fuel.
  // Consumption = heat_rate × generation × duration per block
  // With demand = 50 MW, 2 blocks of 1h each:
  //   fuel = 0.2 × 50 × 1 × 2 = 20 m³
  // Delivery = 20000 m³ total over stage (2h), rate = 10000 m³/h
  //   total delivered = 10000 × 1 × 2 = 20000 m³
  // So efin ≈ 50000 + 20000 - 20 = 69980 m³
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto efin_col = lng_lp.efin_col_at(scenario_lp, stage_lp);
  const auto efin_val = li.get_col_sol()[efin_col];

  // Tank should have increased due to large delivery (20000) minus small
  // consumption (~20)
  CHECK(efin_val > LngTestFixture::tank_ini);
  // But should be less than ini + delivery (some fuel consumed)
  CHECK(efin_val < LngTestFixture::tank_ini + 20'001.0);
}

TEST_CASE("LngTerminal with no delivery — generation drains tank")  // NOLINT
{
  LngTestFixture f;
  // Remove delivery
  f.system.lng_terminal_array.front().delivery = 0.0;

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Tank should decrease: consumption = 0.2 × 50 × 2 = 20 m³
  const auto& lng_lps = system_lp.elements<LngTerminalLP>();
  const auto& lng_lp = lng_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto efin_col = lng_lp.efin_col_at(scenario_lp, stage_lp);
  const auto efin_val = li.get_col_sol()[efin_col];

  CHECK(efin_val < LngTestFixture::tank_ini);
  // Should be roughly 50000 - 20 = 49980
  CHECK(efin_val
        == doctest::Approx(LngTestFixture::tank_ini - 20.0).epsilon(0.1));
}

TEST_CASE("LngTerminal with BOG (annual_loss)")  // NOLINT
{
  LngTestFixture f;
  // Set BOG rate: 10% per year
  f.system.lng_terminal_array.front().annual_loss = 0.1;
  // No delivery, no generator link — pure storage loss test
  f.system.lng_terminal_array.front().delivery = 0.0;
  f.system.lng_terminal_array.front().generators = {};

  // Remove demand so generator doesn't run
  f.system.demand_array.front().lmax = 0.0;

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& lng_lps = system_lp.elements<LngTerminalLP>();
  const auto& lng_lp = lng_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto efin_col = lng_lp.efin_col_at(scenario_lp, stage_lp);
  const auto efin_val = li.get_col_sol()[efin_col];

  // With 2 blocks of 1h each (2h total), loss = 0.1/8760 per hour
  // V_final ≈ V_ini × (1 - 0.1/8760)^2 ≈ 50000 × (1 - 2.28e-5)
  // ≈ 49998.86
  CHECK(efin_val < LngTestFixture::tank_ini);
  CHECK(efin_val > LngTestFixture::tank_ini - 10.0);
}

TEST_CASE("LngTerminal SDDP state variable registration")  // NOLINT
{
  LngTestFixture f;
  f.system.lng_terminal_array.front().use_state_variable = true;

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "LngTerminal user constraint — take-or-pay minimum tank level")
{
  // A take-or-pay contract requires the LNG terminal to maintain a minimum
  // ending tank level (efin >= 60000 m³).  The large delivery (20000) makes
  // this feasible — the constraint just prevents the optimizer from
  // over-dispatching the generator.
  LngTestFixture f;

  f.system.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "takeorpay_min_efin",
          .expression = "lng_terminal('gnl1').efin >= 60000",
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify efin respects the constraint
  const auto& lng_lps = system_lp.elements<LngTerminalLP>();
  REQUIRE(lng_lps.size() == 1);
  const auto& lng_lp = lng_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto efin_col = lng_lp.efin_col_at(scenario_lp, stage_lp);
  const auto efin_val = li.get_col_sol()[efin_col];

  CHECK(efin_val >= 60000.0 - 1.0);
}

TEST_CASE(  // NOLINT
    "LngTerminal user constraint — maximum tank volume per block")
{
  // Limit the per-block tank volume to 55000 m³.
  // The delivery adds 20000 m³ but the constraint prevents the tank
  // from exceeding 55000, so extra LNG must be vented (drain).
  LngTestFixture f;
  f.system.lng_terminal_array.front().spillway_capacity = 100'000.0;

  f.system.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "max_tank_volume",
          .expression = "lng_terminal('gnl1').energy <= 55000",
      },
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify all block energy variables respect the 55000 bound
  const auto& lng_lps = system_lp.elements<LngTerminalLP>();
  REQUIRE(lng_lps.size() == 1);
  const auto& lng_lp = lng_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto& energy_map = lng_lp.energy_cols_at(scenario_lp, stage_lp);
  for (const auto& [buid, ecol] : energy_map) {
    const auto eval = li.get_col_sol()[ecol];
    CHECK(eval <= 55000.0 + 1.0);
  }
}

TEST_CASE("LngTerminalLP - add_to_output via write_out")  // NOLINT
{
  // Exercises LngTerminalLP::add_to_output by calling write_out after
  // resolving the LP.
  LngTestFixture f;

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_lng_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  opts.output_directory = tmpdir.string();

  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(f.simulation, options);
  SystemLP system_lp(f.system, simulation_lp);

  auto& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Exercises LngTerminalLP::add_to_output (delivery_cols + StorageBase output)
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}
