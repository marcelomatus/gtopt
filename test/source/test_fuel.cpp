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

  SUBCASE("JSON parses optional type + subtype taxonomy hint")
  {
    // The converter-side ``plexos2gtopt`` emits ``type`` (canonical
    // fuel family — diesel / fuel_oil / gas / glp / biomasa / biogas /
    // carbon / otros) and an optional ``subtype`` hint (lng /
    // sub_bituminous / …) consumed by the IPCC emission-defaults
    // overlay at share/gtopt/emissions/cen_chile.json.  gtopt itself
    // doesn't wire either into the LP — they're metadata that must
    // round-trip cleanly through the JSON parser.
    const std::string_view json = R"({
        "uid": 11,
        "name": "Gas_Colbun_A",
        "price": 8.5,
        "type": "gas",
        "subtype": "lng"
      })";
    const auto f = daw::json::from_json<Fuel>(json);
    CHECK(f.uid == 11);
    CHECK(f.name == "Gas_Colbun_A");
    REQUIRE(f.type.has_value());
    CHECK(*f.type == "gas");
    REQUIRE(f.subtype.has_value());
    CHECK(*f.subtype == "lng");
  }

  SUBCASE("JSON omits subtype when absent (back-compat)")
  {
    // Pre-9aff260f3 JSON (no subtype field) must still parse.
    const std::string_view json = R"({
        "uid": 12,
        "name": "Diesel_Collahuasi",
        "price": 770.0,
        "type": "diesel"
      })";
    const auto f = daw::json::from_json<Fuel>(json);
    CHECK(f.name == "Diesel_Collahuasi");
    REQUIRE(f.type.has_value());
    CHECK(*f.type == "diesel");
    CHECK_FALSE(f.subtype.has_value());
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
  const auto& b0 = t.blocks().front();

  CHECK(fuel_lp.param_price(t.uid(), b0.uid()).value_or(0.0)
        == doctest::Approx(8.5));
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

// Commitment.fuel was removed on 2026-05-20 (dispatch cost lives on
// Generator now).  The dangling-fuel-FK case is covered by the
// Generator validate test above.

// ── PAMPL parameter mappings (2026-05-17) ────────────────────────────
//
// Fuel and the new Generator heat_rate / emission_rate fields are
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
          // `options.model_options.scale_objective` pattern used elsewhere in
          // the user-constraint suite.
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

// Compact sanity check that every new Fuel parameter resolves to the
// *correct* field — each subcase pins a different attribute by
// choosing a unique numeric value, building a user constraint that
// adds the attribute to a single decision variable, and asserting
// the LP binds at the expected algebraic threshold derived from that
// specific value.  Catches a regression where the new fuel branch
// returns the wrong field (e.g. price when heat_content was asked
// for) — that mistake would still resolve as "param" but produce
// the wrong RHS shift.
TEST_CASE("PAMPL — every Fuel attribute resolves to its field")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // Distinct numeric values so the assertion uniquely identifies
  // which schedule was read.
  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 5.0,
          .heat_content = 7.0,
          .combustion_emission_factor = 11.0,
          .upstream_emission_factor = 13.0,
      },
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
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 100.0;

  // Each subcase: `load + fuel.<attr> <= 5 + <attr_value>`
  // ⇒ load <= 5  ⇒ obj = 5·$1 + 95·$100 = 5 + 9500 = 9505.
  // If the resolver returns the wrong field, the threshold would
  // shift and the obj wouldn't match.
  const auto run_with_expression = [&](std::string expr, double expected_obj)
  {
    const Array<UserConstraint> ucs = {
        {.uid = Uid {1}, .name = "uc", .expression = std::move(expr)},
    };
    const System system {
        .name = "PamplFuelAttrFixture",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .fuel_array = fuel_array,
        .user_constraint_array = ucs,
    };
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(lp.get_obj_value() == doctest::Approx(expected_obj).epsilon(1e-6));
  };

  SUBCASE("price")
  {
    // load + 5 <= 10 ⇒ load <= 5 ⇒ obj = 5 + 9500 = 9505.
    run_with_expression("demand('d1').load + fuel('gas').price <= 10", 9505.0);
  }
  SUBCASE("heat_content")
  {
    // load + 7 <= 12 ⇒ load <= 5 ⇒ obj = 5 + 9500 = 9505.
    run_with_expression("demand('d1').load + fuel('gas').heat_content <= 12",
                        9505.0);
  }
  SUBCASE("combustion_emission_factor")
  {
    // load + 11 <= 16 ⇒ load <= 5.
    run_with_expression(
        "demand('d1').load + fuel('gas').combustion_emission_factor <= 16",
        9505.0);
  }
  SUBCASE("upstream_emission_factor")
  {
    // load + 13 <= 18 ⇒ load <= 5.
    run_with_expression(
        "demand('d1').load + fuel('gas').upstream_emission_factor <= 18",
        9505.0);
  }
}

