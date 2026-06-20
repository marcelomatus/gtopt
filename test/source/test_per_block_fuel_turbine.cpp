// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_per_block_fuel_turbine.cpp
 * @brief     Per-block Fuel.price and Turbine.production_factor read tests
 * @date      2026-06-20
 * @copyright BSD-3-Clause
 *
 * `Fuel.price` and `Turbine.production_factor` (and `Turbine.capacity`)
 * were upgraded from per-stage (`OptTRealFieldSched`) to per-(stage,
 * block) (`OptTBRealFieldSched`).  These tests pin that the LP build
 * reads the per-block sample — a value that differs across blocks flips
 * dispatch / changes the conversion coefficient accordingly — and that
 * the scalar form still broadcasts (back-compat).
 *
 * The two-block fixture mirrors `test_fuel_offtake.cpp`: a single stage
 * with two unit-duration blocks (uids 0, 1) and `chronological = false`,
 * so the per-(stage, block) inline 2-D matrix `{{b0, b1}}` resolves
 * cleanly during the direct-struct LP build.
 */

#include <doctest/doctest.h>
#include <gtopt/fuel.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace per_block_fuel_turbine_test  // NOLINT
{

// One stage, two unit-duration blocks (uids 0, 1), one scenario.
[[nodiscard]] Simulation make_pbft_two_block_simulation()
{
  return {
      .block_array =
          {
              {.uid = Uid {0}, .duration = 1.0},
              {.uid = Uid {1}, .duration = 1.0},
          },
      .stage_array =
          {
              {
                  .uid = Uid {0},
                  .first_block = 0,
                  .count_block = 2,
                  .chronological = false,
              },
          },
      .scenario_array = {{.uid = Uid {0}}},
  };
}

// Locate a generator's dispatch column for (s, t, block_index) and
// return its primal value (or nullopt when missing).
[[nodiscard]] std::optional<Real> pbft_generation_of(SystemLP& system_lp,
                                                     std::string_view gen_name,
                                                     std::size_t block_index)
{
  const auto& gens = system_lp.elements<GeneratorLP>();
  const auto it = std::ranges::find_if(
      gens, [&](const auto& g) { return g.generator().name == gen_name; });
  if (it == gens.end()) {
    return std::nullopt;
  }
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& gen_cols = it->generation_cols_at(s, t);
  const auto buid = t.blocks()[block_index].uid();
  const auto col_it = gen_cols.find(buid);
  if (col_it == gen_cols.end()) {
    return std::nullopt;
  }
  return system_lp.linear_interface().get_col_sol()[col_it->second];
}

}  // namespace per_block_fuel_turbine_test

// ── Fuel.price per-block flips merit order across blocks ──────────────

TEST_CASE("Fuel.price per-block: dispatch flips block-to-block")  // NOLINT
{
  using namespace per_block_fuel_turbine_test;  // NOLINT

  // Fuel cheap in block 0 ($10) and dear in block 1 ($90).  With
  // heat_rate = 1.0 the per-MWh fuel cost equals the price.  vs a flat
  // $50/MWh alternative: block 0 → fuel gen serves, block 1 → alt.
  System sys;
  sys.name = "PerBlockFuelPrice";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "swing_fuel",
                     .price = OptTBRealFieldSched {
                         std::vector<std::vector<Real>> {{10.0, 90.0}}}}};
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "fuel_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 200.0,
       .gcost = 0.0,
       .fuel = Uid {1},
       .heat_rate = 2.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "alt_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 200.0,
       .gcost = 50.0,
       .capacity = 200.0},
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto simulation = make_pbft_two_block_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Block 0: fuel gen ($10) cheaper than alt ($50) → fuel gen serves all.
  CHECK(pbft_generation_of(system_lp, "fuel_gen", 0).value_or(-1.0)
        == doctest::Approx(100.0));
  CHECK(pbft_generation_of(system_lp, "alt_gen", 0).value_or(-1.0)
        == doctest::Approx(0.0));

  // Block 1: fuel gen ($90) dearer than alt ($50) → alt serves all.
  CHECK(pbft_generation_of(system_lp, "fuel_gen", 1).value_or(-1.0)
        == doctest::Approx(0.0));
  CHECK(pbft_generation_of(system_lp, "alt_gen", 1).value_or(-1.0)
        == doctest::Approx(100.0));

  // Per-block price accessor returns the distinct samples.
  const auto& fuel_lp = system_lp.elements<FuelLP>().front();
  const auto& t = system_lp.phase().stages()[0];
  CHECK(fuel_lp.param_price(t.uid(), t.blocks()[0].uid()).value_or(-1.0)
        == doctest::Approx(10.0));
  CHECK(fuel_lp.param_price(t.uid(), t.blocks()[1].uid()).value_or(-1.0)
        == doctest::Approx(90.0));
}

