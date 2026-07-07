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

using namespace gtopt;

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
    "emission": "co2",
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
                  {.zone = Uid {1}, .emission = Uid {1}, .rate = 0.4},
                  {.zone = Uid {2}, .emission = Uid {2}, .rate = 0.05},
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
  // only `zone`, `emission`, and `rate` are needed; uid/name/generator
  // are auto-filled by `System::expand_emission_sources()`.
  constexpr std::string_view src = R"({
    "zone": "global_co2", "emission": "co2", "rate": 0.4
  })";

  const auto s = daw::json::from_json<EmissionSource>(src);
  // When JSON omits `uid`, daw::json default-constructs the field
  // (Uid{0} — NOT the struct's brace-init `unknown_uid` sentinel).
  // `System::expand_emission_sources()` collision-detects this and
  // assigns a fresh uid at expansion time, so this is harmless.
  CHECK(s.name.empty());
  CHECK_FALSE(s.generator.has_value());
  CHECK(std::get<Name>(s.zone) == "global_co2");
  CHECK(std::get<Name>(s.emission) == "co2");
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
      {"zone": "global_co2", "emission": "co2", "rate": 0.4},
      {"zone": "la_nox",     "emission": "nox", "rate": 0.05}
    ]
  })";

  const auto g = daw::json::from_json<Generator>(src);
  REQUIRE(g.emissions.size() == 2);
  CHECK(std::get<Name>(g.emissions[0].zone) == "global_co2");
  CHECK(std::get<Name>(g.emissions[0].emission) == "co2");
  CHECK(std::get<Name>(g.emissions[1].zone) == "la_nox");
  CHECK(std::get<Name>(g.emissions[1].emission) == "nox");
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
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
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

// ── Commit 7 — legacy auto-fold ─────────────────────────────────────

TEST_CASE(
    "System::fold_legacy_emission_rate — synthesizes default CO2 zone+source")  // NOLINT
{
  // Generator carries the legacy scalar `emission_rate` field;
  // expect post-fold:
  //   - emission_array has a CO2 row
  //   - emission_zone_array has a default_co2 zone
  //   - emission_source_array has the synthesized source
  //   - generator.emission_rate stays populated ("don't zero,
  //     accumulate") so a co-existing fuel-derived EmissionSource
  //     adds to (rather than replaces) the direct emission rate
  System sys;
  sys.generator_array = {{.uid = Uid {7},
                          .name = "ngcc",
                          .bus = Uid {1},
                          .gcost = 10.0,
                          .capacity = 100.0,
                          .emission_rate = 0.42}};

  sys.fold_legacy_emission_rate();

  REQUIRE(sys.emission_array.size() == 1);
  CHECK(sys.emission_array.front().name == "co2");

  REQUIRE(sys.emission_zone_array.size() == 1);
  const auto& zone = sys.emission_zone_array.front();
  CHECK(zone.name == "default_co2");
  REQUIRE(zone.emissions.size() == 1);
  CHECK(zone.emissions.front().weight.value_or(0.0) == doctest::Approx(1.0));

  REQUIRE(sys.emission_source_array.size() == 1);
  const auto& src = sys.emission_source_array.front();
  REQUIRE(src.generator.has_value());
  CHECK(std::get<Uid>(src.generator.value_or(SingleId {Uid {0}})) == Uid {7});
  REQUIRE(src.rate.has_value());
  CHECK(std::get<Real>(src.rate.value_or(Real {0.0})) == doctest::Approx(0.42));

  // "Don't zero" — legacy field stays populated after the fold.
  REQUIRE(sys.generator_array.front().emission_rate.has_value());
  CHECK(std::get<Real>(sys.generator_array.front().emission_rate.value_or(0.0))
        == doctest::Approx(0.42));

  // Idempotent: name dedup short-circuits even though the legacy
  // field is still populated.
  sys.fold_legacy_emission_rate();
  CHECK(sys.emission_source_array.size() == 1);
}