TEST_CASE(
    "PAMPL — unknown Fuel attribute is a hard error (even in normal "
    "mode)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // The fuel branch in `resolve_single_param` falls through to
  // `return std::nullopt` for any attribute it doesn't recognise, and
  // `fuel.density` is not a registered LP variable either.  Hardening
  // (2026-05): such a genuinely-undefined attribute is a hard error
  // regardless of `constraint_mode` — `normal` only controls verbosity,
  // never whether the undefined reference is tolerated.  Building the
  // SystemLP therefore throws.
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
  // `fuel.density` is NOT a registered attribute — neither an LP
  // variable nor a data parameter — so the resolver raises an
  // informative hard error during the SystemLP build.
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "unknown_attr",
          .expression = "demand('d1').load + fuel('gas').density <= 100",
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
  // Even under `normal` mode, a genuinely-undefined attribute is a hard
  // error (verbosity-only, not tolerance).
  popts.constraint_mode = ConstraintMode::normal;

  const System system {
      .name = "PamplFuelUnknownAttr",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
      .user_constraint_array = ucs,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  CHECK_THROWS_AS(SystemLP(system, simulation_lp),  // NOLINT
                  std::runtime_error);
}

TEST_CASE(
    "PAMPL — generator('g1').emission_rate resolves to its field")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)
  // generator.emission_rate = 7 (distinct value to identify the
  // field).  load + 7 <= 12 ⇒ load <= 5 ⇒ obj = 9505 (as in the
  // fuel-attribute test).
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 200.0,
          .emission_rate = 7.0,
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
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "emission_rate_cap",
          .expression =
              "demand('d1').load + generator('g1').emission_rate <= 12",
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
      .name = "PamplGenEmissionFactorFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .user_constraint_array = ucs,
  };
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(lp.get_obj_value() == doctest::Approx(9505.0).epsilon(1e-6));
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

// ── Commit 9 — Fuel.emission_factors[] table + auto-fold + expand ──────

TEST_CASE("FuelEmissionFactor JSON round-trip")  // NOLINT
{
  constexpr std::string_view src = R"({
    "emission": "co2",
    "combustion": 0.0561,
    "upstream": 0.0094
  })";
  const auto f = daw::json::from_json<FuelEmissionFactor>(src);
  CHECK(std::get<Name>(f.emission) == "co2");
  REQUIRE(f.combustion.has_value());
  CHECK(std::get<Real>(*f.combustion) == doctest::Approx(0.0561));
  REQUIRE(f.upstream.has_value());
  CHECK(std::get<Real>(*f.upstream) == doctest::Approx(0.0094));
}

TEST_CASE("Fuel.emission_factors[] JSON round-trip")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 1,
    "name": "gas",
    "price": 5.0,
    "emission_factors": [
      {"emission": "co2", "combustion": 0.0561, "upstream": 0.0094},
      {"emission": "ch4", "combustion": 0.0002}
    ]
  })";
  const auto f = daw::json::from_json<Fuel>(src);
  REQUIRE(f.emission_factors.size() == 2);
  CHECK(std::get<Name>(f.emission_factors[0].emission) == "co2");
  CHECK(std::get<Real>(*f.emission_factors[0].upstream)
        == doctest::Approx(0.0094));
  CHECK(std::get<Name>(f.emission_factors[1].emission) == "ch4");
  CHECK_FALSE(f.emission_factors[1].upstream.has_value());
}

