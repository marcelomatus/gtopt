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
      {"generator", "emission_factor"},
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
      "generator",     "demand",
      "line",          "battery",
      "reservoir",     "waterway",
      "turbine",       "converter",
      "junction",      "flow",
      "flow_right",    "volume_right",
      "seepage",       "reserve_provision",
      "reserve_zone",  "bus",
      "lng_terminal",  "fuel",
      "emission_zone", "emission_source",
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
                  .price = OptTRealFieldSched {3.0},
              },
              {
                  .uid = Uid {31},
                  .name = "coal",
                  .price = OptTRealFieldSched {2.0},
              },
              {
                  .uid = Uid {32},
                  .name = "diesel",
                  .price = OptTRealFieldSched {15.0},
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