TEST_CASE(
    "System::fold_legacy_emission_rate — direct + fuel-derived accumulate")  // NOLINT
{
  // A generator with BOTH a direct `emission_rate` AND a fuel that
  // carries `combustion + upstream` factors should produce TWO
  // independent EmissionSource rows after the planning pipeline runs.
  // Together they sum at the EmissionZone cap — "accumulate, don't
  // overwrite".
  System sys;
  // Pre-populate the CO₂ pollutant + covering zone explicitly so
  // both `fuel.emission_factors[].emission` and `zone.emissions[].emission`
  // resolve to the same `SingleId{Uid{...}}` form (avoids the
  // Name-vs-Uid mismatch in `expand_fuel_emission_sources`'s
  // `covers` predicate).
  sys.emission_array = {Emission {.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {EmissionZone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = SingleId {Uid {1}}, .weight = 1.0}},
  }};
  sys.fuel_array = {Fuel {
      .uid = Uid {1},
      .name = "gas",
      .price = 5.0,
      .emission_factors = {{.emission = SingleId {Uid {1}},
                            .combustion = OptTRealFieldSched {0.18},
                            .upstream = OptTRealFieldSched {0.02}}},
  }};
  sys.generator_array = {Generator {
      .uid = Uid {7},
      .name = "ngcc",
      .bus = Uid {1},
      .gcost = 10.0,
      .fuel = OptSingleId {SingleId {Uid {1}}},
      .heat_rate = OptTBRealFieldSched {7.0},  // 7 GJ/MWh
      .capacity = 100.0,
      .emission_rate = 0.42,  // direct, non-combustion
  }};

  // Order matches PlanningLP::expand_pre_lp_passes(): fuel-derived
  // sources first, then the legacy-rate fold.
  sys.expand_fuel_emission_sources();  // 7 × (0.18 + 0.02) per MWh
  sys.fold_legacy_emission_rate();  // reuses existing co2 + zone

  REQUIRE(sys.emission_source_array.size() == 2);

  const auto legacy = std::ranges::find_if(
      sys.emission_source_array,
      [](const EmissionSource& s) { return s.name.contains("co2_legacy"); });
  const auto via_fuel = std::ranges::find_if(
      sys.emission_source_array,
      [](const EmissionSource& s) { return s.name.contains("via_fuel"); });
  REQUIRE(legacy != sys.emission_source_array.end());
  REQUIRE(via_fuel != sys.emission_source_array.end());

  REQUIRE(legacy->rate.has_value());
  CHECK(std::get<Real>(legacy->rate.value_or(0.0)) == doctest::Approx(0.42));

  REQUIRE(via_fuel->rate.has_value());
  CHECK(std::get<Real>(via_fuel->rate.value_or(0.0))
        == doctest::Approx(7.0 * 0.18));
  REQUIRE(via_fuel->upstream_rate.has_value());
  CHECK(std::get<Real>(via_fuel->upstream_rate.value_or(0.0))
        == doctest::Approx(7.0 * 0.02));

  // Both passes are idempotent.
  sys.fold_legacy_emission_rate();
  sys.expand_fuel_emission_sources();
  CHECK(sys.emission_source_array.size() == 2);
}

TEST_CASE(
    "System::fold_legacy_emission_rate — reuses existing CO2 entities")  // NOLINT
{
  // When a CO2 Emission and a covering EmissionZone already exist,
  // the fold should NOT create duplicates — only synthesize the
  // missing EmissionSource row.
  System sys;
  sys.emission_array = {{.uid = Uid {99}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {7},
       .name = "custom_co2_cap",
       .emissions = {{.emission = Uid {99}, .weight = 1.0}},
       .cap = 1.0e6}};
  sys.generator_array = {
      {.uid = Uid {1}, .name = "g1", .bus = Uid {1}, .emission_rate = 0.4}};

  sys.fold_legacy_emission_rate();

  CHECK(sys.emission_array.size() == 1);  // no new CO2
  CHECK(sys.emission_zone_array.size() == 1);  // no new zone
  REQUIRE(sys.emission_source_array.size() == 1);
  const auto& src = sys.emission_source_array.front();
  CHECK(std::get<Uid>(src.zone) == Uid {7});  // points at existing zone
  CHECK(std::get<Uid>(src.emission) == Uid {99});
}