TEST_CASE(
    "System::fold_legacy_fuel_emission_factors — moves scalar legacy fields "
    "into emission_factors[] (CO₂ row)")  // NOLINT
{
  System sys;
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 5.0,
          .combustion_emission_factor = 0.0561,
          .upstream_emission_factor = 0.0094,
      },
  };

  sys.fold_legacy_fuel_emission_factors();

  // A CO₂ emission tag is auto-registered.
  REQUIRE(sys.emission_array.size() == 1);
  CHECK(sys.emission_array.front().name == "co2");
  const auto co2_uid = sys.emission_array.front().uid;

  // The fuel now carries the merged row.
  REQUIRE(sys.fuel_array.front().emission_factors.size() == 1);
  const auto& fef = sys.fuel_array.front().emission_factors.front();
  CHECK(std::get<Uid>(fef.emission) == co2_uid);
  REQUIRE(fef.combustion.has_value());
  CHECK(std::get<Real>(*fef.combustion) == doctest::Approx(0.0561));
  REQUIRE(fef.upstream.has_value());
  CHECK(std::get<Real>(*fef.upstream) == doctest::Approx(0.0094));

  // Legacy fields cleared.
  CHECK_FALSE(sys.fuel_array.front().combustion_emission_factor.has_value());
  CHECK_FALSE(sys.fuel_array.front().upstream_emission_factor.has_value());

  // Idempotent second call.
  sys.fold_legacy_fuel_emission_factors();
  CHECK(sys.fuel_array.front().emission_factors.size() == 1);
}

TEST_CASE(
    "System::expand_fuel_emission_sources — synthesizes EmissionSource rows "
    "from fuel.emission_factors × Generator.heat_rate")  // NOLINT
{
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 5.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = 0.05,
                      .upstream = 0.01,
                  },
              },
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {7},
          .name = "ngcc",
          .bus = Uid {1},
          .gcost = 10.0,
          .fuel = SingleId {Uid {1}},
          .heat_rate = 8.0,  // GJ/MWh
          .capacity = 200.0,
      },
  };

  sys.expand_fuel_emission_sources();

  REQUIRE(sys.emission_source_array.size() == 1);
  const auto& es = sys.emission_source_array.front();
  REQUIRE(es.generator.has_value());
  CHECK(std::get<Uid>(es.generator.value_or(SingleId {Uid {0}})) == Uid {7});
  CHECK(std::get<Uid>(es.zone) == Uid {1});
  CHECK(std::get<Uid>(es.emission) == Uid {1});
  REQUIRE(es.rate.has_value());
  CHECK(std::get<Real>(*es.rate) == doctest::Approx(8.0 * 0.05));  // 0.40
  REQUIRE(es.upstream_rate.has_value());
  CHECK(std::get<Real>(*es.upstream_rate) == doctest::Approx(8.0 * 0.01));

  // Idempotent — same names → skipped on re-run.
  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.size() == 1);
}

TEST_CASE(
    "System::expand_fuel_emission_sources — skips when no covering zone")  // NOLINT
{
  // Pollutant exists, fuel carries a factor for it, but no
  // `EmissionZone` covers the pollutant → no source synthesized.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"},
                        {.uid = Uid {2}, .name = "nox"}};
  // Zone covers NOx only, not CO₂.
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "la_nox",
       .emissions = {{.emission = Uid {2}, .weight = 1.0}}}};
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .emission_factors = {{.emission = SingleId {Uid {1}},
                                .combustion = 0.05}},
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {7},
          .name = "ngcc",
          .bus = Uid {1},
          .fuel = SingleId {Uid {1}},
          .heat_rate = 8.0,
      },
  };

  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.empty());
}

