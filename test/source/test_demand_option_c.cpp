// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for `model_options.demand_fail_rhs_shift` (renamed from
// the legacy `demand_option_c` per §11.10) — the experimental
// demand-fail substitution that encodes the LP column as
// `neg_fail = load − lmax` instead of `load`.  See
// `source/demand_lp.cpp` and the matching commit message for the
// algebraic invariants exercised here.
//
// Test plan:
//   1. **Option C invariants** in isolation: with the flag ON,
//      `obj_constant` stays 0, `get_obj_value() ==
//      get_obj_value_raw() × scale_objective`, and the bus-balance
//      RHS carries the expected `+(1+loss)·lmax` shift.
//
//   2. **Option A ↔ Option C numerical equivalence**: the same
//      fixture solved under both flag values must agree on
//      `get_obj_value()` (the displayed physical objective) to
//      within FP tolerance.  Verified for the supply-sufficient and
//      supply-insufficient regimes.
//
//   3. **AMPL resolver semantics**: a user constraint
//      `demand('d').load <= X` enforces the SAME physical bound
//      under both flag values, even though the LP column means
//      different things internally.
//
// Battery/Converter interactions are NOT covered because Option C
// is not yet safe for those (the converter row references the
// demand col as `load`, which under Option C is `neg_fail` — see
// the deferred-follow-up note in the Commit 2 message).

#include <doctest/doctest.h>
#include <gtopt/demand.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

[[nodiscard]] PlanningOptions make_options(bool option_c)
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.scale_objective = 1.0;
  if (option_c) {
    popts.model_options.demand_fail_rhs_shift = true;
  }
  return popts;
}

[[nodiscard]] Simulation make_single_block_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
}

[[nodiscard]] System make_demand_gen_system(double gen_capacity)
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = gen_capacity,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 100.0,
      },
  };
  return System {
      .name = "DemandOptionCFixture",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };
}

}  // namespace

// ── Invariant 1 — Option C alone ─────────────────────────────────────

TEST_CASE("Option C — obj_constant stays 0; raw obj == physical obj")
{
  const auto simulation = make_single_block_simulation();
  const auto system = make_demand_gen_system(/*gen_capacity=*/300.0);

  const PlanningOptionsLP options(make_options(/*option_c=*/true));
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Algebraic physical obj: gcost × gen × duration = 50 × 100 × 1 = 5000.
  CHECK(lp.get_obj_value() == doctest::Approx(5000.0).epsilon(1e-9));
  // Option C invariant: zero obj_constant, raw obj == physical.
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
  CHECK(lp.get_obj_value()
        == doctest::Approx(lp.get_obj_value_raw() * lp.scale_objective())
               .epsilon(1e-9));
}

TEST_CASE("Option C — supply-insufficient: fail penalty in raw obj")
{
  const auto simulation = make_single_block_simulation();
  // Generator caps at 50 MW; 50 MW of demand is unserved.
  const auto system = make_demand_gen_system(/*gen_capacity=*/50.0);

  const PlanningOptionsLP options(make_options(/*option_c=*/true));
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Algebraic: gen_cost (50 × 50) + fail_cost × fail (1000 × 50) = 52500.
  // Option C: neg_fail = load − lmax = 50 − 100 = −50.
  //   solver_raw = −1000 × (−50) + 50 × 50 = 52500
  //   obj_constant = 0  ⇒  get_obj_value() = 52500.
  CHECK(lp.get_obj_value() == doctest::Approx(52500.0).epsilon(1e-9));
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
}

// ── Invariant 2 — Option A ↔ Option C numerical equivalence ──────────