// ── Commit XX — heat_content multiplication (CEN-Chile mass basis) ─────

TEST_CASE(
    "expand_fuel_emission_sources — mass-basis heat_rate × heat_content × "
    "combustion (CEN-Chile convention)")  // NOLINT
{
  // PLEXOS / CEN-Chile ships ``Generator.heat_rate`` in fuel-MASS units
  // per MWh (tonnes_coal/MWh, m³_gas/MWh, …) with ``Fuel.heat_content``
  // carrying the energy density (GJ per fuel-unit).  The IPCC
  // combustion factor is energy-basis (tCO2/GJ).  The unit-correct
  // synthesized ``EmissionSource.rate`` is therefore
  //   tCO2/MWh = heat_rate [unit/MWh] × heat_content [GJ/unit]
  //                                   × combustion [tCO2/GJ]
  // This test pins the heat_content multiplication on BOTH the
  // combustion (rate) and upstream (upstream_rate) paths.  Reference
  // values come from CEN PCP 2026-04-12 SANTA_MARIA (coal): heat_rate
  // = 0.346 t_coal/MWh, heat_content = 25.8 GJ/t_coal, combustion =
  // 0.0946 tCO2/GJ (IPCC default for sub-bituminous coal).
  System sys;
  sys.emission_array = {Emission {.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {EmissionZone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = SingleId {Uid {1}}, .weight = 1.0}},
  }};
  sys.fuel_array = {Fuel {
      .uid = Uid {1},
      .name = "coal",
      .price = 60.0,
      .heat_content = OptTRealFieldSched {25.8},  // coal NCV: GJ/t_coal
      .emission_factors = {{.emission = SingleId {Uid {1}},
                            .combustion = OptTRealFieldSched {0.0946},
                            .upstream = OptTRealFieldSched {0.005}}},
  }};
  sys.generator_array = {Generator {
      .uid = Uid {7},
      .name = "santa_maria",
      .bus = Uid {1},
      .gcost = 10.0,
      .fuel = OptSingleId {SingleId {Uid {1}}},
      .heat_rate = OptTBRealFieldSched {0.346},  // t_coal/MWh (mass basis)
      .capacity = 350.0,
  }};

  sys.expand_fuel_emission_sources();

  REQUIRE(sys.emission_source_array.size() == 1);
  const auto& src = sys.emission_source_array.front();
  REQUIRE(src.generator.has_value());
  CHECK(std::get<Uid>(src.generator.value_or(SingleId {Uid {0}})) == Uid {7});

  // combustion path: 0.346 × 25.8 × 0.0946 ≈ 0.8444 tCO2/MWh
  REQUIRE(src.rate.has_value());
  CHECK(std::get<Real>(src.rate.value_or(Real {0.0}))
        == doctest::Approx(0.346 * 25.8 * 0.0946));
  // upstream path: 0.346 × 25.8 × 0.005 ≈ 0.04464 tCO2/MWh
  REQUIRE(src.upstream_rate.has_value());
  CHECK(std::get<Real>(src.upstream_rate.value_or(Real {0.0}))
        == doctest::Approx(0.346 * 25.8 * 0.005));
}

