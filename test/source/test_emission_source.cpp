// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the `EmissionSource` generator↔zone bridge entity and its
// passive `EmissionSourceLP` wrapper (Commit 2 of the emissions
// ladder).  Also exercises `System::expand_emission_sources()` —
// the parse-time pass that folds inline `Generator.emissions[]`
// entries into the top-level `emission_source_array`.

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/emission_source.hpp>
#include <gtopt/emission_source_lp.hpp>
#include <gtopt/json/json_emission_source.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("EmissionSource default construction")  // NOLINT
{
  const EmissionSource s;
  CHECK(s.uid == Uid {unknown_uid});
  CHECK(s.name.empty());
  CHECK_FALSE(s.generator.has_value());
  CHECK(s.zone == SingleId {unknown_uid});
  CHECK_FALSE(s.rate.has_value());
}

TEST_CASE("EmissionSource JSON round-trip")  // NOLINT
{
  constexpr std::string_view src = R"({
    "uid": 1,
    "name": "ngcc_la_to_global_co2",
    "generator": "ngcc_la",
    "zone": "global_co2",
    "rate": 0.4
  })";

  const auto s = daw::json::from_json<EmissionSource>(src);
  CHECK(s.name == "ngcc_la_to_global_co2");
  REQUIRE(s.generator.has_value());
  CHECK(std::get<Name>(s.generator.value_or(SingleId {Uid {0}})) == "ngcc_la");
  CHECK(std::get<Name>(s.zone) == "global_co2");
  REQUIRE(s.rate.has_value());
  CHECK(std::get<Real>(s.rate.value_or(Real {0.0})) == doctest::Approx(0.4));
}

TEST_CASE(
    "System::expand_emission_sources moves Generator.emissions[] inline "
    "into emission_source_array")  // NOLINT
{
  System sys;
  sys.generator_array = {
      {
          .uid = Uid {42},
          .name = "ngcc_la",
          .emissions =
              {
                  {.zone = Uid {1}, .rate = 0.4},
                  {.zone = Uid {2}, .rate = 0.05},
              },
      },
  };

  sys.expand_emission_sources();

  // Inline list cleared on the generator.
  CHECK(sys.generator_array.front().emissions.empty());

  // Two rows now live in the flat array, stamped with the parent
  // generator FK and auto-named.
  REQUIRE(sys.emission_source_array.size() == 2);
  for (const auto& es : sys.emission_source_array) {
    REQUIRE(es.generator.has_value());
    CHECK(std::get<Uid>(es.generator.value_or(SingleId {Uid {0}})) == Uid {42});
    CHECK_FALSE(es.name.empty());  // auto-labelled
    CHECK(es.uid != Uid {unknown_uid});
  }

  // Idempotent: a second call is a no-op (inline list already empty).
  sys.expand_emission_sources();
  CHECK(sys.emission_source_array.size() == 2);
}

TEST_CASE(
    "EmissionSource — inline JSON omits uid/name/generator (auto-filled)")  // NOLINT
{
  // The inline form on Generator.emissions[] is the user ergonomics —
  // only `zone` and `rate` are needed; uid/name/generator are
  // auto-filled by `System::expand_emission_sources()`.  Verify that
  // a bare EmissionSource JSON with only `zone` (and optional `rate`)
  // parses cleanly.
  constexpr std::string_view src = R"({
    "zone": "global_co2", "rate": 0.4
  })";

  const auto s = daw::json::from_json<EmissionSource>(src);
  // When JSON omits `uid`, daw::json default-constructs the field
  // (Uid{0} — NOT the struct's brace-init `unknown_uid` sentinel).
  // `System::expand_emission_sources()` collision-detects this and
  // assigns a fresh uid at expansion time, so this is harmless.
  CHECK(s.name.empty());
  CHECK_FALSE(s.generator.has_value());
  CHECK(std::get<Name>(s.zone) == "global_co2");
  REQUIRE(s.rate.has_value());
  CHECK(std::get<Real>(s.rate.value_or(Real {0.0})) == doctest::Approx(0.4));
}

TEST_CASE(
    "EmissionSource — Generator inline emissions[] parses (full fixture)")  // NOLINT
{
  // Full minimal Generator JSON (with the required `bus` etc.) plus
  // the inline emissions[] list.
  constexpr std::string_view src = R"({
    "uid": 1, "name": "ngcc",
    "bus": 1, "gcost": 10.0, "capacity": 200.0,
    "emissions": [
      {"zone": "global_co2", "rate": 0.4},
      {"zone": "la_nox",     "rate": 0.05}
    ]
  })";

  const auto g = daw::json::from_json<Generator>(src);
  REQUIRE(g.emissions.size() == 2);
  CHECK(std::get<Name>(g.emissions[0].zone) == "global_co2");
  CHECK(std::get<Name>(g.emissions[1].zone) == "la_nox");
  CHECK(std::get<Real>(g.emissions[0].rate.value_or(Real {0.0}))
        == doctest::Approx(0.4));
}

TEST_CASE("EmissionSource survives the System → SystemLP pipeline")  // NOLINT
{
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmissionSourceSurvival",
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
                               .emission = Uid {1}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .rate = 0.4}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& sources = system_lp.elements<EmissionSourceLP>();
  REQUIRE(sources.size() == 1);
  CHECK(sources.front().emission_source().name == "g1_co2");
  const auto sid = simulation_lp.stages().front().uid();
  CHECK(sources.front().param_rate(sid).value_or(0.0) == doctest::Approx(0.4));
}
