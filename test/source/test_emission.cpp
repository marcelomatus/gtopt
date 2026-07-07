// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the `Emission` pollutant tag (registry only — no LP
// rows / columns).  Cap / price / slack moved to `EmissionZone` and
// per-generator rates to `EmissionSource` (see test_emission_zone.cpp
// and test_emission_source.cpp for the LP-active surface).

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/emission.hpp>
#include <gtopt/emission_lp.hpp>
#include <gtopt/json/json_emission.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("Emission default construction")  // NOLINT
{
  const Emission e;
  CHECK(e.uid == Uid {unknown_uid});
  CHECK(e.name.empty());
  CHECK_FALSE(e.active.has_value());
}

TEST_CASE("Emission attribute assignment")  // NOLINT
{
  Emission e;
  e.uid = Uid {42};
  e.name = "co2";
  e.active = true;

  CHECK(e.uid == Uid {42});
  CHECK(e.name == "co2");
  REQUIRE(e.active.has_value());
  CHECK(std::get<IntBool>(e.active.value_or(Active {False})) == 1);
}

TEST_CASE("Emission JSON round-trip — minimal pollutant tag")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 7,
    "name": "co2"
  })";

  const auto e = daw::json::from_json<Emission>(src);
  CHECK(e.uid == Uid {7});
  CHECK(e.name == "co2");

  // Re-emit and re-parse — the minimal shape survives.
  const auto rendered = daw::json::to_json(e);
  const auto e2 = daw::json::from_json<Emission>(rendered);
  CHECK(e2.uid == e.uid);
  CHECK(e2.name == e.name);
}

TEST_CASE("Emission JSON — minimal (only uid + name)")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 9,
    "name": "so2"
  })";

  const auto e = daw::json::from_json<Emission>(src);
  CHECK(e.name == "so2");
  CHECK_FALSE(e.active.has_value());
}

TEST_CASE("Emission survives the System → SystemLP pipeline")  // NOLINT
{
  // Pollutant tag is passive — its presence in the system must NOT
  // affect feasibility, row/col counts, or objective.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmissionPassiveTest",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The collection is populated and reachable.
  const auto& emissions = system_lp.elements<EmissionLP>();
  REQUIRE(emissions.size() == 1);
  CHECK(emissions.front().emission().name == "co2");
  CHECK(emissions.front().emission().uid == Uid {1});
}

TEST_CASE(
    "Emission JSON — full System round-trip includes emission_array")  // NOLINT
{
  constexpr std::string_view src = R"({
    "name": "EmRound",
    "emission_array": [
      {"uid": 1, "name": "co2"},
      {"uid": 2, "name": "so2"}
    ]
  })";

  const auto sys = daw::json::from_json<System>(src);
  REQUIRE(sys.emission_array.size() == 2);
  CHECK(sys.emission_array[0].name == "co2");
  CHECK(sys.emission_array[1].name == "so2");

  // Re-emit, re-parse.
  const auto rendered = daw::json::to_json(sys);
  const auto sys2 = daw::json::from_json<System>(rendered);
  REQUIRE(sys2.emission_array.size() == 2);
  CHECK(sys2.emission_array[0].name == "co2");
}

TEST_CASE("System::merge preserves both emission_array rows")  // NOLINT
{
  System a;
  a.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  System b;
  b.emission_array = {{.uid = Uid {2}, .name = "so2"}};

  a.merge(std::move(b));
  REQUIRE(a.emission_array.size() == 2);
  CHECK(a.emission_array[0].name == "co2");
  CHECK(a.emission_array[1].name == "so2");
}
