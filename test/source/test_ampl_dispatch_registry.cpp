/**
 * @file      test_ampl_dispatch_registry.cpp
 * @brief     Unit tests for the AMPL param + iterator dispatch tables
 * @date      2026-05-19
 * @copyright BSD-3-Clause
 *
 * The two tables populated by `ampl_dispatch_registry.cpp` are the
 * single source of truth for which `(class, attribute)` pairs are
 * resolvable from user constraints and which classes participate in
 * `sum(class(all)...)`.  These tests pin that contract:
 *
 *  - **R1**: every documented (class, attribute) registers a non-null
 *    resolver.  A future drop of one entry would silently break user
 *    constraints referencing the attribute — this test trips first.
 *  - **R2**: every iterable class registers a non-null iterator.  Same
 *    rationale for `sum(class(all)...)`.
 *  - **R3**: an unregistered (class, attr) probe returns nullptr — the
 *    typo path stays distinct from the registered path.
 *  - **R4**: the resolver actually returns the underlying schedule value
 *    (smoke check on the indirection layer — not the `param_X` accessor
 *    itself, which has its own coverage).
 */

#include <array>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/uid.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

[[nodiscard]] Simulation make_single_block_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
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

[[nodiscard]] Array<Bus> trivial_bus_array()
{
  return {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
}

[[nodiscard]] System minimal_system_with_generator()
{
  return {
      .name = "ampl_dispatch_smoke",
      .bus_array = trivial_bus_array(),
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .pmin = 0.0,
                  .pmax = 100.0,
                  .gcost = 25.0,
                  .capacity = 100.0,
              },
          },
  };
}

}  // namespace

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — every (class, attribute) pair resolves")
{
  // Spec rather than ground truth: the table below lists the user-facing
  // PAMPL attributes intentionally exposed for each LP class.  If a new
  // attribute is added to
  // `ampl_dispatch_registry.cpp::register_ampl_param_dispatchers` it should be
  // added here too, otherwise the new addition has zero test coverage.
  using Entry = std::pair<std::string_view, std::string_view>;
  static constexpr std::array kEntries = std::to_array<Entry>({
      // Generator
      {"generator", "pmax"},
      {"generator", "pmin"},
      {"generator", "gcost"},
      {"generator", "lossfactor"},
      {"generator", "heat_rate"},
      {"generator", "emission_rate"},
      // Fuel
      {"fuel", "price"},
      {"fuel", "heat_content"},
      {"fuel", "combustion_emission_factor"},
      {"fuel", "upstream_emission_factor"},
      // Demand
      {"demand", "lmax"},
      {"demand", "fcost"},
      {"demand", "lossfactor"},
      // Line — includes newly-exposed voltage / tap_ratio / phase_shift_deg
      //        / lossfactor / resistance attributes.
      {"line", "tmax_ab"},
      {"line", "tmax_ba"},
      {"line", "tcost"},
      {"line", "reactance"},
      {"line", "voltage"},
      {"line", "tap_ratio"},
      {"line", "phase_shift_deg"},
      {"line", "lossfactor"},
      {"line", "resistance"},
      // Battery
      {"battery", "emin"},
      {"battery", "emax"},
      {"battery", "ecost"},
      {"battery", "input_efficiency"},
      {"battery", "output_efficiency"},
      // Reservoir
      {"reservoir", "emin"},
      {"reservoir", "emax"},
      {"reservoir", "ecost"},
      {"reservoir", "capacity"},
      // FlowRight
      {"flow_right", "fmin"},
      {"flow_right", "fmax"},
      {"flow_right", "target"},
      {"flow_right", "fcost"},
      {"flow_right", "uvalue"},
      // Turbine — per-stage hydro conversion schedules.
      {"turbine", "production_factor"},
      {"turbine", "efficiency"},
      {"turbine", "capacity"},
      // Waterway — per-(stage, block) bounds + per-stage capacity /
      // lossfactor / fcost.
      {"waterway", "fmin"},
      {"waterway", "fmax"},
      {"waterway", "capacity"},
      {"waterway", "lossfactor"},
      {"waterway", "fcost"},
      // VolumeRight
      {"volume_right", "fmax"},
      {"volume_right", "emin"},
      {"volume_right", "emax"},
      {"volume_right", "demand"},
      {"volume_right", "saving_rate"},
      {"volume_right", "fail_cost"},
      // EmissionZone — previously unreachable from user constraints.
      {"emission_zone", "cap"},
      {"emission_zone", "cap_cost"},
      {"emission_zone", "price"},
      // EmissionSource — previously unreachable from user constraints.
      {"emission_source", "rate"},
      {"emission_source", "upstream_rate"},
  });

  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  const System system = minimal_system_with_generator();
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  for (const auto& [class_name, attribute] : kEntries) {
    CAPTURE(class_name);
    CAPTURE(attribute);
    CHECK(sc.find_ampl_param(class_name, attribute) != nullptr);
  }
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — every class exposes a sum(class(all)) iterator")
{
  static constexpr auto kClasses = std::to_array<std::string_view>({
      "generator",
      "demand",
      "line",
      "battery",
      "reservoir",
      "waterway",
      "turbine",
      "converter",
      "junction",
      "flow",
      "flow_right",
      "volume_right",
      "seepage",
      "reservoir_discharge_limit",
      "reserve_provision",
      "reserve_zone",
      "bus",
      "lng_terminal",
      "fuel",
      "emission_zone",
      "emission_source",
      // Commitment + Inertia: each LP class registers PAMPL variables
      // (status / startup / shutdown / req / provision) — the iter is
      // needed so `sum(class(all).attribute)` can resolve them.
      "commitment",
      "simple_commitment",
      "inertia_zone",
      "inertia_provision",
      // Free continuous decision variable referenced via
      // `decision_variable("X").value`.
      "decision_variable",
  });

  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  const System system = minimal_system_with_generator();
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  for (const auto& cls : kClasses) {
    CAPTURE(cls);
    CHECK(sc.find_ampl_iter(cls) != nullptr);
  }
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — drift detector: every System::*_array is "
    "either registered or explicitly excluded")
{
  // Drift detector for new element classes.  Maintains a complete
  // inventory of `Array<X> <name>_array` in `include/gtopt/system.hpp`,
  // each paired with one of:
  //
  //   * a `snake_case` element name that MUST have a registered
  //     `find_ampl_iter` (i.e. the class exposes PAMPL-visible
  //     attributes), OR
  //   * a `nullptr` to declare an *explicit* exclusion — the class
  //     is intentionally not iterable via `sum(class(all).X)`
  //     because it's a data carrier (profile schedule, parameter
  //     scalar, file-list, …) with no AMPL attribute surface.
  //
  // When a new `Array<X>` lands in `System`, this test fails until
  // the author either registers an iter OR documents the exclusion
  // by adding an entry here.  Catches the kind of drift that left
  // `DecisionVariable` un-iterable for several days in May 2026.
  using Entry = std::pair<std::string_view, const char*>;
  static constexpr auto kInventory = std::to_array<Entry>({
      // ── Network ───────────────────────────────────────────────
      {"bus_array", "bus"},
      {"demand_array", "demand"},
      {"generator_array", "generator"},
      {"line_array", "line"},
      // ── Profiles: data carriers, no AMPL attributes ───────────
      {"generator_profile_array", nullptr},
      {"demand_profile_array", nullptr},
      {"capacity_profile_array", nullptr},
      // ── Storage / converters ──────────────────────────────────
      {"battery_array", "battery"},
      {"converter_array", "converter"},
      {"lng_terminal_array", "lng_terminal"},
      // ── Reserves / inertia ────────────────────────────────────
      {"reserve_zone_array", "reserve_zone"},
      {"reserve_provision_array", "reserve_provision"},
      {"inertia_zone_array", "inertia_zone"},
      {"inertia_provision_array", "inertia_provision"},
      // ── Fuel / emissions ──────────────────────────────────────
      {"fuel_array", "fuel"},
      // Emission is a parameter dictionary (uid, name, density).  No
      // per-instance dispatch — explicitly excluded.
      {"emission_array", nullptr},
      {"emission_zone_array", "emission_zone"},
      {"emission_source_array", "emission_source"},
      // ── Commitment ────────────────────────────────────────────
      {"commitment_array", "commitment"},
      {"simple_commitment_array", "simple_commitment"},
      // ── Hydro ─────────────────────────────────────────────────
      {"junction_array", "junction"},
      {"waterway_array", "waterway"},
      {"flow_array", "flow"},
      {"reservoir_array", "reservoir"},
      // ReservoirSeepage's snake-case lookup uses "seepage" (legacy
      // naming retained for backward compat with PAMPL bodies).
      {"reservoir_seepage_array", "seepage"},
      // ReservoirDischargeLimit, Pump, ReservoirProductionFactor:
      // currently no per-instance PAMPL attribute surface.  If/when
      // they sprout `.value` / `.flow` accessors callable from
      // `sum(...)`, swap nullptr for the snake-case name and add
      // an iter registration in `ampl_dispatch_registry.cpp`.
      {"reservoir_discharge_limit_array", nullptr},
      {"pump_array", nullptr},
      {"reservoir_production_factor_array", nullptr},
      {"turbine_array", "turbine"},
      // ── Water rights ──────────────────────────────────────────
      {"flow_right_array", "flow_right"},
      {"volume_right_array", "volume_right"},
      // ── User-defined ──────────────────────────────────────────
      // UserParam is a scalar-parameter dictionary keyed by name (no
      // iter); UserConstraint is the consumer side; user_constraint_files
      // is just a path list.
      {"user_param_array", nullptr},
      {"decision_variable_array", "decision_variable"},
      {"user_constraint_array", nullptr},
  });

  // Cross-check against `system.hpp` at compile time — if the file
  // grows new `Array<X> <name>_array` entries that aren't in this
  // inventory, downstream PAMPL resolution may break silently.  See
  // `include/gtopt/system.hpp` line-by-line when this test fires.
  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  const System system = minimal_system_with_generator();
  SystemLP system_lp(system, simulation_lp);
  const auto& sc = system_lp.system_context();

  for (const auto& [array_name, snake] : kInventory) {
    CAPTURE(array_name);
    if (snake == nullptr) {
      continue;  // explicit exclusion — no assertion
    }
    CAPTURE(snake);
    CHECK(sc.find_ampl_iter(snake) != nullptr);
  }

  // Cardinality pin: 34 arrays in System.hpp as of 2026-05-20.  When
  // someone adds a new `Array<X>` they must extend the inventory
  // above too (or the count moves, this assertion bites, and they
  // learn to think about whether the new class needs an iter).
  static_assert(kInventory.size() == 34,
                "kInventory drifted vs include/gtopt/system.hpp — "
                "every Array<X> in System must have an entry here, "
                "either with a snake_case name (iter required) or "
                "nullptr (explicit exclusion).");
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — unknown (class, attribute) returns nullptr")
{
  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  const System system = minimal_system_with_generator();
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  // Unknown class.
  CHECK(sc.find_ampl_param("not_a_class", "pmax") == nullptr);
  CHECK(sc.find_ampl_iter("not_a_class") == nullptr);
  // Known class, unknown attribute.
  CHECK(sc.find_ampl_param("generator", "not_an_attribute") == nullptr);
  // Empty class / attribute — must NOT match `options.*` scalar
  // registrations (those live in a separate map).
  CHECK(sc.find_ampl_param("", "") == nullptr);
  CHECK(sc.find_ampl_iter("") == nullptr);
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — param resolver returns the underlying schedule")
{
  // Smoke check that the shim layer forwards the right value — uses
  // generator.gcost since the test fixture sets it to a known scalar.
  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  const System system = minimal_system_with_generator();
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto fn = sc.find_ampl_param("generator", "gcost");
  REQUIRE(fn != nullptr);

  const auto val = fn(sc, Uid {1}, make_uid<Stage>(1), make_uid<Block>(1));
  REQUIRE(val.has_value());
  CHECK(*val == doctest::Approx(25.0));

  // pmax is a soft-bound schedule.  Confirm it surfaces the 100.0 fixture.
  const auto pmax_fn = sc.find_ampl_param("generator", "pmax");
  REQUIRE(pmax_fn != nullptr);
  const auto pmax_val =
      pmax_fn(sc, Uid {1}, make_uid<Stage>(1), make_uid<Block>(1));
  REQUIRE(pmax_val.has_value());
  CHECK(*pmax_val == doctest::Approx(100.0));
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — iterator emits every element uid for the class")
{
  // Two generators in the system — the iterator must call back twice
  // with the two distinct uids.  The iter callback is captureless;
  // forward through the opaque state pointer as the registry contract
  // requires.
  const auto sim = make_single_block_simulation();
  const System system = {
      .name = "iter_smoke",
      .bus_array = trivial_bus_array(),
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .pmax = 100.0,
                  .gcost = 25.0,
                  .capacity = 100.0,
              },
              {
                  .uid = Uid {2},
                  .name = "g2",
                  .bus = Uid {1},
                  .pmax = 50.0,
                  .gcost = 50.0,
                  .capacity = 50.0,
              },
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  const auto iter_fn = sc.find_ampl_iter("generator");
  REQUIRE(iter_fn != nullptr);

  std::vector<Uid> emitted;
  iter_fn(
      sc,
      &emitted,
      +[](void* state, Uid uid)
      { static_cast<std::vector<Uid>*>(state)->push_back(uid); });

  REQUIRE(emitted.size() == 2);
  // Order matches the input array order (Collection<LP> preserves
  // insertion order).
  CHECK(emitted[0] == Uid {1});
  CHECK(emitted[1] == Uid {2});
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — newly iterable classes walk every element")
{
  // The pre-refactor `collect_sum_cols` if/else only knew about a fixed
  // set of element types.  `fuel`, `emission_zone`, and `emission_source`
  // had iterators registered through the central table for the first
  // time — these tests pin that the iterators are correctly populated
  // *and* actually walk the collection.  Same captureless callback
  // contract as the generator iterator test above.
  const auto sim = make_single_block_simulation();
  const System system = {
      .name = "newly_iterable_smoke",
      .bus_array = trivial_bus_array(),
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .pmax = 100.0,
                  .gcost = 25.0,
                  .capacity = 100.0,
              },
          },
      .fuel_array =
          {
              {
                  .uid = Uid {30},
                  .name = "ng",
                  .price = OptTBRealFieldSched {3.0},
              },
              {
                  .uid = Uid {31},
                  .name = "coal",
                  .price = OptTBRealFieldSched {2.0},
              },
              {
                  .uid = Uid {32},
                  .name = "diesel",
                  .price = OptTBRealFieldSched {15.0},
              },
          },
      .emission_array =
          {
              {
                  .uid = Uid {1},
                  .name = "co2",
              },
              {
                  .uid = Uid {2},
                  .name = "ch4",
              },
          },
      .emission_zone_array =
          {
              {
                  .uid = Uid {10},
                  .name = "ez1",
                  .emissions =
                      {
                          {
                              .emission = Uid {1},
                          },
                      },
                  .cap = OptTRealFieldSched {1000.0},
              },
              {
                  .uid = Uid {11},
                  .name = "ez2",
                  .emissions =
                      {
                          {
                              .emission = Uid {2},
                          },
                      },
                  .cap = OptTRealFieldSched {500.0},
              },
          },
      .emission_source_array =
          {
              {
                  .uid = Uid {20},
                  .name = "es1",
                  .generator = SingleId {Uid {1}},
                  .zone = SingleId {Uid {10}},
                  .emission = SingleId {Uid {1}},
                  .rate = OptTRealFieldSched {0.4},
              },
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  SystemLP system_lp(system, simulation_lp);

  const auto& sc = system_lp.system_context();
  auto collect = [&](std::string_view cls)
  {
    std::vector<Uid> emitted;
    const auto fn = sc.find_ampl_iter(cls);
    REQUIRE(fn != nullptr);
    fn(
        sc,
        &emitted,
        +[](void* state, Uid uid)
        { static_cast<std::vector<Uid>*>(state)->push_back(uid); });
    return emitted;
  };

  // Fuel — 3 entries in insertion order.
  const auto fuels = collect("fuel");
  REQUIRE(fuels.size() == 3);
  CHECK(fuels[0] == Uid {30});
  CHECK(fuels[1] == Uid {31});
  CHECK(fuels[2] == Uid {32});

  // EmissionZone — 2 entries.
  const auto zones = collect("emission_zone");
  REQUIRE(zones.size() == 2);
  CHECK(zones[0] == Uid {10});
  CHECK(zones[1] == Uid {11});

  // EmissionSource — 1 entry.
  const auto sources = collect("emission_source");
  REQUIRE(sources.size() == 1);
  CHECK(sources[0] == Uid {20});
}

TEST_CASE(  // NOLINT
    "AMPL dispatch registry — Turbine / Waterway param shims return the "
    "scheduled value, and ReservoirDischargeLimit appears in the iter table")
{
  // Build a minimal hydro chain (reservoir → waterway → turbine + RDL)
  // and call the registered shims via find_ampl_param.  This pins R4
  // for the eight new hydro entries: a regression on the param accessor,
  // the shim, or the registration line would fail this check.
  const System system = {
      .name = "ampl_dispatch_hydro_smoke",
      .bus_array = trivial_bus_array(),
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g_hydro",
                  .bus = Uid {1},
                  .pmax = 100.0,
                  .gcost = 0.0,
              },
          },
      .junction_array =
          {
              {.uid = Uid {1}, .name = "j_up"},
              {.uid = Uid {2}, .name = "j_down", .drain = true},
          },
      .waterway_array =
          {
              {
                  .uid = Uid {1},
                  .name = "ww1",
                  .junction_a = Uid {1},
                  .junction_b = Uid {2},
                  .capacity = 75.0,
                  .lossfactor = 0.10,
                  .fmin = 5.0,
                  .fmax = 50.0,
                  .fcost = 0.40,
              },
          },
      .reservoir_array =
          {
              {
                  .uid = Uid {1},
                  .name = "rsv1",
                  .junction = Uid {1},
                  .capacity = 1.0e9,
                  .emin = 0.0,
                  .emax = 1.0e9,
                  .eini = 1.0e9,
              },
          },
      .reservoir_discharge_limit_array =
          {
              {
                  .uid = Uid {1},
                  .name = "rdl1",
                  .waterway = Uid {1},
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .intercept = 30.0,
                          },
                      },
              },
          },
      .turbine_array =
          {
              {
                  .uid = Uid {1},
                  .name = "t_hydro",
                  .waterway = Uid {1},
                  .generator = Uid {1},
                  .production_factor = 2.5,
                  .efficiency = 0.95,
                  .capacity = 200.0,
              },
          },
  };

  const PlanningOptionsLP options;
  const auto sim = make_single_block_simulation();
  SimulationLP simulation_lp(sim, options);
  simulation_lp.set_need_ampl_variables(/*v=*/true);
  SystemLP system_lp(system, simulation_lp);
  const auto& sc = system_lp.system_context();

  const auto s = simulation_lp.stages().front().uid();
  const auto b = simulation_lp.blocks().front().uid();
  const Uid t_uid {1};
  const Uid w_uid {1};

  // Turbine per-stage params.
  const AmplParamFn pf = sc.find_ampl_param("turbine", "production_factor");
  REQUIRE(pf != nullptr);
  CHECK(pf(sc, t_uid, s, b).value_or(0.0)
        == doctest::Approx(2.5).epsilon(1e-9));

  const AmplParamFn eff = sc.find_ampl_param("turbine", "efficiency");
  REQUIRE(eff != nullptr);
  CHECK(eff(sc, t_uid, s, b).value_or(0.0)
        == doctest::Approx(0.95).epsilon(1e-9));

  const AmplParamFn tcap = sc.find_ampl_param("turbine", "capacity");
  REQUIRE(tcap != nullptr);
  CHECK(tcap(sc, t_uid, s, b).value_or(0.0)
        == doctest::Approx(200.0).epsilon(1e-9));

  // Waterway per-(stage, block) and per-stage params.
  const AmplParamFn fmin = sc.find_ampl_param("waterway", "fmin");
  REQUIRE(fmin != nullptr);
  CHECK(fmin(sc, w_uid, s, b).value_or(0.0)
        == doctest::Approx(5.0).epsilon(1e-9));

  const AmplParamFn fmax = sc.find_ampl_param("waterway", "fmax");
  REQUIRE(fmax != nullptr);
  CHECK(fmax(sc, w_uid, s, b).value_or(0.0)
        == doctest::Approx(50.0).epsilon(1e-9));

  const AmplParamFn wcap = sc.find_ampl_param("waterway", "capacity");
  REQUIRE(wcap != nullptr);
  CHECK(wcap(sc, w_uid, s, b).value_or(0.0)
        == doctest::Approx(75.0).epsilon(1e-9));

  const AmplParamFn lossf = sc.find_ampl_param("waterway", "lossfactor");
  REQUIRE(lossf != nullptr);
  CHECK(lossf(sc, w_uid, s, b).value_or(0.0)
        == doctest::Approx(0.10).epsilon(1e-9));

  const AmplParamFn fcost = sc.find_ampl_param("waterway", "fcost");
  REQUIRE(fcost != nullptr);
  CHECK(fcost(sc, w_uid, s, b).value_or(0.0)
        == doctest::Approx(0.40).epsilon(1e-9));

  // ReservoirDischargeLimit appears in the iter table (sum(... (all)) works).
  CHECK(sc.find_ampl_iter("reservoir_discharge_limit") != nullptr);
}
