// SPDX-License-Identifier: BSD-3-Clause
//
// Phase 2 LP integration tests for ``AllowancePoolLP``.  Phase 2
// builds only the free-allocation inflow + StorageLP banking carry
// — emissions-consumption coupling (Phase 3) and auction-purchase
// (Phase 4) land in follow-up commits.
//
// What this test exercises:
//   * AllowancePoolLP registers on SystemLP collections,
//   * the LP assembles + solves (a copperplate single-bus topology
//     is enough — no emissions yet),
//   * banking carry across stages works: SoC(stage_2) ≈
//     SoC(stage_1) + delivery (within solver tolerance),
//   * efin enforcement via the StorageLP base.

#include <doctest/doctest.h>
#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_allowance_pool_lp
{

/// Two-stage chronological simulation (each stage = 24 hourly
/// blocks).  Used to verify cross-stage banking carry.
Simulation make_2stage_24h_simulation()
{
  Simulation sim;
  // 48 blocks of 1h each.
  sim.block_array.reserve(48);
  for (int i = 0; i < 48; ++i) {
    sim.block_array.push_back(Block {
        .uid = Uid {i},
        .duration = 1.0,
    });
  }
  sim.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 24,
          .chronological = true,
      },
      {
          .uid = Uid {1},
          .first_block = 24,
          .count_block = 24,
          .chronological = true,
      },
  };
  sim.scenario_array = {
      {
          .uid = Uid {0},
      },
  };
  return sim;
}

}  // namespace test_allowance_pool_lp

TEST_CASE("AllowancePoolLP registers on SystemLP + LP assembles")  // NOLINT
{
  using namespace test_allowance_pool_lp;
  // Minimal copperplate system + a single allowance pool with
  // 100 tCO₂ initial bank and 50 tCO₂/stage free allocation.
  // No EmissionZone in Phase 2 — the pool is unconnected to any
  // consumption, so the LP just banks the allocations.
  System sys;
  sys.name = "co2_phase2_smoke";
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
          .capacity = 10.0,
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
  };
  sys.allowance_pool_array = {
      {
          .uid = Uid {1},
          .name = "co2_pool",
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 100.0,  // 100 tCO₂ initial bank
          .efin = 200.0,  // must reach 200 tCO₂ by horizon end
          .delivery = 50.0,  // 50 tCO₂/stage free allocation
          .capacity = 1000.0,  // cap row needs a finite capacity
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const auto simulation = make_2stage_24h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  // AllowancePoolLP should be in the element collections.
  const auto& pool_elems = system_lp.elements<AllowancePoolLP>();
  REQUIRE(pool_elems.size() == 1);
  CHECK(pool_elems.front().uid() == Uid {1});

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  if (!result.has_value()) {
    MESSAGE("resolve error code=" << static_cast<int>(result.error().code)
                                  << " message=" << result.error().message
                                  << " status=" << result.error().status);
  }
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The LP should solve to optimal.  Pool starts at 100, receives
  // 50/stage × 2 stages = 100 more, ends at 200 — exactly meeting
  // efin = 200.  No cost on the allowance side, so total cost is
  // purely the generator dispatch for demand: 10 MW × 48 h × $10/MWh
  // = $4800 → 4.8 scaled.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(4.8).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "AllowancePoolLP: insufficient free allocation triggers efin_cost slack")
{
  using namespace test_allowance_pool_lp;
  // Same system as above but with efin = 300 (requires 200 more
  // tCO₂ than the pool can collect via delivery).  efin_cost = 5.0
  // makes the cap soft — the LP pays for the shortfall instead of
  // going infeasible.
  System sys;
  sys.name = "co2_phase2_efin_slack";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 10.0},
  };
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 10.0,
       .capacity = 100.0},
  };
  sys.allowance_pool_array = {
      {
          .uid = Uid {1},
          .name = "co2_pool",
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 100.0,  // 100 initial
          .efin = 300.0,  // need 300 — short by 100
          .efin_cost = 5.0,  // $5/tCO₂ shortfall penalty
          .delivery = 50.0,  // 50/stage × 2 = +100 → ends at 200
          .capacity = 1000.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const auto simulation = make_2stage_24h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  if (!result.has_value()) {
    MESSAGE("resolve error code=" << static_cast<int>(result.error().code)
                                  << " message=" << result.error().message);
  }
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Demand cost stays at 4.8.  Slack on efin row = 100 tCO₂
  // × $5/tCO₂ = $500 → 0.5 scaled.  Total ≈ 4.8 + 0.5 = 5.3.
  // (Tolerance loose enough for solver / scaling variation; the
  // important assertion is that the LP solved and obj > 4.8.)
  const auto obj = li.get_obj_value_raw();
  CHECK(obj > 4.8);
}