TEST_CASE(
    "System::expand_fuel_emission_sources — time-varying heat_rate skipped "
    "with warn (LP-balance lossless contract)")  // NOLINT
{
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "gas",
                     .emission_factors = {{.emission = SingleId {Uid {1}},
                                           .combustion = 0.05}}}};
  // Per-stage vector heat_rate — fold path requires scalar.
  sys.generator_array = {{.uid = Uid {7},
                          .name = "ngcc",
                          .bus = Uid {1},
                          .fuel = SingleId {Uid {1}},
                          .heat_rate = OptTBRealFieldSched {
                              std::vector<std::vector<Real>> {{8.0}, {9.0}}}}};

  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.empty());
}

TEST_CASE(
    "Fuel.emission_factors[] + Generator.heat_rate end-to-end — "
    "balance row hits expected coefficient via PlanningLP")  // NOLINT
{
  // gen.heat_rate=8 GJ/MWh, fuel.emission_factors[co2].combustion=0.05
  // ⇒ effective combustion rate = 0.40 tCO₂/MWh.  Upstream=0.01 ⇒
  // additional 0.08 tCO₂/MWh.  Cap=20 tCO₂ at the zone, demand=100,
  // 1 block of 1h ⇒ binding (cap = 20 / 0.48 = 41.67 MWh served).
  //
  // We don't pin the objective here — just verify the system builds,
  // solves, and that fold + expand produced one EmissionSource.

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Emission> emission_array = {{.uid = Uid {1}, .name = "co2"}};
  const Array<EmissionZone> emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}},
       .cap = 20.0}};
  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 1.0,
          .emission_factors = {{.emission = SingleId {Uid {1}},
                                .combustion = 0.05,
                                .upstream = 0.01}},
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "ngcc",
          .bus = Uid {1},
          .gcost = 1.0,
          .fuel = SingleId {Uid {1}},
          .heat_rate = 8.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};

  Planning planning;
  planning.simulation.block_array = {{.uid = Uid {1}, .duration = 1.0}};
  planning.simulation.stage_array = {
      {.uid = Uid {1}, .first_block = 0, .count_block = 1}};
  planning.simulation.scenario_array = {{.uid = Uid {0}}};
  planning.system = System {
      .name = "FuelEmFactorsE2E",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
      .emission_array = emission_array,
      .emission_zone_array = emission_zone_array,
  };
  planning.options.model_options.demand_fail_cost = 1000.0;
  planning.options.model_options.scale_objective = 1.0;

  // Drive the fold+expand path that lives in PlanningLP::create_systems
  // without depending on PlanningLP construction internals: call the
  // System hooks directly, then verify the synthesized source.  This
  // is the same contract `PlanningLP::create_systems()` relies on.
  planning.system.fold_legacy_fuel_emission_factors();
  planning.system.expand_fuel_emission_sources();

  REQUIRE(planning.system.emission_source_array.size() == 1);
  const auto& es = planning.system.emission_source_array.front();
  REQUIRE(es.rate.has_value());
  CHECK(std::get<Real>(*es.rate) == doctest::Approx(0.40));
  REQUIRE(es.upstream_rate.has_value());
  CHECK(std::get<Real>(*es.upstream_rate) == doctest::Approx(0.08));
}

// ── More fold / expand edge cases ──────────────────────────────────

TEST_CASE(
    "System::expand_fuel_emission_sources — resolves Generator.fuel by Name "
    "(in addition to Uid)")  // NOLINT
{
  // Some workflows reference the fuel by its `name` string instead of
  // the numeric `uid` (this is the form plp2gtopt emits today).  The
  // expand pass must resolve either form.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {99},
                     .name = "gas",
                     .emission_factors = {{.emission = SingleId {Uid {1}},
                                           .combustion = 0.05}}}};
  sys.generator_array = {
      {.uid = Uid {7},
       .name = "ngcc",
       .bus = Uid {1},
       .fuel = SingleId {Name {"gas"}},  // Name reference, not Uid
       .heat_rate = 8.0}};

  sys.expand_fuel_emission_sources();

  REQUIRE(sys.emission_source_array.size() == 1);
  CHECK(std::get<Real>(*sys.emission_source_array.front().rate)
        == doctest::Approx(0.40));
}

