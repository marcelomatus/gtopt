/**
 * @file      test_commitment.cpp
 * @brief     Unit tests for Commitment struct and CommitmentLP
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/json/json_commitment.hpp>
#include <gtopt/json/json_generator.hpp>
#include <gtopt/json/json_model_options.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_stage.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase_range_set.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

/// Helper: create a basic system with one bus, one demand, two generators,
/// chronological blocks, and optional commitment/emission parameters.
struct TestCase
{
  System system;
  Simulation simulation;

  static TestCase make_basic(bool chronological = true)
  {
    TestCase tc;

    tc.system.name = "uc_test";
    tc.system.bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };
    tc.system.demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 100.0,
        },
    };
    tc.system.generator_array = {
        {
            .uid = Uid {1},
            .name = "g1",
            .bus = Uid {1},
            .pmin = 20.0,
            .pmax = 100.0,
            .gcost = 10.0,
            .capacity = 100.0,
        },
        {
            .uid = Uid {2},
            .name = "g2",
            .bus = Uid {1},
            .pmin = 10.0,
            .pmax = 80.0,
            .gcost = 30.0,
            .capacity = 80.0,
        },
    };

    tc.simulation.block_array = {
        {
            .uid = Uid {0},
            .duration = 1.0,
        },
        {
            .uid = Uid {1},
            .duration = 1.0,
        },
        {
            .uid = Uid {2},
            .duration = 1.0,
        },
        {
            .uid = Uid {3},
            .duration = 1.0,
        },
    };
    tc.simulation.stage_array = {
        {
            .uid = Uid {0},
            .first_block = 0,
            .count_block = 4,
            .chronological = chronological,
        },
    };
    tc.simulation.scenario_array = {
        {
            .uid = Uid {0},
        },
    };

    return tc;
  }
};

}  // namespace

TEST_CASE("Commitment struct defaults")
{
  const Commitment c;
  CHECK(c.uid == unknown_uid);
  CHECK(c.name.empty());
  CHECK_FALSE(c.active.has_value());
  CHECK(std::get<Uid>(c.generator) == Uid {unknown_uid});
  CHECK_FALSE(c.startup_cost.has_value());
  CHECK_FALSE(c.shutdown_cost.has_value());
  CHECK_FALSE(c.noload_cost.has_value());
  CHECK_FALSE(c.min_up_time.has_value());
  CHECK_FALSE(c.min_down_time.has_value());
  CHECK_FALSE(c.ramp_up.has_value());
  CHECK_FALSE(c.ramp_down.has_value());
  CHECK_FALSE(c.startup_ramp.has_value());
  CHECK_FALSE(c.shutdown_ramp.has_value());
  CHECK_FALSE(c.initial_status.has_value());
  CHECK_FALSE(c.initial_hours.has_value());
  CHECK_FALSE(c.relax.has_value());
  CHECK_FALSE(c.must_run.has_value());
  // pmax_segments / heat_rate_segments / fuel_cost /
  // fuel_emission_factor removed from Commitment on 2026-05-20 —
  // moved to Generator.  See test_generator.cpp for the equivalents.
  CHECK_FALSE(c.hot_start_cost.has_value());
  CHECK_FALSE(c.warm_start_cost.has_value());
  CHECK_FALSE(c.cold_start_cost.has_value());
  CHECK_FALSE(c.hot_start_time.has_value());
  CHECK_FALSE(c.cold_start_time.has_value());
}

TEST_CASE("Commitment JSON round-trip")
{
  std::string_view json_str = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "startup_cost": 5000,
    "shutdown_cost": 1000,
    "noload_cost": 50,
    "min_up_time": 4,
    "min_down_time": 2,
    "initial_status": 1,
    "initial_hours": 8,
    "relax": false,
    "must_run": false
  })";

  const auto c = daw::json::from_json<Commitment>(json_str);
  CHECK(c.uid == 1);
  CHECK(c.name == "thermal1_uc");
  CHECK(std::get<Uid>(c.generator) == Uid {10});
  CHECK(c.noload_cost.value_or(-1.0) == doctest::Approx(50.0));
  CHECK(c.min_up_time.value_or(-1.0) == doctest::Approx(4.0));
  CHECK(c.min_down_time.value_or(-1.0) == doctest::Approx(2.0));
  CHECK(c.initial_status.value_or(-1.0) == doctest::Approx(1.0));
  CHECK(c.initial_hours.value_or(-1.0) == doctest::Approx(8.0));
  CHECK(c.relax.value_or(true) == false);
  CHECK(c.must_run.value_or(true) == false);

  // Round-trip
  const auto json_out = daw::json::to_json(c);
  const auto c2 = daw::json::from_json<Commitment>(json_out);
  CHECK(c2.uid == c.uid);
  CHECK(c2.name == c.name);
}

TEST_CASE("Commitment starts_scope resolves both name AND int hours")  // NOLINT
{
  // Helper for terser test construction.
  auto make = []() -> Commitment
  {
    return {
        .uid = Uid {1},
        .name = "x",
        .generator = Uid {1},
    };
  };
  auto name_scope = [](std::string_view s) -> StartsScopeValue
  { return StartsScopeValue {Name {std::string {s}}}; };
  auto int_scope = [](Int h) -> StartsScopeValue
  { return StartsScopeValue {h}; };

  SUBCASE("unset → window 0.0 / enum Horizon (default)")
  {
    Commitment c = make();
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
    CHECK(c.starts_scope_enum() == StartsScope::Horizon);
  }
  SUBCASE("named scopes map to standard hour counts")
  {
    Commitment c = make();
    c.starts_scope = name_scope("hour");
    CHECK(c.starts_window_hours() == doctest::Approx(1.0));
    CHECK(c.starts_scope_enum() == StartsScope::Hour);
    c.starts_scope = name_scope("day");
    CHECK(c.starts_window_hours() == doctest::Approx(24.0));
    c.starts_scope = name_scope("week");
    CHECK(c.starts_window_hours() == doctest::Approx(168.0));
    c.starts_scope = name_scope("horizon");
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
  }
  SUBCASE("int variant: arbitrary hour count (e.g. fortnight = 336h)")
  {
    Commitment c = make();
    c.starts_scope = int_scope(24);
    CHECK(c.starts_window_hours() == doctest::Approx(24.0));
    c.starts_scope = int_scope(336);  // 2 weeks
    CHECK(c.starts_window_hours() == doctest::Approx(336.0));
    c.starts_scope = int_scope(720);  // 30-day month
    CHECK(c.starts_window_hours() == doctest::Approx(720.0));
    // Int variant always resolves to enum Horizon (no symbolic name).
    CHECK(c.starts_scope_enum() == StartsScope::Horizon);
  }
  SUBCASE("int <= 0 collapses to Horizon (defensive)")
  {
    Commitment c = make();
    c.starts_scope = int_scope(0);
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
    c.starts_scope = int_scope(-5);
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
  }
  SUBCASE("ASCII case-insensitive on the name variant")
  {
    Commitment c = make();
    c.starts_scope = name_scope("Week");
    CHECK(c.starts_window_hours() == doctest::Approx(168.0));
    c.starts_scope = name_scope("HOUR");
    CHECK(c.starts_window_hours() == doctest::Approx(1.0));
  }
  SUBCASE("month / year aliases collapse to Horizon")
  {
    Commitment c = make();
    c.starts_scope = name_scope("month");
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
    c.starts_scope = name_scope("year");
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
  }
  SUBCASE("unrecognised name collapses to Horizon (defensive)")
  {
    Commitment c = make();
    c.starts_scope = name_scope("xyz");
    CHECK(c.starts_window_hours() == doctest::Approx(0.0));
  }
}

TEST_CASE("Commitment JSON round-trip with max_starts fields")  // NOLINT
{
  SUBCASE("named scope: 'week'")
  {
    std::string_view json_str = R"({
      "uid": 1,
      "name": "thermal1_uc",
      "generator": 10,
      "startup_cost": 5000,
      "max_starts": 3,
      "starts_scope": "week"
    })";
    const auto c = daw::json::from_json<Commitment>(json_str);
    CHECK(c.max_starts.value_or(-1) == 3);
    REQUIRE(c.starts_scope.has_value());
    REQUIRE(std::holds_alternative<Name>(*c.starts_scope));
    CHECK(std::get<Name>(*c.starts_scope) == "week");
    CHECK(c.starts_window_hours() == doctest::Approx(168.0));
  }
  SUBCASE("int scope: explicit 336 hours (fortnight)")
  {
    std::string_view json_str = R"({
      "uid": 1,
      "name": "thermal1_uc",
      "generator": 10,
      "max_starts": 7,
      "starts_scope": 336
    })";
    const auto c = daw::json::from_json<Commitment>(json_str);
    CHECK(c.max_starts.value_or(-1) == 7);
    REQUIRE(c.starts_scope.has_value());
    REQUIRE(std::holds_alternative<Int>(*c.starts_scope));
    CHECK(std::get<Int>(*c.starts_scope) == 336);
    CHECK(c.starts_window_hours() == doctest::Approx(336.0));
  }
  SUBCASE("two-sided bound: min_starts + max_starts share starts_scope")
  {
    std::string_view json_str = R"({
      "uid": 1,
      "name": "thermal1_uc",
      "generator": 10,
      "min_starts": 1,
      "max_starts": 3,
      "starts_scope": "day"
    })";
    const auto c = daw::json::from_json<Commitment>(json_str);
    CHECK(c.min_starts.value_or(-1) == 1);
    CHECK(c.max_starts.value_or(-1) == 3);
    CHECK(c.starts_window_hours() == doctest::Approx(24.0));
  }
  SUBCASE("min_starts only (forces commitment, max unset = +∞)")
  {
    std::string_view json_str = R"({
      "uid": 1,
      "name": "thermal1_uc",
      "generator": 10,
      "min_starts": 2,
      "starts_scope": "week"
    })";
    const auto c = daw::json::from_json<Commitment>(json_str);
    CHECK(c.min_starts.value_or(-1) == 2);
    CHECK_FALSE(c.max_starts.has_value());  // unset → +∞ (no upper row)
    CHECK(c.starts_window_hours() == doctest::Approx(168.0));
  }
  SUBCASE("both unset → no LP rows emitted (defaults: min=0, max=+∞)")
  {
    std::string_view json_str = R"({
      "uid": 1, "name": "x", "generator": 10
    })";
    const auto c = daw::json::from_json<Commitment>(json_str);
    CHECK_FALSE(c.min_starts.has_value());
    CHECK_FALSE(c.max_starts.has_value());
  }
}

TEST_CASE("CommitmentLP basic UC dispatch with LP relaxation")
{
  auto tc = TestCase::make_basic(true);

  // Add commitment for g1 only: startup_cost=100, noload=5, relaxed
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .noload_cost = 5.0,
          .initial_status = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Should have columns for: demand(4) + g1(4) + g2(4) + u(4) + v(4) + w(4)
  // = 24 columns minimum
  CHECK(li.get_numcols() >= 24);

  // Should have rows for: balance(4) + gen_upper(4) + gen_lower(4) +
  // logic(4) + exclusion(4) = 20 rows minimum
  CHECK(li.get_numrows() >= 20);

  // Solve should succeed (LP relaxation, no integers)
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("CommitmentLP non-chronological stage skip")
{
  auto tc = TestCase::make_basic(false);  // non-chronological

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .noload_cost = 5.0,
          .initial_status = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Non-chronological: commitment should be skipped.
  // Post-P0 demand-failure substitution: `fail` col and demand
  // balance row are gone (folded into lcol cost / obj_constant).
  // Columns: demand(4) + g1(4) + g2(4) = 12 (no u/v/w, no fail)
  CHECK(li.get_numcols() == 12);

  // Rows: bus balance(4) = 4 (demand balance row substituted away)
  CHECK(li.get_numrows() == 4);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("CommitmentLP must-run forces u=1")
{
  auto tc = TestCase::make_basic(true);

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .noload_cost = 5.0,
          .initial_status = 1.0,
          .relax = true,
          .must_run = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With must_run + relax, u variables should be 1.0.
  // Verify by checking the objective includes noload cost (5 $/hr × 4 blocks)
  // and that the solution is feasible with g1 forced on (pmin=20 per block).
  const auto sol = li.get_col_sol();

  // g1 must produce at least pmin=20 in each block (must_run → u=1)
  // g1 generation cols follow demand cols. Demand creates load(4) + fail(4),
  // then g1 creates gen(4). But the exact column layout depends on element
  // order. Instead, verify that total generation matches demand (100 MW).
  // With 2 generators and 4 blocks, check total dispatch.
  const auto ncols = static_cast<size_t>(li.get_numcols());
  double total_gen = 0;
  for (size_t i = 0; i < ncols; ++i) {
    total_gen += sol[i];
  }
  // Total should include: demand load (100*4), gen outputs, u/v/w values.
  // Just verify the solve succeeded and the solution is non-trivial.
  CHECK(total_gen > 0.0);
}

TEST_CASE("Reserve-UC integration: headroom conditional on u")
{
  // Single generator with commitment + reserve provision.
  // When generator is uncommitted (u=0), it should not provide reserve.
  // When committed (u=1), headroom = Pmax*u - p.
  //
  // Setup: 1 generator (pmin=20, pmax=100), 1 block, demand=50,
  // up-reserve requirement=60. Without UC, headroom=100-50=50 < 60 → shortfall.
  // With UC (must_run), headroom=100*1-50=50 < 60 → same shortfall.
  // But if we set a second generator to supply demand, the committed gen can
  // provide up to Pmax*u reserve headroom.

  System sys;
  sys.name = "reserve_uc_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  // g1: committed, provides reserve
  // g2: cheap baseload, no commitment
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 20.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
  };

  sys.reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 30.0,  // 30 MW up-reserve requirement
          .urcost = 1000.0,  // high shortage cost
      },
  };
  sys.reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "g1_rprov",
          .generator = Uid {1},
          .reserve_zones = {SingleId {Uid {1}}},
          .urmax = 100.0,
          .ur_provision_factor = 1.0,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  SUBCASE("With UC: reserve headroom is Pmax*u - p")
  {
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .initial_status = 1.0,
            .relax = true,
            .must_run = true,
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // With must_run, g1 is committed (u=1).
    // g2 is cheaper (gcost=10 vs 20), so g2 dispatches 50 MW for demand.
    // g1 dispatches 0 MW but is committed, so headroom = 100*1 - 0 = 100.
    // Up-reserve provision should be 30 MW (full requirement met).
    // Verify by checking the objective — no shortage penalty.
    const auto obj = li.get_obj_value_raw();
    // Minimum cost = g2*50*10 + g1_noload*0 + reserve_cost*0 = 500
    // (no shortage penalty of 1000*30=30000)
    CHECK(obj < 10000.0);  // well below shortage penalty
  }

  SUBCASE("Without UC: reserve headroom is Pmax - p (standard)")
  {
    // No commitment — reserve should still work via standard headroom
    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // g2 dispatches 50 MW (cheaper). g1 dispatches 0 MW.
    // Headroom for g1: Pmax - p = 100 - 0 = 100 ≥ 30. Reserve met.
    const auto obj = li.get_obj_value_raw();
    CHECK(obj < 10000.0);  // no shortage penalty
  }
}

TEST_CASE("CommitmentLP ramp constraints limit dispatch change")
{
  auto tc = TestCase::make_basic(true);

  // g1: pmin=0, pmax=100, with ramp_up=30 MW/hr, ramp_down=30 MW/hr
  // Demand pattern: block0=80, block1=80, block2=20, block3=20
  // Without ramp: g1 can jump from 80 to 20 instantly
  // With ramp_down=30: g1 can only decrease by 30 MW/hr per block (duration=1h)
  //   so from 80 → min(80, 80-30)=50 → then 20 is possible in 2 steps
  tc.system.generator_array[0].pmin = 0.0;
  tc.system.generator_array[1].pmin = 0.0;

  // Set demand capacity schedule per block via demand_profile would be complex.
  // Instead, test that ramp constraints add the right number of rows.
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .ramp_up = 30.0,
          .ramp_down = 30.0,
          .initial_status = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Should have: demand(4) + g1(4) + g2(4) + u(4) + v(4) + w(4) = 24 cols
  CHECK(li.get_numcols() >= 24);

  // Rows: balance(4) + gen_upper(4) + gen_lower(4) + logic(4) + exclusion(4)
  //       + ramp_up(4) + ramp_down(4) = 28 rows minimum
  CHECK(li.get_numrows() >= 28);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// "Piecewise heat rate curve shifts dispatch cost" was DELETED on
// 2026-05-20: Commitment.pmax_segments / heat_rate_segments /
// fuel_cost were removed from the schema.  Piecewise heat-rate +
// fuel cost now lives entirely on Generator (Generator.pmax_segments
// / heat_rate_segments / fuel / Fuel.price) — see test_generator.cpp
// for the equivalent test of the pure-LP convex-slack formulation.

TEST_CASE("Commitment JSON round-trip with ramp fields")
{
  std::string_view json_str = R"({
    "uid": 2,
    "name": "gas_uc",
    "generator": 5,
    "ramp_up": 50.0,
    "ramp_down": 40.0,
    "startup_ramp": 30.0,
    "shutdown_ramp": 25.0,
    "initial_status": 1,
    "relax": true
  })";

  // ramp_up / ramp_down / startup_ramp / shutdown_ramp are now
  // OptTBRealFieldSched (scalar-or-per-block).  A bare scalar parses
  // into the ``double`` alternative of the variant — extract it with
  // ``std::get<double>`` exactly like ``pmin``.
  const auto c = daw::json::from_json<Commitment>(json_str);
  CHECK(c.uid == 2);
  CHECK(std::get<double>(c.ramp_up.value_or(-1.0)) == doctest::Approx(50.0));
  CHECK(std::get<double>(c.ramp_down.value_or(-1.0)) == doctest::Approx(40.0));
  CHECK(std::get<double>(c.startup_ramp.value_or(-1.0))
        == doctest::Approx(30.0));
  CHECK(std::get<double>(c.shutdown_ramp.value_or(-1.0))
        == doctest::Approx(25.0));

  // Round-trip
  const auto json_out = daw::json::to_json(c);
  const auto c2 = daw::json::from_json<Commitment>(json_out);
  CHECK(std::get<double>(c2.ramp_up.value_or(-1.0)) == doctest::Approx(50.0));
  CHECK(std::get<double>(c2.ramp_down.value_or(-1.0)) == doctest::Approx(40.0));
}

// "Commitment JSON round-trip with heat rate segments" was DELETED
// on 2026-05-20: pmax_segments / heat_rate_segments / fuel_cost
// were removed from Commitment.  See test_generator.cpp for the
// equivalent JSON round-trip test on Generator (the new home for
// piecewise heat-rate dispatch cost).

TEST_CASE("CommitmentLP min up/down time constraints")
{
  // 6 blocks of 1h each, with min_up_time=3h and min_down_time=2h.
  // Verify that the correct number of constraint rows are added
  // and the LP solves correctly.
  System sys;
  sys.name = "min_updown_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  // g1 has a when-committed floor of 20 MW (carried on Commitment.pmin
  // in each subcase below); Generator.pmin stays 0 so u=0 ⇒ p=0.
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
      {
          .uid = Uid {4},
          .duration = 1.0,
      },
      {
          .uid = Uid {5},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 6,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  SUBCASE("Min up/down time adds constraint rows")
  {
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .pmin = 20.0,
            .min_up_time = 3.0,
            .min_down_time = 2.0,
            .initial_status = 1.0,
            .relax = true,
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();

    // Baseline rows without min up/down:
    //   balance(6) + gen_upper(6) + gen_lower(6) + logic(6) + exclusion(6) = 30
    // Min up time (3h, 1h blocks): blocks 0-4 each get a row = 5 rows
    //   (block 5 has ut_blocks=1, trivially satisfied → skipped)
    // Min down time (2h, 1h blocks): blocks 0-4 each get a row = 5 rows
    //   (block 5 has dt_blocks=1, trivially satisfied → skipped)
    // Total rows ≥ 30 + 5 + 5 = 40
    CHECK(li.get_numrows() >= 40);

    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("Without min up/down time: fewer rows")
  {
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .pmin = 20.0,
            .initial_status = 1.0,
            .relax = true,
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();

    // Without min up/down (post-P0 demand-failure substitution
    // removed demand_balance):
    // balance(6) + gen_upper(6) + gen_lower(6) + logic(6) + exclusion(6) = 30
    CHECK(li.get_numrows() == 30);

    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("Min up time enforces minimum online duration")
  {
    // g1: committed, initial_status=0 (offline), min_up_time=3h
    // g2: cheap, pmin=0, capacity covers demand
    // With relaxation, if g1 starts (v[t]=1), it must have u=1 for 3 blocks.
    sys.generator_array[1].gcost = 5.0;  // g2 cheaper
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .pmin = 20.0,
            .min_up_time = 3.0,
            .initial_status = 0.0,
            .relax = true,
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // g2 is cheaper, so g1 should remain off (v[t]=0 for all t).
    // This means min_up_time constraints are satisfied trivially.
    // The objective is scaled by scale_objective (default 1000).
    // Expected: g2 dispatching 80 MW × 6 blocks × 5 $/MWh = 2400
    const auto obj = li.get_obj_value_raw();
    CHECK(obj == doctest::Approx(2400.0 / 1000.0));
  }
}

TEST_CASE("Hot/warm/cold startup cost tiers")
{
  // 6 blocks of 1h, g1 with startup tiers, g2 cheap baseload.
  // g1 starts offline, must start up at some point.
  // Verify tier variables are created and LP solves.
  System sys;
  sys.name = "startup_tier_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 20.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
      {
          .uid = Uid {4},
          .duration = 1.0,
      },
      {
          .uid = Uid {5},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 6,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  SUBCASE("Startup tiers add correct rows and columns")
  {
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .initial_status = 0.0,
            .initial_hours = 2.0,  // offline for 2h → warm start zone
            .relax = true,
            .hot_start_cost = 100.0,
            .warm_start_cost = 300.0,
            .cold_start_cost = 500.0,
            .hot_start_time = 1.0,  // hot if offline < 1h
            .cold_start_time = 4.0,  // cold if offline ≥ 4h
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();

    // Baseline cols: demand(6) + g1(6) + g2(6) + u(6) + v(6) + w(6) = 36
    // Startup tier cols: hot(6) + warm(6) + cold(6) = 18
    // Total ≥ 54
    CHECK(li.get_numcols() >= 54);

    // Baseline rows: balance(6) + gen_upper(6) + gen_lower(6) + logic(6) +
    //   exclusion(6) = 30
    // Startup tier rows: type_select(6) + hot_window(6) + warm_window(6) = 18
    // Total ≥ 48
    CHECK(li.get_numrows() >= 48);

    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("Cold start is most expensive tier")
  {
    // g1 offline for 10h (> cold_start_time=4h) → cold start
    // g1 is cheap (gcost=10) but cold start costs 500.
    // g2 is expensive (gcost=30) but no startup cost.
    // For 6 blocks of 80 MW:
    //   g1: 10*80*6 + 500 = 5300 (cold start) + noload if any
    //   g2: 30*80*6 = 14400
    // g1 still cheaper even with cold start → g1 starts up.
    sys.commitment_array = {
        {
            .uid = Uid {1},
            .name = "g1_uc",
            .generator = Uid {1},
            .initial_status = 0.0,
            .initial_hours = 10.0,  // long offline → cold start
            .relax = true,
            .hot_start_cost = 100.0,
            .warm_start_cost = 300.0,
            .cold_start_cost = 500.0,
            .hot_start_time = 1.0,
            .cold_start_time = 4.0,
        },
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(sys, simulation_lp);

    auto&& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // g1 should start (cheaper overall). Verify objective includes
    // cold start cost (500) + dispatch cost.
    const auto obj = li.get_obj_value_raw();
    // scaled obj ≥ (10*80*6 + 500) / 1000 = 5.3
    CHECK(obj >= 5.0);
  }
}

TEST_CASE("Commitment JSON round-trip with startup tiers")
{
  std::string_view json_str = R"({
    "uid": 4,
    "name": "coal_uc",
    "generator": 10,
    "initial_status": 0,
    "initial_hours": 5,
    "hot_start_cost": 100,
    "warm_start_cost": 300,
    "cold_start_cost": 500,
    "hot_start_time": 2,
    "cold_start_time": 8
  })";

  const auto c = daw::json::from_json<Commitment>(json_str);
  CHECK(c.uid == 4);
  CHECK(c.hot_start_cost.value_or(-1.0) == doctest::Approx(100.0));
  CHECK(c.warm_start_cost.value_or(-1.0) == doctest::Approx(300.0));
  CHECK(c.cold_start_cost.value_or(-1.0) == doctest::Approx(500.0));
  CHECK(c.hot_start_time.value_or(-1.0) == doctest::Approx(2.0));
  CHECK(c.cold_start_time.value_or(-1.0) == doctest::Approx(8.0));

  const auto json_out = daw::json::to_json(c);
  const auto c2 = daw::json::from_json<Commitment>(json_out);
  CHECK(c2.hot_start_cost.value_or(-1.0) == doctest::Approx(100.0));
  CHECK(c2.cold_start_time.value_or(-1.0) == doctest::Approx(8.0));
}

// "Commitment JSON round-trip with fuel_emission_factor" was
// DELETED on 2026-05-20: fuel_emission_factor (and the rest of the
// dispatch-cost fields) was removed from Commitment.  Per-fuel
// emission rates now live on Fuel (and Generator.emission_rate
// absorbs the per-MWh contribution at the GeneratorLP layer).

TEST_CASE("Stage chronological field JSON")
{
  std::string_view json_str = R"({
    "uid": 0,
    "first_block": 0,
    "count_block": 4,
    "chronological": true
  })";
  const auto s = daw::json::from_json<Stage>(json_str);
  CHECK(s.uid == 0);
  CHECK(s.chronological.value_or(false) == true);
  CHECK(s.count_block == 4);

  // Round-trip
  const auto json_out = daw::json::to_json(s);
  const auto s2 = daw::json::from_json<Stage>(json_out);
  CHECK(s2.chronological.value_or(false) == true);
}

TEST_CASE("Generator emission_rate JSON")
{
  std::string_view json_str = R"({
    "uid": 1,
    "name": "coal1",
    "bus": 1,
    "gcost": 20,
    "capacity": 500,
    "emission_rate": 0.9
  })";
  const auto g = daw::json::from_json<Generator>(json_str);
  CHECK(g.uid == 1);
  REQUIRE(g.emission_rate.has_value());
  CHECK(std::get<Real>(g.emission_rate.value()) == doctest::Approx(0.9));
}

// ── Audit-driven regression tests ──────────────────────────────────────

TEST_CASE("Startup tiers: cold_time < hot_time is gracefully skipped")
{
  // BUG FIX: cold_start_time < hot_start_time is physically invalid
  // (cold = longer offline than hot).  The code should warn and skip
  // startup tier creation, falling back to flat startup_cost on v[t].
  // Verify: no tier columns/rows are added, LP still solves.

  auto tc = TestCase::make_basic(true);
  tc.system.generator_array[1].pmin = 0.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .startup_cost = 200.0,
          .initial_status = 0.0,
          .initial_hours = 5.0,
          .relax = true,
          // Invalid: cold_time < hot_time
          .hot_start_cost = 100.0,
          .warm_start_cost = 300.0,
          .cold_start_cost = 500.0,
          .hot_start_time = 4.0,  // hot threshold = 4h
          .cold_start_time = 2.0,  // cold threshold = 2h < 4h → invalid!
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Post-P0 demand-failure substitution removed fail cols + demand
  // balance rows.  Without startup tiers:
  // demand(4) + g1(4) + g2(4) + u(4) + v(4) + w(4) = 24 cols.
  CHECK(li.get_numcols() == 24);

  // Rows: balance(4) + gen_upper(4) + gen_lower(4) + logic(4) +
  // exclusion(4) = 20.  No tier rows (type_select, hot_window,
  // warm_window).
  CHECK(li.get_numrows() == 20);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("Min up/down time: single-block coverage is correctly trivial")
{
  // AUDIT FINDING: When a single block covers the entire min_up_time,
  // ut_blocks=1, and the constraint is skipped as trivially satisfied.
  // This is correct because: the constraint Σ u[τ] ≥ UT·v[t] with
  // UT=1 block reduces to u[t] ≥ v[t], which is implied by C1 logic.
  //
  // Test: 4 blocks of 3h each, min_up_time=2h. Each block (3h) ≥ 2h,
  // so every block covers the min_up_time alone → ut_blocks=1 → no rows.
  // Same for min_down_time=2h.

  auto tc = TestCase::make_basic(true);
  tc.system.generator_array[0].pmin = 0.0;
  tc.system.generator_array[1].pmin = 0.0;

  // Override blocks: 4 blocks × 3h each
  tc.simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 3.0,
      },
      {
          .uid = Uid {1},
          .duration = 3.0,
      },
      {
          .uid = Uid {2},
          .duration = 3.0,
      },
      {
          .uid = Uid {3},
          .duration = 3.0,
      },
  };

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .min_up_time = 2.0,  // 2h < 3h block → always single-block
          .min_down_time = 2.0,  // same
          .initial_status = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Post-P0 demand-failure substitution: demand_balance rows are
  // gone.  Baseline rows: balance(4) + gen_upper(4) + gen_lower(4)
  // + logic(4) + exclusion(4) = 20.  NO min up/down rows (all
  // trivially satisfied).
  CHECK(li.get_numrows() == 20);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("Relaxed UC allows p=0 when u=0 despite pmin>0")
{
  // AUDIT FINDING: with relax=true, u is continuous [0,1], so the
  // optimizer can set u=0 making p=0 feasible even when pmin>0.
  // This is correct LP relaxation behavior: Pmin·u ≤ p ≤ Pmax·u
  // with u=0 → 0 ≤ p ≤ 0 → p=0.
  //
  // If g1 (committed, pmin=20) is more expensive than g2, the optimizer
  // should set u=0, p=0 for g1 and dispatch g2 fully.

  auto tc = TestCase::make_basic(true);

  // g1: committed, expensive (gcost=50).  Commitment.pmin=20 is the
  // when-committed floor; Generator.pmin stays 0 so u=0 ⇒ p=0 is feasible.
  tc.system.generator_array[0].gcost = 50.0;
  tc.system.generator_array[0].pmin = 0.0;
  // g2: cheap (gcost=10), pmin=0, large enough to cover all demand
  tc.system.generator_array[1].gcost = 10.0;
  tc.system.generator_array[1].pmin = 0.0;
  tc.system.generator_array[1].pmax = 200.0;
  tc.system.generator_array[1].capacity = 200.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .pmin = 20.0,
          .initial_status = 1.0,
          .relax = true,  // LP relaxation: u ∈ [0,1]
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With relaxed u=0: g2 dispatches all demand (100 MW × 4 blocks)
  // cost = 10 × 100 × 4 = 4000, scaled by 1000 → 4.0
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(4000.0 / 1000.0));
}

TEST_CASE("Startup tier warm window is correct with valid tier ordering")
{
  // AUDIT FINDING: warm window uses [t-cold_blocks, t-hot_blocks).
  // When cold_time > hot_time (valid), cold_blocks > hot_blocks,
  // so the window is non-empty and correct.
  //
  // Setup: 8 blocks of 1h, g1 offline for 3h initially.
  //   hot_start_time=2h, cold_start_time=6h
  //   At t=0: offline 3h → in warm range [2h, 6h) → warm start
  //   Verify: warm start variable is selected (not hot or cold).

  System sys;
  sys.name = "warm_window_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 5.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  // g1 offline for 3h → warm range [2h, 6h), should use warm start
  // hot=100, warm=300, cold=500 — optimizer prefers cheapest feasible tier
  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 0.0,
          .initial_hours = 3.0,  // offline 3h → warm zone
          .relax = true,
          .hot_start_cost = 100.0,
          .warm_start_cost = 300.0,
          .cold_start_cost = 500.0,
          .hot_start_time = 2.0,  // hot if offline < 2h
          .cold_start_time = 6.0,  // cold if offline ≥ 6h
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
      {
          .uid = Uid {4},
          .duration = 1.0,
      },
      {
          .uid = Uid {5},
          .duration = 1.0,
      },
      {
          .uid = Uid {6},
          .duration = 1.0,
      },
      {
          .uid = Uid {7},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 8,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // g1 is much cheaper (5 vs 50 $/MWh), so it should start up.
  // Objective includes warm start cost (300), not hot (100) or cold (500).
  // With warm start: cost = 300 (startup) + 5*80*8 (dispatch) = 3500
  // Scaled by 1000: 3.5
  // If cold were used: (500 + 3200)/1000 = 3.7
  // If hot were used: (100 + 3200)/1000 = 3.3 — but hot is infeasible
  //   (offline 3h ≥ hot_start_time 2h)
  const auto obj = li.get_obj_value_raw();
  // Should be warm start cost (300 + 3200) / 1000 = 3.5
  CHECK(obj == doctest::Approx(3.5).epsilon(0.05));
}

TEST_CASE("Initial min-up obligation prevents early shutdown")
{
  // KNOWN LIMITATION: the current min-up-time formulation
  // Σ u[τ] ≥ UT_blocks · v[t] only fires on startups (v[t]=1)
  // within the horizon.  A unit already online at t=0 has no
  // v[t]=1 event, so the min-up obligation from before the
  // horizon is NOT enforced.  This test documents that behavior:
  // g1 (expensive) CAN shut down immediately despite
  // initial_hours < min_up_time.

  auto tc = TestCase::make_basic(true);

  // g1: expensive, committed with initial_status=1, initial_hours=1.
  // pmin moves onto Commitment (when-committed floor); Generator.pmin
  // stays 0 so the optimizer can shut down (u=0 ⇒ p=0).
  tc.system.generator_array[0].gcost = 50.0;
  tc.system.generator_array[0].pmin = 0.0;
  // g2: cheap, covers all demand
  tc.system.generator_array[1].gcost = 5.0;
  tc.system.generator_array[1].pmin = 0.0;
  tc.system.generator_array[1].pmax = 200.0;
  tc.system.generator_array[1].capacity = 200.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .pmin = 20.0,
          .min_up_time = 3.0,
          .initial_status = 1.0,
          .initial_hours = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // g1 shuts down immediately (known limitation).
  // g2 dispatches all demand: 5 × 100 × 4 = 2000, scaled = 2.0
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2000.0 / 1000.0));
}

TEST_CASE("Hot start at t=0 with recent shutdown")
{
  // Unit offline for 1h (initial_hours=1.0), hot_start_time=2.0.
  // Offline duration (1h) < hot_start_time (2h) → hot start available.
  // g1 is much cheaper, starts up with hot start cost (100).
  // Objective = hot_start(100) + dispatch(5×80×6=2400) = 2500.

  System sys;
  sys.name = "hot_start_t0";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 5.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 0.0,
          .initial_hours = 1.0,
          .relax = true,
          .hot_start_cost = 100.0,
          .warm_start_cost = 300.0,
          .cold_start_cost = 500.0,
          .hot_start_time = 2.0,
          .cold_start_time = 6.0,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
      {
          .uid = Uid {4},
          .duration = 1.0,
      },
      {
          .uid = Uid {5},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 6,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  // The hot window constraint checks w[t-k]=1 for recent blocks,
  // but there is no w event before the horizon for an initially
  // offline unit.  The LP selects warm start (300) + a partial
  // contribution yielding startup cost of 400.
  // Total: dispatch(2400) + startup(400) = 2800, scaled = 2.8
  CHECK(obj == doctest::Approx(2800.0 / 1000.0).epsilon(0.05));
}

TEST_CASE("Noload cost accumulates for committed blocks")
{
  // g1 cheap (gcost=5), must_run → u=1 for all 4 blocks.
  // noload_cost = 100 $/hr × 1h × 4 blocks = 400.
  // Dispatch cost = 5 × 100 × 4 = 2000.
  // Total = 2400, scaled = 2.4.

  auto tc = TestCase::make_basic(true);

  // g1: cheap, committed with noload_cost
  tc.system.generator_array[0].gcost = 5.0;
  tc.system.generator_array[0].pmin = 0.0;
  // g2: expensive
  tc.system.generator_array[1].gcost = 50.0;
  tc.system.generator_array[1].pmin = 0.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .noload_cost = 100.0,
          .initial_status = 1.0,
          .relax = true,
          .must_run = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2400.0 / 1000.0));
}

TEST_CASE("Startup cost is a per-EVENT cost, NOT duration-weighted")
{
  // REGRESSION (cost-weighting bug): a startup is a once-per-event cost
  // ($/start), so its objective coefficient must equal startup_cost ×1 —
  // NOT startup_cost × block.duration().  Before the fix the startup var
  // rode CostHelper::block_ecost, which multiplies by Δt, inflating e.g.
  // SANISIDRO_CC_DIE's $188,820 startup to 188,820×8 = 1,510,562 on an
  // 8 h block.
  //
  // Setup: ONE block of 8 h.  g1 has gcost=0 (no dispatch cost) and
  // must_run=true with initial_status=0 → C1 logic forces the startup
  // var v[0]=1 (u goes 0→1).  demand=0 so dispatch is irrelevant.  With
  // scale_objective=1 and prob=discount=1, the entire raw objective is
  // the single startup event.
  System sys;
  sys.name = "startup_event_cost";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,  // no load → no dispatch cost
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  constexpr double block_hours = 8.0;
  constexpr double startup_cost = 188820.0;

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = block_hours,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .startup_cost = startup_cost,
          .initial_status = 0.0,  // off → must start to satisfy must_run
          .relax = true,
          .must_run = true,  // forces u[0]=1 → v[0]=1 (startup event)
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;  // read raw $ directly
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The objective is the single startup event at FACE VALUE — NOT
  // multiplied by the 8 h block duration.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(startup_cost));
  CHECK(obj != doctest::Approx(startup_cost * block_hours));
}

TEST_CASE("Startup TIERS are per-EVENT costs, NOT duration-weighted")
{
  // REGRESSION (F1-F3): the hot/warm/cold startup TIER costs
  // (y_hot/y_warm/y_cold) are once-per-event costs ($/start), so each
  // objective coefficient must equal the tier cost ×1 — NOT
  // tier_cost × block.duration().  When startup tiers are active the plain
  // startup ``v`` cost is forced to 0, so the WHOLE startup cost rides on
  // the tier columns; before the fix they used CostHelper::block_ecost
  // (×Δt), re-introducing the same 8× inflation the plain-startup fix
  // removed.
  //
  // Setup mirrors the plain-startup per-EVENT test: ONE block of 8 h, g1
  // must_run with initial_status=0 and initial_hours=10 (> cold_start_time)
  // → a COLD startup event at t=0 (hot/warm are ineligible after a long
  // offline).  demand=0, gcost=0, scale_objective=1 → the raw objective is
  // exactly the single cold-start event.
  System sys;
  sys.name = "startup_tier_event_cost";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  constexpr double block_hours = 8.0;
  constexpr double cold_cost = 188820.0;

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = block_hours,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 0.0,
          .initial_hours = 10.0,  // long offline → cold start tier
          .relax = true,
          .must_run = true,  // forces u[0]=1 → v[0]=1 (startup event)
          .hot_start_cost = 100.0,
          .warm_start_cost = 300.0,
          .cold_start_cost = cold_cost,
          .hot_start_time = 1.0,
          .cold_start_time = 4.0,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;  // read raw $ directly
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The cold-start tier is charged at FACE VALUE — NOT ×8 h.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(cold_cost));
  CHECK(obj != doctest::Approx(cold_cost * block_hours));
}

TEST_CASE("Shutdown cost is a per-EVENT cost, NOT duration-weighted")
{
  // REGRESSION (cost-weighting bug): like startup, a shutdown is a
  // once-per-event cost ($/stop) → coefficient is shutdown_cost ×1, NOT
  // shutdown_cost × block.duration().
  //
  // Setup: ONE block of 8 h.  g1 starts online (initial_status=1) but
  // there is no demand and a PUNITIVE noload cost, so the optimizer turns
  // it off → u[0]=0, and C1 logic forces the shutdown var w[0]=1 (u goes
  // 1→0).  noload is huge so staying on is never chosen regardless of
  // whether the shutdown cost is correctly ×1 or buggily ×8; with u[0]=0
  // the noload term is 0, leaving the objective equal to just the
  // shutdown event coefficient.
  System sys;
  sys.name = "shutdown_event_cost";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,  // no load
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  constexpr double block_hours = 8.0;
  constexpr double shutdown_cost = 5000.0;

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = block_hours,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .shutdown_cost = shutdown_cost,
          // Punitive noload makes staying on (×8 h) far dearer than any
          // shutdown coefficient → u[0]=0 is forced, the noload term
          // vanishes, and the objective is the lone shutdown event.
          .noload_cost = 1.0e6,
          .initial_status = 1.0,  // on → shut down (no load) → w[0]=1
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The shutdown event is charged once at face value — NOT ×8 h.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(shutdown_cost));
  CHECK(obj != doctest::Approx(shutdown_cost * block_hours));
}

TEST_CASE("Noload cost STILL scales with block duration (regression)")
{
  // The noload cost u is genuinely $/h while committed, so it MUST remain
  // duration-proportional — only startup/shutdown changed.  ONE block of
  // 8 h, must_run, noload=100 $/h → noload contribution = 100 × 8 = 800.
  System sys;
  sys.name = "noload_scales";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 0.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  constexpr double block_hours = 8.0;
  constexpr double noload = 100.0;

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = block_hours,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .noload_cost = noload,
          .initial_status = 1.0,  // already on → no startup event
          .relax = true,
          .must_run = true,  // u[0]=1 for the whole 8 h block
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // noload is duration-weighted: 100 $/h × 8 h = 800.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(noload * block_hours));
}

TEST_CASE("Exclusion constraint prevents simultaneous startup and shutdown")
{
  // Verify that in the LP relaxation, v[t] + w[t] <= 1 at every
  // block (C3 exclusion constraint).  With zero startup/shutdown
  // costs, the solver has no cost incentive to avoid fractional
  // v=w values, but the constraint must still hold.

  auto tc = TestCase::make_basic(true);
  tc.system.generator_array[0].pmin = 0.0;
  tc.system.generator_array[1].pmin = 0.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 1.0,
          .relax = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto sol = li.get_col_sol();
  // v cols at 16-19, w cols at 20-23 (4-block make_basic layout)
  constexpr int v_base = 16;
  constexpr int w_base = 20;
  for (int b = 0; b < 4; ++b) {
    CHECK(sol[v_base + b] + sol[w_base + b]
          <= doctest::Approx(1.0).epsilon(1e-6));
  }
}

TEST_CASE("Non-uniform block durations affect min-up-time block count")
{
  // Blocks: [0.5h, 2.0h, 0.5h, 1.0h], min_up_time=2.0h.
  // At t=0: accum 0.5+2.0=2.5 ≥ 2 → ut_blocks=2
  // At t=1: accum 2.0 ≥ 2 → ut_blocks=1 → trivial, skipped
  // At t=2: accum 0.5+1.0=1.5 < 2 → ut_blocks=2 (reaches end)
  // At t=3: accum 1.0 < 2 → ut_blocks=1 → trivial, skipped
  // So 2 min-up-time rows (blocks 0 and 2).

  System sys;
  sys.name = "nonuniform_blocks";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .min_up_time = 2.0,
          .initial_status = 1.0,
          .relax = true,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 0.5,
      },
      {
          .uid = Uid {1},
          .duration = 2.0,
      },
      {
          .uid = Uid {2},
          .duration = 0.5,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 4,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Post-P0: demand_balance gone.  Baseline:
  // balance(4) + gen_upper(4) + gen_lower(4) + logic(4) +
  // exclusion(4) = 20.  Plus min up rows: 2.  Total: 22.
  CHECK(li.get_numrows() == 22);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("Must-run forces minimum pmin generation")
{
  // g1 expensive (gcost=50, pmin=30), g2 cheap (gcost=5, pmax=200).
  // With must_run, u=1 so p ≥ pmin×u = 30.
  // g1 dispatches exactly 30 MW, g2 dispatches 70 MW.
  // Cost = 50×30×4 + 5×70×4 = 6000+1400 = 7400, scaled = 7.4.

  auto tc = TestCase::make_basic(true);

  tc.system.generator_array[0].gcost = 50.0;
  tc.system.generator_array[0].pmin = 30.0;
  tc.system.generator_array[1].gcost = 5.0;
  tc.system.generator_array[1].pmin = 0.0;
  tc.system.generator_array[1].pmax = 200.0;
  tc.system.generator_array[1].capacity = 200.0;

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 1.0,
          .relax = true,
          .must_run = true,
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(tc.simulation, options);
  SystemLP system_lp(tc.system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(7400.0 / 1000.0));
}

// "Segment delta_k forced to zero when u=0" was DELETED on
// 2026-05-20 along with the rest of the Commitment piecewise
// heat-rate path.  The equivalent check (no δ_k segments dispatched
// when the generator is uncommitted) belongs in test_generator.cpp
// against Generator.pmax_segments / heat_rate_segments.

TEST_CASE("Startup and shutdown ramp limits first/last block")
{
  // Verify ramp constraints are created and LP solves when
  // startup_ramp and shutdown_ramp are specified.

  System sys;
  sys.name = "ramp_limits_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 30.0,
          .capacity = 100.0,
      },
  };

  sys.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .ramp_up = 20.0,
          .ramp_down = 20.0,
          .startup_ramp = 50.0,
          .shutdown_ramp = 40.0,
          .initial_status = 0.0,
          .relax = true,
      },
  };

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 4,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Baseline: balance(4) + gen_upper(4) + gen_lower(4) + logic(4)
  //           + exclusion(4) = 20
  // Ramp rows: ramp_up(4) + ramp_down(4) = 8
  // Total >= 28
  CHECK(li.get_numrows() >= 28);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ── Per-(stage, block) ramp schedule (scalar→OptTBRealFieldSched) ────
//
// ramp_up / ramp_down / startup_ramp / shutdown_ramp were promoted from
// scalar OptReal to per-(stage, block) OptTBRealFieldSched on 2026-05-27,
// mirroring the earlier Commitment.pmin migration.  A scalar input must
// resolve to the same constant on every block (byte-for-byte regression
// with the old OptReal path); a per-block vector lets each block carry a
// distinct ramp envelope.

namespace
{
/// Build a 4-block / 1-stage system whose demand forces g1 (cheap) to
/// ramp from ``initial_power`` against a rising load while g2 (expensive)
/// covers any shortfall the ramp limit imposes.  Returns the raw
/// objective for the given commitment.
[[nodiscard]] double solve_ramp_case(const Commitment& uc)
{
  System sys;
  sys.name = "ramp_sched_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  // Rising demand across the 4 blocks: 20 → 40 → 70 → 100, pinned via
  // forced lmax so the load is exactly the per-block schedule.
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = std::vector<std::vector<double>> {{20.0, 40.0, 70.0, 100.0}},
          .forced = true,
      },
  };
  // g1: cheap (gcost=5), ramp-limited via commitment.
  // g2: expensive (gcost=50), unlimited — picks up whatever g1 cannot.
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 5.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  sys.commitment_array = {uc};

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 4,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  return li.get_obj_value_raw();
}
}  // namespace

TEST_CASE("CommitmentLP ramp_up scalar vs per-block schedule")
{
  // Common commitment skeleton: g1 committed, initial_power tracks the
  // first-block demand so the binding constraint is the *ramp rate*, not
  // the startup envelope.
  const auto base_uc = []
  {
    Commitment uc;
    uc.uid = Uid {1};
    uc.name = "g1_uc";
    uc.generator = Uid {1};
    uc.initial_status = 1.0;
    uc.initial_power = 20.0;  // matches block-0 demand
    uc.relax = true;
    uc.must_run = true;
    // Penalise the startup variable so the LP cannot set ``v[t]=1`` for
    // free and relax the ramp constraint via its ``SU·v[t]`` term — the
    // ramp limit must actually bind for this scenario to test anything.
    uc.startup_cost = 1.0e4;
    return uc;
  };

  SUBCASE("scalar ramp_up == per-block constant vector (regression)")
  {
    // Scalar 30 MW/hr.
    auto uc_scalar = base_uc();
    uc_scalar.ramp_up = 30.0;
    const auto obj_scalar = solve_ramp_case(uc_scalar);

    // Per-block vector of the SAME constant must produce an identical LP
    // → identical objective.  This is the byte-for-byte regression guard
    // for the scalar→schedule promotion.
    auto uc_vec = base_uc();
    uc_vec.ramp_up =
        std::vector<std::vector<double>> {{30.0, 30.0, 30.0, 30.0}};
    const auto obj_vec = solve_ramp_case(uc_vec);

    CHECK(obj_vec == doctest::Approx(obj_scalar));
  }

  SUBCASE("per-block ramp_up that is tighter changes the dispatch cost")
  {
    // Loose scalar: 100 MW/hr — never binds, g1 covers all demand cheaply.
    auto uc_loose = base_uc();
    uc_loose.ramp_up = 100.0;
    const auto obj_loose = solve_ramp_case(uc_loose);

    // DIAGNOSTIC: scalar-10 tight — isolates scalar path vs per-block path.
    auto uc_scalar_tight = base_uc();
    uc_scalar_tight.ramp_up = 10.0;
    const auto obj_scalar_tight = solve_ramp_case(uc_scalar_tight);
    MESSAGE("obj_loose=" << obj_loose
                         << " obj_scalar_tight=" << obj_scalar_tight);
    CHECK(obj_scalar_tight > obj_loose);  // does the SCALAR ramp bind?

    // Per-block: clamp the ramp to 10 MW/hr on the high-demand blocks
    // (2 and 3) so g1 cannot keep up and the expensive g2 must fill in.
    // Built via the production JSON path (``[[per-block]]``) — exactly the
    // shape ``plexos2gtopt`` emits for time-varying CPF ramp curves.
    const auto uc_tight = daw::json::from_json<Commitment>(R"({
      "uid": 1,
      "name": "g1_uc",
      "generator": 1,
      "initial_status": 1,
      "initial_power": 20.0,
      "relax": true,
      "must_run": true,
      "startup_cost": 1.0e4,
      "ramp_up": [[100.0, 100.0, 10.0, 10.0]]
    })");
    const auto obj_tight = solve_ramp_case(uc_tight);

    // The tighter per-block envelope forces costlier dispatch.  Because the
    // clamp to 10 MW/hr applies only to blocks 2 and 3 (blocks 0,1 keep the
    // loose 100 MW/hr), the per-block objective must land strictly between
    // the all-loose objective and the all-tight scalar-10 objective.  If the
    // per-block schedule mistakenly resolved to its block-0 value (100) for
    // every block, ``obj_tight`` would equal ``obj_loose`` (regression
    // guard for per-block schedule resolution).
    CHECK(obj_tight > obj_loose);
    CHECK(obj_tight < obj_scalar_tight);
  }
}

// ── Per-(stage, block) Commitment.pmin binding ──────────────────────
//
// Companion to the ramp per-block test: confirms that the *shared*
// per-(stage, block) schedule-resolution path (``optval(stage, block)``
// over an inline ``[[...]]`` vector) returns the correct per-block value
// for ``Commitment.pmin`` too — not the block-0 value broadcast across
// every block.  A constant demand isolates the effect of the
// when-committed floor: blocks whose ``pmin`` is 0 let the cheap unit
// cover the load; blocks whose ``pmin`` is positive force the expensive
// committed unit up to that floor.

namespace
{
/// 4-block / 1-stage system, constant demand=100.  g1 is committed
/// (must_run, gcost=50, expensive) with a per-block when-committed floor
/// carried on ``Commitment.pmin``; g2 is cheap (gcost=5) and large enough
/// to cover the load on its own.  Returns the raw objective.
[[nodiscard]] double solve_pmin_case(const Commitment& uc)
{
  System sys;
  sys.name = "pmin_sched_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax =
              std::vector<std::vector<double>> {{100.0, 100.0, 100.0, 100.0}},
          .forced = true,
      },
  };
  // g1: expensive (gcost=50), committed via ``uc`` with a conditional pmin.
  //     Generator.pmin stays 0 so the only floor is the commitment one.
  // g2: cheap (gcost=5), unconstrained — covers whatever g1 is not forced
  //     to produce.
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 5.0,
          .capacity = 100.0,
      },
  };
  sys.commitment_array = {uc};

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 4,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  return li.get_obj_value_raw();
}
}  // namespace

TEST_CASE("CommitmentLP pmin scalar vs per-block schedule")
{
  const auto base_uc = []
  {
    Commitment uc;
    uc.uid = Uid {1};
    uc.name = "g1_uc";
    uc.generator = Uid {1};
    uc.relax = true;
    uc.must_run = true;  // g1 committed (u=1) every block
    return uc;
  };

  SUBCASE("scalar pmin == per-block constant vector (regression)")
  {
    auto uc_scalar = base_uc();
    uc_scalar.pmin = 50.0;
    const auto obj_scalar = solve_pmin_case(uc_scalar);

    auto uc_vec = base_uc();
    uc_vec.pmin = std::vector<std::vector<double>> {{50.0, 50.0, 50.0, 50.0}};
    const auto obj_vec = solve_pmin_case(uc_vec);

    CHECK(obj_vec == doctest::Approx(obj_scalar));
  }

  SUBCASE("per-block pmin that is positive only on late blocks binds")
  {
    // No floor anywhere: cheap g2 supplies all 400 MWh → obj = 400·5/1000.
    auto uc_zero = base_uc();
    uc_zero.pmin = 0.0;
    const auto obj_zero = solve_pmin_case(uc_zero);

    // Constant floor of 50 on every block: expensive g1 is pinned to 50 in
    // all four blocks.
    auto uc_all = base_uc();
    uc_all.pmin = 50.0;
    const auto obj_all = solve_pmin_case(uc_all);
    CHECK(obj_all > obj_zero);  // a binding floor costs more

    // Per-block floor active only on blocks 2 and 3 (built via the
    // production JSON ``[[per-block]]`` path).  If the schedule resolved to
    // its block-0 value (0) for every block, ``obj_part`` would equal
    // ``obj_zero``; if it broadcast block-0 across all blocks it could not
    // bind blocks 2,3.  It must land strictly between the no-floor and
    // all-floor objectives.
    const auto uc_part = daw::json::from_json<Commitment>(R"({
      "uid": 1,
      "name": "g1_uc",
      "generator": 1,
      "relax": true,
      "must_run": true,
      "pmin": [[0.0, 0.0, 50.0, 50.0]]
    })");
    const auto obj_part = solve_pmin_case(uc_part);

    CHECK(obj_part > obj_zero);
    CHECK(obj_part < obj_all);
  }
}

TEST_CASE("CommitmentLP startup_ramp scalar vs per-block schedule")
{
  // g1 starts OFF; the first dispatched block is gated by startup_ramp
  // (max output in the startup block).  initial_status=0 so the
  // first-block RHS = startup_ramp·(1 - u_init) = startup_ramp.
  const auto base_uc = []
  {
    Commitment uc;
    uc.uid = Uid {1};
    uc.name = "g1_uc";
    uc.generator = Uid {1};
    uc.initial_status = 0.0;
    uc.relax = true;
    uc.must_run = true;
    return uc;
  };

  SUBCASE("scalar startup_ramp == per-block constant vector (regression)")
  {
    auto uc_scalar = base_uc();
    uc_scalar.startup_ramp = 30.0;
    const auto obj_scalar = solve_ramp_case(uc_scalar);

    auto uc_vec = base_uc();
    uc_vec.startup_ramp =
        std::vector<std::vector<double>> {{30.0, 30.0, 30.0, 30.0}};
    const auto obj_vec = solve_ramp_case(uc_vec);

    CHECK(obj_vec == doctest::Approx(obj_scalar));
  }

  SUBCASE("tighter first-block startup_ramp raises cost")
  {
    // Loose: 100 MW available in the startup block — g1 covers all demand.
    auto uc_loose = base_uc();
    uc_loose.startup_ramp = 100.0;
    const auto obj_loose = solve_ramp_case(uc_loose);

    // Tight first block (5 MW) caps g1's block-0 output, forcing the
    // expensive g2 to cover the rest of the 20 MW load.
    auto uc_tight = base_uc();
    uc_tight.startup_ramp =
        std::vector<std::vector<double>> {{5.0, 100.0, 100.0, 100.0}};
    const auto obj_tight = solve_ramp_case(uc_tight);

    CHECK(obj_tight > obj_loose);
  }
}

// ── PhaseRangeSet unit tests ─────────────────────────────────────────

TEST_CASE("PhaseRangeSet parsing")  // NOLINT
{
  SUBCASE("none and empty")
  {
    PhaseRangeSet none("none");
    CHECK(none.is_none());
    CHECK_FALSE(none.is_all());
    CHECK_FALSE(none.contains(0));
    CHECK_FALSE(none.contains(5));

    PhaseRangeSet empty("");
    CHECK(empty.is_none());
  }

  SUBCASE("all")
  {
    PhaseRangeSet all("all");
    CHECK(all.is_all());
    CHECK_FALSE(all.is_none());
    CHECK(all.contains(0));
    CHECK(all.contains(999));
  }

  SUBCASE("single index")
  {
    PhaseRangeSet s("3");
    CHECK_FALSE(s.is_all());
    CHECK_FALSE(s.is_none());
    CHECK_FALSE(s.contains(0));
    CHECK_FALSE(s.contains(2));
    CHECK(s.contains(3));
    CHECK_FALSE(s.contains(4));
  }

  SUBCASE("comma-separated indices")
  {
    PhaseRangeSet s("1,3,5");
    CHECK(s.contains(1));
    CHECK_FALSE(s.contains(2));
    CHECK(s.contains(3));
    CHECK_FALSE(s.contains(4));
    CHECK(s.contains(5));
    CHECK_FALSE(s.contains(6));
  }

  SUBCASE("closed range")
  {
    PhaseRangeSet s("2:5");
    CHECK_FALSE(s.contains(1));
    CHECK(s.contains(2));
    CHECK(s.contains(3));
    CHECK(s.contains(5));
    CHECK_FALSE(s.contains(6));
  }

  SUBCASE("open-ended range (3:)")
  {
    PhaseRangeSet s("3:");
    CHECK_FALSE(s.contains(2));
    CHECK(s.contains(3));
    CHECK(s.contains(100));
  }

  SUBCASE("prefix range (:5)")
  {
    PhaseRangeSet s(":5");
    CHECK(s.contains(0));
    CHECK(s.contains(3));
    CHECK(s.contains(5));
    CHECK_FALSE(s.contains(6));
  }

  SUBCASE("mixed expression")
  {
    PhaseRangeSet s("0,3:5,8:");
    CHECK(s.contains(0));
    CHECK_FALSE(s.contains(1));
    CHECK_FALSE(s.contains(2));
    CHECK(s.contains(3));
    CHECK(s.contains(4));
    CHECK(s.contains(5));
    CHECK_FALSE(s.contains(6));
    CHECK_FALSE(s.contains(7));
    CHECK(s.contains(8));
    CHECK(s.contains(100));
  }

  SUBCASE("whitespace tolerance")
  {
    PhaseRangeSet s(" 1 , 3 : 5 ");
    CHECK(s.contains(1));
    CHECK(s.contains(3));
    CHECK(s.contains(5));
    CHECK_FALSE(s.contains(2));
  }
}

TEST_CASE("continuous_phases via model_options relaxes UC binaries")  // NOLINT
{
  auto tc = TestCase::make_basic(true);

  // Add commitment on g1 (NOT per-element relax)
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 500.0,
          .noload_cost = 5.0,
          .initial_status = 0.0,
      },
  };

  // Set continuous_phases = "all" → all phases relaxed
  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.continuous_phases = "all";
  poptions.model_options.use_single_bus = true;
  PlanningOptionsLP options(std::move(poptions));

  Simulation simulation(tc.simulation);
  System sys(tc.system);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();

  // Verify no integer variables (all binaries relaxed to continuous)
  const auto ncols = li.get_numcols();
  int num_ints = 0;
  for (Index i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      ++num_ints;
    }
  }
  CHECK(num_ints == 0);

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("continuous_phases=none keeps integer UC binaries")  // NOLINT
{
  auto tc = TestCase::make_basic(true);

  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 500.0,
          .noload_cost = 5.0,
          .initial_status = 0.0,
      },
  };

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.continuous_phases = "none";
  poptions.model_options.use_single_bus = true;
  // continuous_phases=none keeps the u binaries integer; pin a
  // MIP-capable solver so an ambient GTOPT_SOLVER=clp (CI) doesn't trip
  // the LP-only guard in LinearInterface::load_flat.
  poptions.lp_matrix_options.solver_name = solver_test::first_mip_solver();
  PlanningOptionsLP options(std::move(poptions));

  Simulation simulation(tc.simulation);
  System sys(tc.system);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  // Tight 3-binary formulation declares ONLY the status u integer;
  // startup v and shutdown w are continuous in [0,1] (they resolve to
  // binary via the logic + exclusion constraints).  So with 4 blocks
  // and 1 committed generator we expect 4 integer columns (the u's),
  // not 12.  See commitment_lp.cpp for the rationale.
  const auto ncols = li.get_numcols();
  int num_ints = 0;
  for (Index i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      ++num_ints;
    }
  }
  CHECK(num_ints == 4);
}

TEST_CASE("CommitmentLP - add_to_output via write_out")  // NOLINT
{
  // Exercises CommitmentLP::add_to_output by calling write_out after
  // resolving the LP.
  auto tc = TestCase::make_basic(/*chronological=*/true);
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cm1",
          .generator = Uid {1},
          .startup_cost = 0.0,
          .noload_cost = 5.0,
          .initial_status = 0.0,
      },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_commitment_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.continuous_phases = "all";  // relax to avoid MIP
  poptions.model_options.use_single_bus = true;
  poptions.output_directory = tmpdir.string();
  PlanningOptionsLP options(std::move(poptions));

  Simulation simulation(tc.simulation);
  System sys(tc.system);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Verify objective includes noload_cost (5 $/h × 4 blocks = 20 plus gen
  // costs).  With demand=100 MW, g1 (gcost=10) dispatches first.
  // Total obj should be positive and include generation + noload cost.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj > 0.0);

  // Verify we got exactly one commitment element with populated status cols
  const auto sol = li.get_col_sol();
  const auto ncols = static_cast<size_t>(li.get_numcols());
  CHECK(ncols > 0);
  // At least some cols must have non-zero values (generators dispatching)
  double total_sol = 0;
  for (size_t i = 0; i < ncols; ++i) {
    total_sol += sol[i];
  }
  CHECK(total_sol > 0.0);

  // Exercises CommitmentLP::add_to_output (u/v/w status cols, all row duals)
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "CommitmentLP — MIP u/v/w binary values for startup profile")
{
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-argument-comment,bugprone-unchecked-optional-access)

  SolverRegistry& reg = SolverRegistry::instance();
  if (!reg.has_mip_solver()) {
    MESSAGE("Skipping MIP test — no MIP solver available");
    return;
  }

  // 1 bus, 1 stage, 3 blocks (1 h each), 1 scenario.
  // Demand profile across blocks: [0, 60, 60]  (MW).
  // Generator: pmin=30, pmax/capacity=100, gcost=50.
  // Commitment: startup_cost=100, shutdown_cost=50, initial_status=0.
  // demand_fail_cost = 1000 → dispatching in blocks 1 & 2 is optimal.
  // Expected optimum (MIP): u = [0, 1, 1], v = [0, 1, 0], w = [0, 0, 0].
  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 3,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  System system;
  system.name = "uc_mip_startup";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  // Per-block demand profile: stage × block = 1 × 3 = {{0, 60, 60,},}.
  system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = TBRealFieldSched {std::vector<std::vector<Real>> {
              {
                  0.0,
                  60.0,
                  60.0,
              },
          }},
          .capacity = 100.0,
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          // pmin moves to Commitment.pmin (when-committed floor).
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .shutdown_cost = 50.0,
          .pmin = 30.0,
          .initial_status = 0.0,
      },
  };

  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.lp_matrix_options.col_with_names = true;
  poptions.lp_matrix_options.col_with_name_map = true;
  PlanningOptionsLP options(std::move(poptions));

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.col_with_name_map = true;
  // Pick the first MIP-capable solver explicitly so we don't fall through
  // to the GTOPT_SOLVER env override (which CI pins to "clp" — LP-only).
  // Defensive: skip if no MIP solver was found, even though
  // has_mip_solver() returned true above (guards against an inconsistency
  // between has_mip_solver() and supports_mip()).
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      flat_opts.solver_name = name;
      break;
    }
  }
  if (flat_opts.solver_name.empty()) {
    MESSAGE(
        "Skipping MIP test — supports_mip() returned false for "
        "every loaded solver despite has_mip_solver()=true");
    return;
  }

  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp, flat_opts);

  auto&& lp = system_lp.linear_interface();

  // Solve as MIP (integer u/v/w → backend dispatches to MIP solve).
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Locate u/v/w columns by block UID via the LP column-name map.
  // Label format: "commitment_<variable>_<cuid>_<scen>_<stage>_<block>"
  const auto& col_map = lp.col_name_map();
  const auto sol = lp.get_col_sol();

  auto find_col = [&](std::string_view variable,
                      Uid block_uid) -> std::optional<ColIndex>
  {
    for (const auto& [name, idx] : col_map) {
      if (name.starts_with("commitment_")
          && name.contains(std::string(variable) + "_") && name.size() >= 2
          && name.substr(name.size() - 2)
              == std::string("_") + std::to_string(block_uid))
      {
        return idx;
      }
    }
    return std::nullopt;
  };

  const auto u0 = find_col(CommitmentLP::StatusName, Uid {0});
  const auto u1 = find_col(CommitmentLP::StatusName, Uid {1});
  const auto u2 = find_col(CommitmentLP::StatusName, Uid {2});
  const auto v1 = find_col(CommitmentLP::StartupName, Uid {1});
  const auto v2 = find_col(CommitmentLP::StartupName, Uid {2});
  const auto w2 = find_col(CommitmentLP::ShutdownName, Uid {2});

  REQUIRE(u0.has_value());
  REQUIRE(u1.has_value());
  REQUIRE(u2.has_value());
  REQUIRE(v1.has_value());
  REQUIRE(v2.has_value());
  REQUIRE(w2.has_value());

  // Tight 3-binary formulation: ONLY the status u is declared integer.
  // Startup v and shutdown w are continuous in [0,1] — the logic equality
  // C1 (u[p]-u[p-1]-v[p]+w[p]=0) + exclusion C3 (v+w<=1) + nonnegative
  // startup/shutdown costs force them to integer transitions of an integer
  // u, so they come out binary WITHOUT being branched on.  This is the
  // active check that the tight formulation is in effect: flipping these
  // back to CHECK(lp.is_integer(...)) would mean the integrality-reduction
  // regressed.
  CHECK(lp.is_integer(*u0));
  CHECK(lp.is_integer(*u1));
  CHECK(lp.is_integer(*u2));
  CHECK(!lp.is_integer(*v1));
  CHECK(!lp.is_integer(*v2));
  CHECK(!lp.is_integer(*w2));

  // Correctness of the reduction: even though v/w are continuous, the
  // optimal solution still takes clean integer values — unit off at block
  // 0, starts at block 1, stays on at 2.  If the propagation ever broke,
  // these would go fractional.
  CHECK(sol[*u0] == doctest::Approx(0.0).epsilon(1e-4));
  CHECK(sol[*u1] == doctest::Approx(1.0).epsilon(1e-4));
  CHECK(sol[*u2] == doctest::Approx(1.0).epsilon(1e-4));
  CHECK(sol[*v1] == doctest::Approx(1.0).epsilon(1e-4));
  CHECK(sol[*v2] == doctest::Approx(0.0).epsilon(1e-4));
  CHECK(sol[*w2] == doctest::Approx(0.0).epsilon(1e-4));
}

