// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the `Fuel` entity and its `FuelLP` parameter-carrier wrapper.
//
// `Fuel` is a passive top-level element introduced 2026-05-16 that
// bundles the stage-schedulable fuel `price` ($/GJ) and two CO₂
// emission factors (`combustion_emission_factor` and
// `upstream_emission_factor`, both tCO₂/GJ).  `FuelLP` resolves the
// three schedules at construction; it contributes NO LP variables or
// rows on its own.  The downstream consumer is `GeneratorLP`, which
// reaches the resolved schedules via `SystemContext::element<FuelLP>(...)`
// when a `Generator.fuel` reference is configured.

#include <algorithm>
#include <ranges>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/fuel.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/json/json_fuel.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/validate_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

[[nodiscard]] Simulation make_one_stage_one_block_simulation()
{
  return {
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
                  .uid = Uid {0},
              },
          },
  };
}

}  // namespace

TEST_CASE("Fuel construction and JSON round-trip")  // NOLINT
{
  SUBCASE("default-constructed Fuel has empty schedule fields")
  {
    Fuel f;
    CHECK(f.uid == Uid {unknown_uid});
    CHECK_FALSE(f.price.has_value());
    CHECK_FALSE(f.combustion_emission_factor.has_value());
    CHECK_FALSE(f.upstream_emission_factor.has_value());
  }

  SUBCASE("JSON parses all three schedule fields")
  {
    const std::string_view json = R"({
        "uid": 7,
        "name": "natural_gas",
        "price": 8.5,
        "combustion_emission_factor": 0.0561,
        "upstream_emission_factor": 0.0094
      })";
    const auto f = daw::json::from_json<Fuel>(json);
    CHECK(f.uid == 7);
    CHECK(f.name == "natural_gas");
    REQUIRE(f.price.has_value());
    CHECK(std::get<Real>(*f.price) == doctest::Approx(8.5));
    REQUIRE(f.combustion_emission_factor.has_value());
    CHECK(std::get<Real>(*f.combustion_emission_factor)
          == doctest::Approx(0.0561));
    REQUIRE(f.upstream_emission_factor.has_value());
    CHECK(std::get<Real>(*f.upstream_emission_factor)
          == doctest::Approx(0.0094));
  }

  SUBCASE("JSON round-trip via System.fuel_array")
  {
    const std::string_view json = R"({
        "fuel_array": [
          {
            "uid": 1,
            "name": "diesel",
            "price": 22.0,
            "combustion_emission_factor": 0.0741,
            "upstream_emission_factor": 0.0118
          },
          {
            "uid": 2,
            "name": "coal",
            "price": 3.5,
            "combustion_emission_factor": 0.0946,
            "upstream_emission_factor": 0.0042
          }
        ]
      })";
    const auto sys = daw::json::from_json<System>(json);
    REQUIRE(sys.fuel_array.size() == 2);
    CHECK(sys.fuel_array[0].name == "diesel");
    CHECK(sys.fuel_array[1].name == "coal");
  }
}

// ── Generator + Fuel + heat_rate integration ─────────────────────────
//
// When `Generator.fuel` and `Generator.heat_rate` are both set, the
// per-MWh generation cost on the primary `generation` column should
// equal `(fuel.price × heat_rate + gcost) × block_ecost_factor`.

TEST_CASE(
    "Generator with fuel + scalar heat_rate uses fuel-derived gcost")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // fuel.price = 10 $/GJ, heat_rate = 8 GJ/MWh, gcost = 2 $/MWh.
  // effective per-MWh = 10*8 + 2 = 82.  duration = 1, prob = 1.
  // Expected objective at p=50: 82 * 50 = 4100.
  const auto fuel_price = 10.0;
  const auto hr = 8.0;
  const auto gcost_adder = 2.0;
  const auto demand_val = 50.0;
  const auto dur = 1.0;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = fuel_price,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = gcost_adder,
          .fuel = Uid {1},
          .heat_rate = hr,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = demand_val,
      },
  };

  const Simulation simulation {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = dur,
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
                  .uid = Uid {0},
                  .probability_factor = 1.0,
              },
          },
  };
  const System system {
      .name = "GenFuelScalarHR",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto obj = lp.get_obj_value();
  const auto expected = (fuel_price * hr + gcost_adder) * demand_val * dur;
  CHECK(obj == doctest::Approx(expected).epsilon(1e-6));
}