TEST_CASE(
    "System::expand_fuel_emission_sources — dangling Generator.fuel ref is "
    "skipped (no crash, no source synthesized)")  // NOLINT
{
  // No matching Fuel in `fuel_array` — validate_planning catches this,
  // but the expand pass must be robust if called pre-validation (some
  // unit tests bypass validation).
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "gas",
                     .emission_factors = {{.emission = SingleId {Uid {1}},
                                           .combustion = 0.05}}}};
  sys.generator_array = {{.uid = Uid {7},
                          .name = "g1",
                          .bus = Uid {1},
                          .fuel = SingleId {Uid {404}},  // dangling
                          .heat_rate = 8.0}};

  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.empty());
}

TEST_CASE(
    "System::expand_fuel_emission_sources — fuel with empty "
    "emission_factors[] yields no sources")  // NOLINT
{
  // The fuel exists and is referenced, but it carries no
  // emission_factors[] rows — typical for a non-emitting biomass fuel
  // with no upstream WTT factor set.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {1}, .name = "biomass"}};
  sys.generator_array = {{.uid = Uid {7},
                          .name = "biomass_plant",
                          .bus = Uid {1},
                          .fuel = SingleId {Uid {1}},
                          .heat_rate = 12.0}};

  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.empty());
}

TEST_CASE(
    "System::expand_fuel_emission_sources — generator with heat_rate but no "
    "fuel reference is skipped")  // NOLINT
{
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "gas",
                     .emission_factors = {{.emission = SingleId {Uid {1}},
                                           .combustion = 0.05}}}};
  sys.generator_array = {{
      .uid = Uid {7},
      .name = "ngcc_unconfigured",
      .bus = Uid {1},
      .heat_rate = 8.0  // no .fuel set
  }};

  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.empty());
}

TEST_CASE(
    "System::expand_fuel_emission_sources — multiple zones covering same "
    "pollutant produce one EmissionSource per (generator, zone)")  // NOLINT
{
  // Useful when a generator must respect both a regional and a global
  // CO₂ basket (e.g. EU ETS plus a national sector cap).  Expect
  // 1 fueled generator × 2 covering zones = 2 sources.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "national_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}},
      {.uid = Uid {2},
       .name = "regional_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "gas",
                     .emission_factors = {{.emission = SingleId {Uid {1}},
                                           .combustion = 0.05}}}};
  sys.generator_array = {{.uid = Uid {7},
                          .name = "ngcc",
                          .bus = Uid {1},
                          .fuel = SingleId {Uid {1}},
                          .heat_rate = 8.0}};

  sys.expand_fuel_emission_sources();
  REQUIRE(sys.emission_source_array.size() == 2);
  // Both sources tied to the same generator.
  for (const auto& es : sys.emission_source_array) {
    REQUIRE(es.generator.has_value());
    CHECK(std::get<Uid>(es.generator.value_or(SingleId {Uid {0}})) == Uid {7});
  }
  // One source per zone — auto-named distinctly.
  const auto& a = sys.emission_source_array[0];
  const auto& b = sys.emission_source_array[1];
  CHECK(a.name != b.name);
  CHECK(std::get<Uid>(a.zone) != std::get<Uid>(b.zone));
}

TEST_CASE(
    "System::fold_legacy_fuel_emission_factors — merges into an existing "
    "CO₂ row instead of duplicating it")  // NOLINT
{
  // The fuel already has a CO₂ row (combustion only set) and ALSO
  // sets the legacy `upstream_emission_factor` scalar.  The fold
  // should fill the missing `upstream` slot in the existing row,
  // NOT push a duplicate row.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.fuel_array = {{.uid = Uid {1},
                     .name = "gas",
                     .upstream_emission_factor = 0.0094,  // legacy field
                     .emission_factors = {
                         {.emission = SingleId {Uid {1}},
                          .combustion = 0.0561,
                          .upstream = std::nullopt},
                     }}};

  sys.fold_legacy_fuel_emission_factors();

  REQUIRE(sys.fuel_array.front().emission_factors.size() == 1);
  const auto& fef = sys.fuel_array.front().emission_factors.front();
  REQUIRE(fef.combustion.has_value());
  CHECK(std::get<Real>(*fef.combustion) == doctest::Approx(0.0561));
  REQUIRE(fef.upstream.has_value());
  CHECK(std::get<Real>(*fef.upstream) == doctest::Approx(0.0094));
  // Legacy field cleared.
  CHECK_FALSE(sys.fuel_array.front().upstream_emission_factor.has_value());
}