TEST_CASE("Fuel.price scalar still broadcasts to every block")  // NOLINT
{
  using namespace per_block_fuel_turbine_test;  // NOLINT

  // Scalar price — must broadcast to both blocks (back-compat).
  System sys;
  sys.name = "ScalarFuelPrice";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "flat_fuel",
                     .price = OptTBRealFieldSched {12.0}}};
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "fuel_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 200.0,
       .fuel = Uid {1},
       .heat_rate = 1.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "alt_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 200.0,
       .gcost = 50.0,
       .capacity = 200.0},
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto simulation = make_pbft_two_block_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // $12 < $50 in BOTH blocks → fuel gen serves all in both.
  CHECK(pbft_generation_of(system_lp, "fuel_gen", 0).value_or(-1.0)
        == doctest::Approx(100.0));
  CHECK(pbft_generation_of(system_lp, "fuel_gen", 1).value_or(-1.0)
        == doctest::Approx(100.0));

  // Param accessor broadcasts the scalar to both block uids.
  const auto& fuel_lp = system_lp.elements<FuelLP>().front();
  const auto& t = system_lp.phase().stages()[0];
  CHECK(fuel_lp.param_price(t.uid(), t.blocks()[0].uid()).value_or(-1.0)
        == doctest::Approx(12.0));
  CHECK(fuel_lp.param_price(t.uid(), t.blocks()[1].uid()).value_or(-1.0)
        == doctest::Approx(12.0));
}

// ── Turbine.production_factor per-block changes hydro head room ────────

TEST_CASE("Turbine.production_factor per-block changes dispatch")  // NOLINT
{
  using namespace per_block_fuel_turbine_test;  // NOLINT

  // Cheap hydro (served first) + expensive thermal backstop.  Fixed 30
  // m3/s discharge.  production_factor 2.0 (block 0) → 60 MW hydro head
  // room ≥ 50 demand → hydro serves all; 0.5 (block 1) → 15 MW → thermal
  // covers the rest.
  System sys;
  sys.name = "PerBlockTurbinePF";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0}};
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "hydro_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 500.0,
       .gcost = 5.0,
       .capacity = 500.0},
      {.uid = Uid {2},
       .name = "thermal_gen",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 500.0,
       .gcost = 80.0,
       .capacity = 500.0},
  };
  sys.junction_array = {{.uid = Uid {1}, .name = "j", .drain = true}};
  sys.flow_array = {{.uid = Uid {1},
                     .name = "river_flow",
                     .direction = 1,
                     .junction = Uid {1},
                     .discharge = 30.0}};
  sys.turbine_array = {{.uid = Uid {1},
                        .name = "tur",
                        .flow = SingleId {Uid {1}},
                        .generator = Uid {1},
                        .production_factor = OptTBRealFieldSched {
                            std::vector<std::vector<Real>> {{2.0, 0.5}}}}};

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto simulation = make_pbft_two_block_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Block 0 (PF=2.0): hydro head room 60 MW ≥ 50 → hydro serves all 50.
  CHECK(pbft_generation_of(system_lp, "hydro_gen", 0).value_or(-1.0)
        == doctest::Approx(50.0));
  CHECK(pbft_generation_of(system_lp, "thermal_gen", 0).value_or(-1.0)
        == doctest::Approx(0.0));

  // Block 1 (PF=0.5): hydro capped at 15 MW → thermal covers 35 MW.
  CHECK(pbft_generation_of(system_lp, "hydro_gen", 1).value_or(-1.0)
        == doctest::Approx(15.0));
  CHECK(pbft_generation_of(system_lp, "thermal_gen", 1).value_or(-1.0)
        == doctest::Approx(35.0));

  // Per-block PF accessor returns the distinct samples.
  const auto& tur_lp = system_lp.elements<TurbineLP>().front();
  const auto& t = system_lp.phase().stages()[0];
  CHECK(tur_lp.param_production_factor(t.uid(), t.blocks()[0].uid())
            .value_or(-1.0)
        == doctest::Approx(2.0));
  CHECK(tur_lp.param_production_factor(t.uid(), t.blocks()[1].uid())
            .value_or(-1.0)
        == doctest::Approx(0.5));
}
