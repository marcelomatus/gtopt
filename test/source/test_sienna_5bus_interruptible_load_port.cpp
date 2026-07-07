// SPDX-License-Identifier: BSD-3-Clause
//
// Port of NREL-Sienna PowerSimulations.jl ``c_sys5_il``
// (Interruptible Load) to a gtopt integration test.
//
// ## Source
//
// PSY's ``InterruptibleLoad`` is a price-sensitive demand: instead
// of being a forced load, it carries a per-MWh willingness-to-pay
// (``proportional_term``) and the LP curtails (sheds) it whenever
// the marginal serving cost exceeds that value.
//
// gtopt's native mechanism is ``Demand.fcost`` (see
// ``demand.hpp`` line 67) — when set (>0), the per-MWh curtailment
// cost replaces the global ``demand_fail_cost`` for THIS load.  An
// LP routinely curtails the load when the marginal serving cost
// exceeds fcost, and serves it fully otherwise.
//
// This integration test exercises:
//
//   * fcost ABOVE marginal gen cost ⇒ load fully served.
//   * fcost BELOW marginal gen cost ⇒ load curtailed.
//   * Multiple demand rows: the mandatory load uses the global
//     demand_fail_cost (high), the interruptible load uses fcost
//     (low) — both demand rows coexist on the same bus.

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_sienna_5bus_interruptible_load_port
{
namespace
{

constexpr Uid kBusUid {1};

constexpr Real kMandatoryLoad = 30.0;
constexpr Real kIlLoad = 50.0;
// Cheap generator covers the mandatory load + part of the IL.
constexpr Real kCheapGcost = 10.0;
constexpr Real kCheapCap = 40.0;
constexpr Real kExpensiveGcost = 100.0;
constexpr Real kExpensiveCap = 200.0;
constexpr Real kDemandFailCost = 1.0e6;

[[nodiscard]] System make_system(Real il_fcost)
{
  System sys;
  sys.name = "SiennaC5Il";
  sys.bus_array = {{.uid = kBusUid, .name = "b1"}};
  sys.demand_array = {
      // Mandatory load — high fcost (the global demand_fail_cost
      // applies; we leave fcost unset for the mandatory load).
      {.uid = Uid {1},
       .name = "mandatory",
       .bus = kBusUid,
       .capacity = kMandatoryLoad},
      // Interruptible load — per-row fcost, the LP curtails it
      // whenever the marginal serving cost exceeds il_fcost.
      {.uid = Uid {2},
       .name = "il_load",
       .type = "interruptible",
       .bus = kBusUid,
       .fcost = il_fcost,
       .capacity = kIlLoad},
  };
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "cheap",
       .bus = kBusUid,
       .gcost = kCheapGcost,
       .capacity = kCheapCap},
      {.uid = Uid {2},
       .name = "expensive",
       .bus = kBusUid,
       .gcost = kExpensiveGcost,
       .capacity = kExpensiveCap},
  };
  return sys;
}

[[nodiscard]] Simulation make_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}, .probability_factor = 1.0}},
  };
}

[[nodiscard]] PlanningOptions make_options()
{
  PlanningOptions popts;
  popts.model_options.use_single_bus = true;
  popts.model_options.use_kirchhoff = false;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = kDemandFailCost;
  return popts;
}

}  // namespace

TEST_CASE(
    "Sienna c_sys5_il port — fcost > expensive cost: IL fully served")  // NOLINT
{
  // With il_fcost = 150 > expensive gcost (100), the LP will serve
  // the IL at $100/MWh and pay $150/MWh penalty only if it's
  // CHEAPER than dispatching — it never is, so the IL is fully
  // served.
  //   Total demand = 30 (mandatory) + 50 (IL) = 80 MW
  //   Cheap serves 40 (gcost $10 → $400), expensive serves 40 ($100
  //   → $4000), total $4400.
  const auto sys = make_system(/*il_fcost=*/150.0);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  constexpr Real kExpectedObj = kCheapGcost * kCheapCap
      + kExpensiveGcost * (kMandatoryLoad + kIlLoad - kCheapCap);
  CHECK(lp.get_obj_value_raw() == doctest::Approx(kExpectedObj).epsilon(1e-4));
}

TEST_CASE(
    "Sienna c_sys5_il port — fcost < expensive cost: IL curtailed")  // NOLINT
{
  // With il_fcost = 25 (below expensive's 100), the LP will:
  //   * serve mandatory (30 MW) at $10 via cheap (always required —
  //     demand_fail_cost = 1e6),
  //   * serve only 10 MW of IL via cheap (cheap is capped at 40),
  //   * leave the remaining 40 MW of IL CURTAILED at $25/MWh
  //     (cheaper than dispatching expensive at $100).
  //
  // Total cost:
  //   cheap @ $10 × 40 MW = $400
  //   IL curtailment penalty @ $25 × 40 MW = $1000
  //   total = $1400
  const auto sys = make_system(/*il_fcost=*/25.0);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  constexpr Real kIlFcost = 25.0;
  constexpr Real kCheapServed = kCheapCap;  // cheap fully dispatched
  constexpr Real kIlCurtailed = kMandatoryLoad + kIlLoad - kCheapServed;
  constexpr Real kExpectedObj =
      kCheapGcost * kCheapServed + kIlFcost * kIlCurtailed;
  CHECK(lp.get_obj_value_raw() == doctest::Approx(kExpectedObj).epsilon(1e-4));
}

TEST_CASE(
    "Sienna c_sys5_il port — mandatory and IL coexist as separate Demands")  // NOLINT
{
  const auto sys = make_system(/*il_fcost=*/25.0);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  // Pin element counts: 2 distinct DemandLP entries (mandatory + IL).
  CHECK(system_lp.elements<DemandLP>().size() == 2);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

}  // namespace test_sienna_5bus_interruptible_load_port