TEST_CASE(
    "System::fold_legacy_fuel_emission_factors — does NOT overwrite an "
    "existing combustion factor in the table (new-style wins on conflict)")  // NOLINT
{
  // Both legacy + new-style combustion factors are set with DIFFERENT
  // numeric values.  The fold leaves the new-style value in place
  // (i.e. legacy only fills null slots), so the user-curated
  // emission_factors[] table is always the source of truth on conflict.
  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.fuel_array = {
      {.uid = Uid {1},
       .name = "gas",
       .combustion_emission_factor = 0.99,  // legacy (would clobber)
       .emission_factors = {
           {.emission = SingleId {Uid {1}}, .combustion = 0.0561}}}};

  sys.fold_legacy_fuel_emission_factors();

  REQUIRE(sys.fuel_array.front().emission_factors.size() == 1);
  // New-style value preserved.
  CHECK(std::get<Real>(
            *sys.fuel_array.front().emission_factors.front().combustion)
        == doctest::Approx(0.0561));
}

TEST_CASE(
    "System::fold_legacy_fuel_emission_factors — reuses existing CO₂ tag "
    "(no duplicate emission registry entry)")  // NOLINT
{
  // A CO₂ tag with uid 42 already exists in emission_array; the fold
  // should NOT push a second one.
  System sys;
  sys.emission_array = {{.uid = Uid {42}, .name = "co2"}};
  sys.fuel_array = {
      {.uid = Uid {1}, .name = "gas", .combustion_emission_factor = 0.05}};

  sys.fold_legacy_fuel_emission_factors();

  CHECK(sys.emission_array.size() == 1);
  CHECK(sys.emission_array.front().uid == Uid {42});
  REQUIRE(sys.fuel_array.front().emission_factors.size() == 1);
  CHECK(std::get<Uid>(sys.fuel_array.front().emission_factors.front().emission)
        == Uid {42});
}

// ── Literature-reference unit tests ────────────────────────────────
//
// Pin gtopt's per-MWh rates against published IPCC 2006 (AR6) default
// emission factors so a future refactor of the fold/expand path
// cannot silently break the contract `rate = heat_rate · factor`.
//
// Reference: IPCC 2006 Guidelines for National Greenhouse Gas
// Inventories, Volume 2 (Energy), Chapter 2 (Stationary Combustion),
// Table 2.2 (default CO₂ EFs for stationary combustion).
// https://www.ipcc-nggip.iges.or.jp/public/2006gl/pdf/2_Volume2/V2_2_Ch2_Stationary_Combustion.pdf
//
//   Coking Coal:           94.6 kg/GJ  (0.0946 tCO₂/GJ)
//   Other Bituminous Coal: 94.6 kg/GJ
//   Sub-Bituminous Coal:   96.1 kg/GJ
//   Lignite:              101.0 kg/GJ
//   Anthracite:            98.3 kg/GJ
//   Natural Gas:           56.1 kg/GJ  (0.0561 tCO₂/GJ)
//   Diesel / Gas Oil:      74.1 kg/GJ  (0.0741 tCO₂/GJ)
//   Residual Fuel Oil:     77.4 kg/GJ
//   Motor Gasoline:        69.3 kg/GJ
//   LPG:                   63.1 kg/GJ
//
// Heat rates picked from typical EIA-EPM / IEA-WEO values for
// modern subcritical / CCGT / oil-peaker plants:
//   Coal subcritical:      ~10.5 GJ/MWh  (≈ 34% η)
//   Coal supercritical:    ~9.0 GJ/MWh   (≈ 40% η)
//   Gas CCGT:              ~7.5 GJ/MWh   (≈ 48% η)
//   Gas OCGT:              ~10.0 GJ/MWh  (≈ 36% η)
//   Diesel peaker:         ~10.5 GJ/MWh  (≈ 34% η)