TEST_CASE(
    "Generator with fuel + piecewise heat_rate_segments — convex")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Three convex segments:
  //   k=0: [0..100] MW @ heat_rate 7.0 GJ/MWh
  //   k=1: [100..150] MW @ 8.5 GJ/MWh
  //   k=2: [150..200] MW @ 10.0 GJ/MWh
  // fuel.price = 5 $/GJ ⇒ per-MWh: 35, 42.5, 50.  gcost = 1 (adder).
  //
  // Demand = 170 MW forces dispatch into segment 2:
  //   100 MWh × (5·7 + 1) = 100 × 36 = 3600
  //    50 MWh × (5·8.5 + 1) =  50 × 43.5 = 2175
  //    20 MWh × (5·10  + 1) =  20 × 51   = 1020
  //   total = 6795
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 5.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmax = 200.0,
          .gcost = 1.0,
          .fuel = Uid {1},
          .pmax_segments =
              {
                  100.0,
                  150.0,
                  200.0,
              },
          .heat_rate_segments =
              {
                  7.0,
                  8.5,
                  10.0,
              },
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 170.0,
      },
  };

  const Simulation simulation {
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
                  .uid = Uid {0},
                  .probability_factor = 1.0,
              },
          },
  };
  const System system {
      .name = "GenFuelPiecewise",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto obj = lp.get_obj_value();
  const auto expected = 100.0 * (5.0 * 7.0 + 1.0) + 50.0 * (5.0 * 8.5 + 1.0)
      + 20.0 * (5.0 * 10.0 + 1.0);
  CHECK(obj == doctest::Approx(expected).epsilon(1e-6));

  // Verify the LP picked segments in convex order: s_1 = 70, s_2 = 20.
  const auto& gens = system_lp.elements<GeneratorLP>();
  REQUIRE(gens.size() == 1);
  const auto& slacks = gens.front().heat_rate_slack_cols();
  REQUIRE(slacks.size() == 2);
  const auto col_sol = lp.get_col_sol();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto st_key = std::tuple {s.uid(), t.uid()};
  const auto& blocks = t.blocks();
  REQUIRE(!blocks.empty());
  const auto buid = blocks[0].uid();

  REQUIRE(slacks[0].contains(st_key));
  const auto& s1_block = slacks[0].at(st_key);
  REQUIRE(s1_block.contains(buid));
  CHECK(col_sol[s1_block.at(buid)] == doctest::Approx(70.0).epsilon(1e-6));

  REQUIRE(slacks[1].contains(st_key));
  const auto& s2_block = slacks[1].at(st_key);
  REQUIRE(s2_block.contains(buid));
  CHECK(col_sol[s2_block.at(buid)] == doctest::Approx(20.0).epsilon(1e-6));
}

TEST_CASE("FuelLP resolves the three schedules and exposes them")  // NOLINT
{
  // FuelLP itself adds no LP entities — verify that building a
  // SystemLP whose only non-trivial entity is a `Fuel` succeeds and
  // that the resolved `param_*` accessors return the configured
  // values.
  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 8.5,
          .combustion_emission_factor = 0.0561,
          .upstream_emission_factor = 0.0094,
      },
  };
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  // A minimal Demand makes the system non-degenerate for SystemLP plumbing.
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,
      },
  };
  const System system {
      .name = "FuelOnly",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .fuel_array = fuel_array,
  };
  const auto simulation = make_one_stage_one_block_simulation();

  PlanningOptions popts;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fuels = system_lp.elements<FuelLP>();
  REQUIRE(fuels.size() == 1);
  const auto& fuel_lp = fuels.front();
  const auto& t = system_lp.phase().stages()[0];

  CHECK(fuel_lp.param_price(t.uid()).value_or(0.0) == doctest::Approx(8.5));
  CHECK(fuel_lp.param_combustion_emission_factor(t.uid()).value_or(0.0)
        == doctest::Approx(0.0561));
  CHECK(fuel_lp.param_upstream_emission_factor(t.uid()).value_or(0.0)
        == doctest::Approx(0.0094));
}

// ── Validation: heat-rate / fuel schema rules ────────────────────────────
//
// `validate_planning` rejects malformed Fuel/heat-rate inputs before LP
// construction so an opaque solver fallback can't mask an input bug.
// The rules under test:
//   - `Generator.heat_rate` AND `heat_rate_segments` set → error.
//   - `pmax_segments` / `heat_rate_segments` length mismatch → error.
//   - `heat_rate_segments` not strictly increasing (non-convex) → error.
//   - `Generator.fuel` UID with no matching Fuel → error.
//   - `Commitment.fuel` UID with no matching Fuel → error.
namespace
{
[[nodiscard]] Planning make_minimal_planning()
{
  Planning p;
  p.system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  p.simulation.block_array = {
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
  };
  p.simulation.stage_array = {
      {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      },
  };
  return p;
}
}  // namespace