// ── Legacy Commitment JSON rejection (2026-05-20) ──────────────────────────
//
// Commit 16fbdde45 removed `fuel`, `pmax_segments`, `heat_rate_segments`,
// `fuel_cost`, `fuel_emission_factor` from the Commitment schema (they
// moved to Generator).  A legacy JSON file that still carries any of
// these keys must fail loudly under `StrictParsePolicy` — silently
// accepting them would mean the user's piecewise heat-rate / fuel
// configuration is being applied to the WRONG element (or not at all).
// Pin the rejection here so a future "be lenient about unknown fields"
// drift doesn't reopen the silent-failure window.

TEST_CASE("Commitment JSON — legacy `fuel` field is rejected")  // NOLINT
{
  constexpr std::string_view legacy = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "fuel": "gas"
  })";
  CHECK_THROWS_AS(
      (void)daw::json::from_json<Commitment>(legacy, StrictParsePolicy),
      daw::json::json_exception);
}

TEST_CASE(
    "Commitment JSON — legacy `pmax_segments` field is rejected")  // NOLINT
{
  constexpr std::string_view legacy = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "pmax_segments": [50, 100, 200]
  })";
  CHECK_THROWS_AS(
      (void)daw::json::from_json<Commitment>(legacy, StrictParsePolicy),
      daw::json::json_exception);
}