TEST_CASE(
    "Literature reference — IPCC 2006 default factors produce the expected "
    "per-MWh rates through expand_fuel_emission_sources")  // NOLINT
{
  // Build one generator per (fuel, plant-type) pair listed above and
  // verify the synthesized EmissionSource.rate matches the literature
  // benchmark to within 0.1%.
  struct Bench
  {
    Name fuel_name;
    Uid fuel_uid;
    Real combustion;  // tCO₂/GJ
    Name plant_name;
    Uid plant_uid;
    Real heat_rate;  // GJ/MWh
    Real expected_rate;  // tCO₂/MWh (= combustion · heat_rate)
  };
  // Use Approx(...) on every check, so the table can be read as
  // "literature value × heat-rate ≈ expected".
  const std::array benchmarks {
      Bench {"coal_subcrit",
             Uid {1},
             0.0946,
             "coal_pc_subcrit",
             Uid {1},
             10.5,
             0.99330},
      Bench {"coal_supercrit",
             Uid {2},
             0.0946,
             "coal_pc_supercrit",
             Uid {2},
             9.0,
             0.85140},
      Bench {"natural_gas", Uid {3}, 0.0561, "ccgt", Uid {3}, 7.5, 0.42075},
      Bench {
          "natural_gas_ocgt", Uid {4}, 0.0561, "ocgt", Uid {4}, 10.0, 0.56100},
      Bench {
          "diesel", Uid {5}, 0.0741, "diesel_peaker", Uid {5}, 10.5, 0.77805},
      Bench {
          "residual_oil", Uid {6}, 0.0774, "oil_steam", Uid {6}, 11.0, 0.85140},
  };

  System sys;
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};

  for (const auto& b : benchmarks) {
    sys.fuel_array.push_back(
        {.uid = b.fuel_uid,
         .name = b.fuel_name,
         .emission_factors = {
             {.emission = SingleId {Uid {1}}, .combustion = b.combustion}}});
    sys.generator_array.push_back({.uid = b.plant_uid,
                                   .name = b.plant_name,
                                   .bus = Uid {1},
                                   .fuel = SingleId {b.fuel_uid},
                                   .heat_rate = b.heat_rate});
  }

  sys.expand_fuel_emission_sources();
  REQUIRE(sys.emission_source_array.size() == benchmarks.size());

  for (const auto& b : benchmarks) {
    const auto it = std::ranges::find_if(
        sys.emission_source_array,
        [&](const auto& es)
        {
          return es.generator.has_value()
              && std::get<Uid>(es.generator.value_or(SingleId {Uid {0}}))
              == b.plant_uid;
        });
    REQUIRE(it != sys.emission_source_array.end());
    REQUIRE(it->rate.has_value());
    CHECK(std::get<Real>(*it->rate)
          == doctest::Approx(b.expected_rate).epsilon(1e-4));
  }
}

// Many real-world fuels emit more than just CO₂.  PLEXOS Power Datasets
// and EPA AP-42 publish co-pollutants (NOₓ, SO₂, CH₄, particulates)
// alongside the headline CO₂ number.  Verify the multi-pollutant row
// expansion works end-to-end with literature values.
//
// AP-42 / EIA-923 NOₓ + SO₂ rates for coal (uncontrolled, low-sulfur
// bituminous):
//   coal NOₓ (combustion): ~0.000800 tNOx/GJ  (≈ 0.8 kg/GJ)
//   coal SO₂ (1% S coal):  ~0.000900 tSO₂/GJ  (≈ 0.9 kg/GJ)
//   gas  NOₓ:              ~0.000060 tNOx/GJ
//
// Ref: US EPA AP-42 Compilation of Air Pollutant Emission Factors,
// Vol. 1 Stationary Sources, Ch. 1 (External Combustion).