TEST_CASE(  // NOLINT
    "Generator validate — scalar heat_rate and heat_rate_segments are mutually "
    "exclusive")
{
  Planning p = make_minimal_planning();
  p.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g_both",
          .bus = Uid {1},
          .heat_rate = 7.5,
          .pmax_segments = {100.0, 200.0},
          .heat_rate_segments = {7.0, 9.0},
      },
  };

  const auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.contains("mutually exclusive"); });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "Generator validate — pmax_segments / heat_rate_segments length mismatch")
{
  Planning p = make_minimal_planning();
  p.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g_mismatch",
          .bus = Uid {1},
          .pmax_segments = {100.0, 200.0, 300.0},
          .heat_rate_segments = {7.0, 9.0},
      },
  };

  const auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors, [](const auto& e) { return e.contains("equal length"); });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "Generator validate — non-convex heat_rate_segments rejected")
{
  Planning p = make_minimal_planning();
  p.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g_nonconvex",
          .bus = Uid {1},
          .pmax_segments = {100.0, 200.0, 300.0},
          // Non-monotone slopes: 7 → 5 → 9 ⇒ LP would pick a non-convex
          // mix without binary disambiguation.
          .heat_rate_segments = {7.0, 5.0, 9.0},
      },
  };

  const auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e)
      { return e.contains("heat_rate_segments must be strictly increasing"); });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "Generator validate — fuel UID not in fuel_array rejected")
{
  Planning p = make_minimal_planning();
  // Note: fuel_array is empty.
  p.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g_dangling_fuel",
          .bus = Uid {1},
          .fuel = Uid {99},
      },
  };

  const auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(result.errors,
                                         [](const auto& e)
                                         {
                                           return e.contains("Generator")
                                               && e.contains("fuel")
                                               && e.contains("Fuel");
                                         });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "Commitment validate — fuel UID not in fuel_array rejected")
{
  Planning p = make_minimal_planning();
  p.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
      },
  };
  p.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "c_dangling_fuel",
          .generator = Uid {1},
          .fuel = Uid {77},
      },
  };

  const auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(result.errors,
                                         [](const auto& e)
                                         {
                                           return e.contains("Commitment")
                                               && e.contains("fuel")
                                               && e.contains("Fuel");
                                         });
  CHECK(found);
}

// ── PAMPL parameter mappings (2026-05-17) ────────────────────────────
//
// Fuel and the new Generator heat_rate / emission_factor fields are
// exposed as `resolve_single_param` constants so user constraints
// can reference them directly.  These tests build a small fixture
// with a user constraint that uses the parameter on the LHS as a
// scalar coefficient and verifies the constraint binds at the
// expected algebraic threshold.

TEST_CASE("PAMPL — fuel('gas').price resolves as scalar parameter")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // Fuel.price = 5; demand load capped via
  //   `fuel('gas').price * demand('d1').load <= 250`
  // ⇒ 5 · load ≤ 250 ⇒ load ≤ 50.  With cheap g1 ($1/MWh) and
  // demand 100 MW the LP serves 50 MW (cost 50) and pays fail on
  // 50 MW @ fail_cost = 100 (cost 5000).  obj = 5050 (raw,
  // scale_objective=1).
  const Array<Fuel> fuel_array = {
      {.uid = Uid {1}, .name = "gas", .price = 5.0},
  };
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "fuel_price_load_cap",
          // Use the param as an additive term — `resolve_single_param`
          // moves it to the RHS via `param_shift`, so this resolves to
          // `load <= 50` (= 55 − 5).  Mirrors the
          // `options.scale_objective` pattern used elsewhere in the
          // user-constraint suite.
          .expression = "demand('d1').load + fuel('gas').price <= 55",
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 100.0;

  const System system {
      .name = "PamplFuelPriceFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
      .user_constraint_array = user_constraint_array,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  // 50 MW served · $1/MWh + 50 MW unserved · $100/MWh = 50 + 5000 = 5050.
  CHECK(lp.get_obj_value() == doctest::Approx(5050.0).epsilon(1e-6));
}

TEST_CASE(
    "PAMPL — generator('g1').heat_rate resolves as scalar parameter")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // Generator.heat_rate = 4; demand cap via
  //   `generator('g1').heat_rate * demand('d1').load <= 200`
  // ⇒ 4 · load ≤ 200 ⇒ load ≤ 50.  Same algebra as the fuel-price
  // case (load capped by a scalar parameter times the load
  // variable), just exercising the new generator.heat_rate mapping.
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 1.0,
          .heat_rate = 4.0,  // PLEXOS Heat Rate
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "heat_rate_load_cap",
          // Param-as-additive form (`param + var <= rhs` ⇒ `var <= rhs −
          // param`): load + heat_rate (= 4) <= 54 ⇒ load <= 50.
          .expression = "demand('d1').load + generator('g1').heat_rate <= 54",
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 100.0;

  const System system {
      .name = "PamplGenHeatRateFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .user_constraint_array = user_constraint_array,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  // 50 MW served · $1/MWh + 50 MW unserved · $100/MWh = 5050.
  CHECK(lp.get_obj_value() == doctest::Approx(5050.0).epsilon(1e-6));
}