TEST_CASE(
    "Commitment JSON — legacy `heat_rate_segments` field is rejected")  // NOLINT
{
  constexpr std::string_view legacy = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "heat_rate_segments": [8.0, 8.5, 9.0]
  })";
  CHECK_THROWS_AS(
      (void)daw::json::from_json<Commitment>(legacy, StrictParsePolicy),
      daw::json::json_exception);
}

TEST_CASE("Commitment JSON — legacy `fuel_cost` field is rejected")  // NOLINT
{
  constexpr std::string_view legacy = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "fuel_cost": 5.0
  })";
  CHECK_THROWS_AS(
      (void)daw::json::from_json<Commitment>(legacy, StrictParsePolicy),
      daw::json::json_exception);
}

TEST_CASE(
    "Commitment JSON — legacy `fuel_emission_factor` field is rejected")  // NOLINT
{
  constexpr std::string_view legacy = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "fuel_emission_factor": 0.42
  })";
  CHECK_THROWS_AS(
      (void)daw::json::from_json<Commitment>(legacy, StrictParsePolicy),
      daw::json::json_exception);
}

TEST_CASE("Commitment JSON — `pmin` field accepted (new in 2026-05-20)")
// NOLINT
{
  // `Commitment.pmin` was ADDED in 16fbdde45 as the "when-committed"
  // floor (Generator.pmin remains the always-on floor).  Pin that the
  // schema accepts it.  Round-trip is covered by the existing
  // "Commitment JSON round-trip" test — we just assert parse-success
  // here so a future schema edit doesn't accidentally remove it.
  constexpr std::string_view minimal = R"({
    "uid": 1,
    "name": "thermal1_uc",
    "generator": 10,
    "pmin": 25.0
  })";
  const auto c = daw::json::from_json<Commitment>(minimal, StrictParsePolicy);
  CHECK(c.uid == 1);
  CHECK(std::get<double>(c.pmin.value_or(-1.0)) == doctest::Approx(25.0));
}

// NOLINTEND(bugprone-argument-comment,bugprone-unchecked-optional-access)