TEST_CASE(
    "expand_fuel_emission_sources — heat_content==0 preserves legacy "
    "hr × combustion formula")  // NOLINT
{
  // Mirror image of the mass-basis test: when ``Fuel.heat_content`` is
  // unset (the gtopt default used by sddp2gtopt, plp2gtopt synthetic
  // Fuels, 118-Bus virtual fuels — anything that already ships
  // ``heat_rate`` in GJ/MWh), the formula must collapse to the legacy
  // ``hr × combustion`` byte-for-byte.  The hc=1.0 fallback in
  // ``expand_fuel_emission_sources`` is what guarantees this no-op
  // semantics when ``scalar_or(fuel->heat_content)`` returns 0.
  System sys;
  sys.emission_array = {Emission {.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {EmissionZone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = SingleId {Uid {1}}, .weight = 1.0}},
  }};
  sys.fuel_array = {Fuel {
      .uid = Uid {1},
      .name = "gas_energy_basis",
      .price = 5.0,
      // heat_content deliberately UNSET → scalar_or returns 0 → hc
      // falls back to 1.0 (no-op multiplier).
      .emission_factors = {{.emission = SingleId {Uid {1}},
                            .combustion = OptTRealFieldSched {0.18}}},
  }};
  sys.generator_array = {Generator {
      .uid = Uid {7},
      .name = "ngcc",
      .bus = Uid {1},
      .gcost = 10.0,
      .fuel = OptSingleId {SingleId {Uid {1}}},
      .heat_rate = OptTBRealFieldSched {7.0},  // already in GJ/MWh
      .capacity = 100.0,
  }};

  sys.expand_fuel_emission_sources();

  REQUIRE(sys.emission_source_array.size() == 1);
  const auto& src = sys.emission_source_array.front();
  REQUIRE(src.rate.has_value());
  // Legacy formula: 7.0 × 0.18 = 1.26 tCO2/MWh — NO heat_content
  // factor applied because hc collapsed to the 1.0 fallback.
  CHECK(std::get<Real>(src.rate.value_or(Real {0.0}))
        == doctest::Approx(7.0 * 0.18));
  // No upstream factor declared → no upstream_rate emitted.
  CHECK_FALSE(src.upstream_rate.has_value());
}

// ── Commit XX — tolerant lookup for zero-pmax skipped generator ────────

TEST_CASE(
    "EmissionSourceLP::add_to_lp — tolerant of zero-pmax skipped generator "
    "(no flat_map::at throw)")  // NOLINT
{
  // End-to-end pin for the ``lookup_generation_cols`` migration in
  // ``source/emission_source_lp.cpp``.  Pre-fix the per-block loop
  // called ``gen.generation_cols_at(scenario, stage)`` which throws
  // ``std::out_of_range`` when the (scenario, stage) outer key is
  // absent — and the P1 zero-pmax optimization in GeneratorLP elides
  // that outer key whenever every block of a (scenario, stage) hits
  // bounds=[0,0].  Symptom in the wild: PLEXOS CEN bundles ship
  // alternate-fuel-mode variants (``*_GNL_X``, ``*_DIE``) with
  // capacity=0 for some weeks, and any EmissionSource pointing at one
  // of those crashed LP build.  The fix swaps to the tolerant
  // ``lookup_generation_cols`` (returns an empty inner map) plus an
  // early ``return true`` when the map is empty.
  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1.0},
              {.uid = Uid {2}, .duration = 2.0},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmissionSourceZeroPmaxGen",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 10.0}},
      .generator_array =
          {
              // Zero-capacity unit — P1 skip elides every column.
              {.uid = Uid {1},
               .name = "g_zero_with_emission",
               .bus = Uid {1},
               .gcost = 50.0,
               .capacity = 0.0},
              // Backup so the LP is feasible without emissions.
              {.uid = Uid {2},
               .name = "g_backup",
               .bus = Uid {1},
               .gcost = 100.0,
               .capacity = 1000.0},
          },
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "global_co2",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      // EmissionSource attached to the zero-pmax generator — this is
      // the exact configuration that pre-fix crashed at LP build.
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g_zero_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = 0.4}},
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  // Construction must not throw despite the missing outer key on
  // ``generation_cols`` for (scenario=0, stage=1).
  REQUIRE_NOTHROW(SystemLP {system, simulation_lp});

  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Sanity: the EmissionSource survived the System → SystemLP pipeline
  // (no silent drop on the LP side either).
  const auto& sources = system_lp.elements<EmissionSourceLP>();
  REQUIRE(sources.size() == 1);
  CHECK(sources.front().emission_source().name == "g_zero_co2");
}