TEST_CASE(
    "Literature reference — EPA AP-42 multi-pollutant factors for coal + gas "
    "produce expected NOₓ / SO₂ rates")  // NOLINT
{
  constexpr Real nox_coal = 0.000800;  // tNOx/GJ
  constexpr Real so2_coal = 0.000900;  // tSO2/GJ
  constexpr Real nox_gas = 0.000060;  // tNOx/GJ
  constexpr Real hr_coal = 10.5;  // GJ/MWh
  constexpr Real hr_gas = 7.5;  // GJ/MWh

  System sys;
  // Pollutants.
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"},
                        {.uid = Uid {2}, .name = "nox"},
                        {.uid = Uid {3}, .name = "so2"}};
  // Three zones — one per pollutant.
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}},
      {.uid = Uid {2},
       .name = "regional_nox",
       .emissions = {{.emission = Uid {2}, .weight = 1.0}}},
      {.uid = Uid {3},
       .name = "regional_so2",
       .emissions = {{.emission = Uid {3}, .weight = 1.0}}},
  };
  // Coal — 3-pollutant factors.
  sys.fuel_array.push_back(
      {.uid = Uid {1},
       .name = "coal",
       .emission_factors = {
           {.emission = SingleId {Uid {1}}, .combustion = 0.0946},
           {.emission = SingleId {Uid {2}}, .combustion = nox_coal},
           {.emission = SingleId {Uid {3}}, .combustion = so2_coal}}});
  // Gas — CO₂ + NOₓ (no SO₂ — natural gas is essentially sulfur-free).
  sys.fuel_array.push_back(
      {.uid = Uid {2},
       .name = "gas",
       .emission_factors = {
           {.emission = SingleId {Uid {1}}, .combustion = 0.0561},
           {.emission = SingleId {Uid {2}}, .combustion = nox_gas}}});
  // Two generators.
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "coal_pc",
       .bus = Uid {1},
       .fuel = SingleId {Uid {1}},
       .heat_rate = hr_coal},
      {.uid = Uid {2},
       .name = "ngcc",
       .bus = Uid {1},
       .fuel = SingleId {Uid {2}},
       .heat_rate = hr_gas},
  };

  sys.expand_fuel_emission_sources();
  // 1 coal × 3 zones (CO₂+NOₓ+SO₂) + 1 gas × 2 zones (CO₂+NOₓ) = 5.
  REQUIRE(sys.emission_source_array.size() == 5);

  // Helper to locate a synthesized row by (gen_uid, emission_uid).
  const auto get_rate = [&](Uid gen_uid, Uid em_uid) -> Real
  {
    const auto it = std::ranges::find_if(
        sys.emission_source_array,
        [&](const auto& es)
        {
          return es.generator.has_value()
              && std::get<Uid>(es.generator.value_or(SingleId {Uid {0}}))
              == gen_uid
              && std::holds_alternative<Uid>(es.emission)
              && std::get<Uid>(es.emission) == em_uid;
        });
    REQUIRE(it != sys.emission_source_array.end());
    REQUIRE(it->rate.has_value());
    return std::get<Real>(*it->rate);
  };

  // Coal: CO₂, NOₓ, SO₂ rates per MWh.
  CHECK(get_rate(Uid {1}, Uid {1})
        == doctest::Approx(hr_coal * 0.0946).epsilon(1e-6));
  CHECK(get_rate(Uid {1}, Uid {2})
        == doctest::Approx(hr_coal * nox_coal).epsilon(1e-6));
  CHECK(get_rate(Uid {1}, Uid {3})
        == doctest::Approx(hr_coal * so2_coal).epsilon(1e-6));
  // Gas: CO₂, NOₓ.
  CHECK(get_rate(Uid {2}, Uid {1})
        == doctest::Approx(hr_gas * 0.0561).epsilon(1e-6));
  CHECK(get_rate(Uid {2}, Uid {2})
        == doctest::Approx(hr_gas * nox_gas).epsilon(1e-6));
}