TEST_CASE("Option A ↔ Option C: get_obj_value() matches in both regimes")
{
  const auto simulation = make_single_block_simulation();

  SUBCASE("supply-sufficient (load = lmax, fail = 0)")
  {
    const auto system = make_demand_gen_system(/*gen_capacity=*/300.0);

    double obj_a {};
    double obj_c {};
    {
      const PlanningOptionsLP options(make_options(/*option_c=*/false));
      SimulationLP simulation_lp(simulation, options);
      SystemLP system_lp(system, simulation_lp);
      auto&& lp = system_lp.linear_interface();
      const auto result = lp.resolve();
      REQUIRE(result.has_value());
      obj_a = lp.get_obj_value();
      // Option A invariant: obj_constant accumulates +c·lmax = 100000.
      CHECK(lp.get_obj_constant() == doctest::Approx(100000.0));
    }
    {
      const PlanningOptionsLP options(make_options(/*option_c=*/true));
      SimulationLP simulation_lp(simulation, options);
      SystemLP system_lp(system, simulation_lp);
      auto&& lp = system_lp.linear_interface();
      const auto result = lp.resolve();
      REQUIRE(result.has_value());
      obj_c = lp.get_obj_value();
      CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
    }
    // Both modes produce identical physical objectives.
    CHECK(obj_a == doctest::Approx(obj_c).epsilon(1e-9));
    CHECK(obj_a == doctest::Approx(5000.0));
  }

  SUBCASE("supply-insufficient (load < lmax, fail > 0)")
  {
    const auto system = make_demand_gen_system(/*gen_capacity=*/50.0);

    double obj_a {};
    double obj_c {};
    {
      const PlanningOptionsLP options(make_options(/*option_c=*/false));
      SimulationLP simulation_lp(simulation, options);
      SystemLP system_lp(system, simulation_lp);
      auto&& lp = system_lp.linear_interface();
      const auto result = lp.resolve();
      REQUIRE(result.has_value());
      obj_a = lp.get_obj_value();
      CHECK(lp.get_obj_constant() == doctest::Approx(100000.0));
    }
    {
      const PlanningOptionsLP options(make_options(/*option_c=*/true));
      SimulationLP simulation_lp(simulation, options);
      SystemLP system_lp(system, simulation_lp);
      auto&& lp = system_lp.linear_interface();
      const auto result = lp.resolve();
      REQUIRE(result.has_value());
      obj_c = lp.get_obj_value();
      CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
    }
    CHECK(obj_a == doctest::Approx(obj_c).epsilon(1e-9));
    CHECK(obj_a == doctest::Approx(52500.0));
  }
}

// ── Invariant 3 — AMPL resolver under Option C ───────────────────────

TEST_CASE(
    "Option C — user constraint `demand.load <= X` enforces physical bound")
{
  // Scenario: lmax = 100, gcost = 50, demand_fail_cost = 1000.
  // Without any user constraint, the LP fully serves (load = 100,
  // obj = 5000) because serving is cheaper than failing.
  //
  // Adding `demand('d1').load <= 60` should cap load at 60, leaving
  // 40 MW unserved.  Algebraic obj:
  //   gen_cost = 50 × 60 = 3000
  //   fail_cost = 1000 × 40 = 40000
  //   total = 43000.
  //
  // The AMPL resolver must produce a row that pins load (the
  // physical quantity) at ≤ 60, regardless of whether the LP
  // column carries `load` (Option A) or `neg_fail = load − lmax`
  // (Option C with offset = +lmax).
  const auto simulation = make_single_block_simulation();
  auto system = make_demand_gen_system(/*gen_capacity=*/200.0);
  system.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "cap_load",
          .expression = "demand('d1').load <= 60",
      },
  };

  SUBCASE("Option A (flag off)")
  {
    const PlanningOptionsLP options(make_options(/*option_c=*/false));
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(lp.get_obj_value() == doctest::Approx(43000.0).epsilon(1e-6));
  }

  SUBCASE("Option C (flag on) — must produce SAME physical bound")
  {
    const PlanningOptionsLP options(make_options(/*option_c=*/true));
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    // Same algebraic obj — proves the resolver folded the
    // `+lmax` offset onto the row's RHS correctly.
    CHECK(lp.get_obj_value() == doctest::Approx(43000.0).epsilon(1e-6));
    // Option C still keeps obj_constant at 0 even with the
    // user-constraint row added.
    CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
  }
}