// ────────────────────────────────────────────────────────────────────────
// PAMPL accessor `emission_source('X').emissions` (issue #529 follow-up
// audit, gap G2).  After the EmissionZone.production substitute-out
// the per-source emission expression `Σ_b α_b · gen_b` is exposed as
// an AMPL weighted-sum-of-cols attribute — no aggregator LP column.
// A user constraint `emission_source('X').emissions <= K` must
// translate to direct coefficient stamps on the generator dispatch
// columns at constraint-build time.
// ────────────────────────────────────────────────────────────────────────
TEST_CASE(
    "PAMPL emission_source('X').emissions UC binds via weighted-sum")  // NOLINT
{
  // 1-block, 1-stage.  Zone is pure reporting (no cap / price / pool).
  // The UC `emissions <= 10.0` constrains net per-block tonnage; with
  // rate = 0.4 the gen is capped at 25 MW; 25 MWh of the 50 MW demand
  // is unserved and pays the fail-cost.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  constexpr double kRate = 0.4;
  constexpr double kDemand = 50.0;
  constexpr double kCap = 10.0;
  constexpr double kFailCost = 1000.0;
  // Gen capped at 25 MW (rate × gen ≤ 10) ⇒ 25 MWh unserved at $1000/MWh.
  // Plus gcost on the 25 MW served.
  constexpr double kGenAllowed = kCap / kRate;  // 25
  constexpr double kUnserved = kDemand - kGenAllowed;  // 25
  constexpr double kGcost = 10.0;
  constexpr double kExpectedObj = kGenAllowed * kGcost + kUnserved * kFailCost;

  const System system = {
      .name = "EmissionSourcePamplUC",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = kFailCost,
                        .capacity = kDemand}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = kGcost,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "report_only",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = kRate}},
      .user_constraint_array = {{.uid = Uid {1},
                                 .name = "uc_emissions_cap",
                                 .expression =
                                     "emission_source('g1_co2').emissions "
                                     "<= 10.0"}},
  };

  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {popts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  CHECK(lp.get_obj_value() == doctest::Approx(kExpectedObj).epsilon(1e-6));
}

// ────────────────────────────────────────────────────────────────────────
// Audit gap G7 (issue #529): when an `EmissionSource` references a
// pollutant that the targeted zone does NOT list in its
// `emissions[]` table, the LP must build cleanly (no cap row, no
// pool drawdown, no tax adder) — the source contributes zero by
// definition.  The current code logs a WARN and returns true; this
// test pins the LP-side invariant (zone stays reporting-only) and
// the objective stays at the baseline dispatch cost.
// ────────────────────────────────────────────────────────────────────────
TEST_CASE(
    "EmissionSourceLP: pollutant not in zone.emissions skips cleanly")  // NOLINT
{
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  constexpr double kGcost = 10.0;
  constexpr double kGen = 50.0;
  // Baseline obj = gcost · gen · dur = 10 · 50 · 1 = 500.  Any
  // emission-side contamination (cap, price, pool drawdown) would
  // shift this away from 500 — pinning it proves the silent-skip.
  constexpr double kExpectedObj = kGcost * kGen * 1.0;

  const System system = {
      .name = "EmissionSourcePollutantMismatch",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1.0e6,
                        .capacity = kGen}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = kGcost,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"},
                         {.uid = Uid {2}, .name = "ch4"}},
      // Zone covers ONLY CO₂; sets a cap so the path would normally
      // build a cap row + slack — proves the source's mismatch
      // doesn't accidentally trip those.
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "co2_only",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}},
                               .cap = 1.0,
                               .cap_cost = 1.0}},
      // Source declares emission = ch4 (uid=2) — NOT in zone's emissions.
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_ch4",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {2},
                                 .rate = 0.1}},
  };

  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {popts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // The cap row IS built (cap is set on the zone, gated independently
  // from source contributions).  But the source contributes zero to
  // it (pollutant mismatch), so the cap is trivially non-binding —
  // slack stays at 0, obj = baseline dispatch cost.
  CHECK(lp.get_obj_value() == doctest::Approx(kExpectedObj).epsilon(1e-6));

  // Slack column exists (cap+cap_cost set) but is 0 (no emissions
  // contributing).
  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  const auto& slack_cols = zone.cap_slack_cols();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg_uid = simulation_lp.stages().front().uid();
  REQUIRE(slack_cols.find({scen_uid, stg_uid}) != slack_cols.end());
  const auto slack_col = slack_cols.at({scen_uid, stg_uid});
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[slack_col] == doctest::Approx(0.0).epsilon(1e-9));
}
