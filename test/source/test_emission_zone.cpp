// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the `EmissionZone` constraint-owner entity and its passive
// `EmissionZoneLP` wrapper (Commit 2 of the emissions ladder).
//
// `EmissionZone` is the per-pollutant cap / price owner — mirrors
// `InertiaZone` / `ReserveZone`.  Passive in Commit 2: just data +
// JSON + Collection presence.  LP-active wiring (the balance row +
// optional cap row + optional price coefficient) lands in Commit 3.

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/emission_zone.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/json/json_emission_zone.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("EmissionZone default construction")  // NOLINT
{
  const EmissionZone z;
  CHECK(z.uid == Uid {unknown_uid});
  CHECK(z.name.empty());
  CHECK_FALSE(z.active.has_value());
  CHECK(z.emission == SingleId {unknown_uid});
  CHECK_FALSE(z.cap.has_value());
  CHECK_FALSE(z.cap_cost.has_value());
  CHECK_FALSE(z.price.has_value());
}

TEST_CASE("EmissionZone attribute assignment")  // NOLINT
{
  EmissionZone z;
  z.uid = Uid {1};
  z.name = "global_co2";
  z.emission = Uid {1};
  z.cap = 1.0e6;
  z.cap_cost = 1000.0;
  z.price = 50.0;

  CHECK(z.name == "global_co2");
  CHECK(std::get<Uid>(z.emission) == Uid {1});
  REQUIRE(z.cap.has_value());
  REQUIRE(z.cap_cost.has_value());
  REQUIRE(z.price.has_value());
}

TEST_CASE("EmissionZone JSON round-trip")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 1,
    "name": "global_co2",
    "emission": "co2",
    "cap": 1000000.0,
    "cap_cost": 1000.0,
    "price": 50.0
  })";

  const auto z = daw::json::from_json<EmissionZone>(src);
  CHECK(z.name == "global_co2");
  CHECK(std::get<Name>(z.emission) == "co2");
  REQUIRE(z.cap.has_value());
  CHECK(std::get<Real>(z.cap.value_or(Real {0.0})) == doctest::Approx(1.0e6));

  // Re-emit and re-parse.
  const auto rendered = daw::json::to_json(z);
  const auto z2 = daw::json::from_json<EmissionZone>(rendered);
  CHECK(z2.name == z.name);
}

TEST_CASE("EmissionZone JSON — pure reporting (no cap, no price)")  // NOLINT
{
  // Zone with neither cap nor price — the balance row will still be
  // built in Commit 3 so total emissions are accounted for.
  constexpr std::string_view src = R"({
    "uid": 99,
    "name": "default_co2",
    "emission": "co2"
  })";

  const auto z = daw::json::from_json<EmissionZone>(src);
  CHECK(z.name == "default_co2");
  CHECK_FALSE(z.cap.has_value());
  CHECK_FALSE(z.price.has_value());
}

TEST_CASE("EmissionZone survives the System → SystemLP pipeline")  // NOLINT
{
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmissionZoneSurvival",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "global_co2",
                               .emission = Uid {1},
                               .cap = 1.0e6,
                               .cap_cost = 1000.0,
                               .price = 50.0}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  CHECK(zones.front().emission_zone().name == "global_co2");
  const auto sid = simulation_lp.stages().front().uid();
  CHECK(zones.front().param_cap(sid).value_or(0.0) == doctest::Approx(1.0e6));
  CHECK(zones.front().param_price(sid).value_or(0.0) == doctest::Approx(50.0));
}

TEST_CASE("System::merge concatenates emission_zone_array")  // NOLINT
{
  System a;
  a.emission_zone_array = {
      {.uid = Uid {1}, .name = "global_co2", .emission = Uid {1}}};
  System b;
  b.emission_zone_array = {
      {.uid = Uid {2}, .name = "la_nox", .emission = Uid {2}}};

  a.merge(std::move(b));
  REQUIRE(a.emission_zone_array.size() == 2);
  CHECK(a.emission_zone_array[0].name == "global_co2");
  CHECK(a.emission_zone_array[1].name == "la_nox");
}
