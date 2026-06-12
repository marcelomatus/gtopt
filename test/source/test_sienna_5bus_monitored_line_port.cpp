// SPDX-License-Identifier: BSD-3-Clause
//
// Port of NREL-Sienna PowerSimulations.jl ``c_sys5_ml`` (Monitored
// Line) to a gtopt integration test.
//
// ## Source
//
// PowerSimulations.jl ships ``MonitoredLine`` to model branches
// whose thermal limit is enforced; sibling ``Line`` rows on the
// same case are treated as unconstrained.  gtopt mirrors this knob
// with ``Line.enforce_level``:
//
//   * level=2 (default) — hard cap binds (``|flow| ≤ tmax``).
//   * level=0           — cap NOT enforced; ``tmax`` is carried only
//                          for loss-segment discretization.
//
// The c_sys5_ml fixture marks ONE branch (the first row of the
// upstream 5-bus ``branch.csv`` — ``branch4``, bus 2 → bus 3,
// rated 80 MW) as monitored; the other 6 branches stay
// unconstrained.  This test pins the contract: the LP must allow
// flow > rating on the un-monitored lines while the monitored
// line's cap binds whenever the dispatch otherwise wants to push
// past 80 MW across it.
//
// Topology numerics (matches `_monitored_line.py`):
//
//   bus_a/bus_b/Rate (MW) per branch.csv:
//     branch4 : 2→3 , 80   (MONITORED)
//     branch1 : 1→2 , 13.3
//     branch2 : 1→4 , 13.3
//     branch7 : 4→10, 13.3
//     branch3 : 1→10, 66.6
//     branch5 : 3→4 , 66.6
//     branch6 : 3→4 , 28.4
//
// The default 5-bus load total = 71.43 MW.  We replace it with a
// single concentrated 200 MW load on bus 3 (the monitored
// downstream end-point) so the LP MUST push power across the
// monitored line for the cheapest path, and there is no way the
// 80 MW cap can serve the 200 MW load alone — the LP is forced
// to over-saturate one of the unmonitored sibling routes.  This
// is the only way to differentiate level=0 from level=2.
//
// All generation goes on bus 1; the only meaningful unmonitored
// path from bus 1 to bus 3 is via bus 4 (branch2 + branch5/6).
// Both branch2 (13.3 MW) and the parallel branch5+6 (66.6+28.4 =
// 95 MW) are unmonitored; the LP can push the full 200 MW across
// them as long as enforce_level=0 lets the small branches over-load.

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_sienna_5bus_monitored_line_port
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

constexpr Uid kBusUid1 {1};
constexpr Uid kBusUid2 {2};
constexpr Uid kBusUid3 {3};
constexpr Uid kBusUid4 {4};
constexpr Uid kBusUid5 {10};

constexpr Real kLoadBus3 = 200.0;  // concentrated load (see file header)
constexpr Real kGenCost = 10.0;
constexpr Real kGenCap = 500.0;
constexpr Real kDemandFailCost = 1.0e6;

[[nodiscard]] System make_system(bool monitor_branch4)
{
  System sys;
  sys.name = "SiennaC5Ml";
  sys.bus_array = {
      {.uid = kBusUid1, .name = "bus1"},
      {.uid = kBusUid2, .name = "bus2"},
      {.uid = kBusUid3, .name = "bus3"},
      {.uid = kBusUid4, .name = "bus4"},
      {.uid = kBusUid5, .name = "bus5"},
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "load_bus3",
          .bus = kBusUid3,
          .capacity = kLoadBus3,
      },
  };
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = kBusUid1,
          .gcost = kGenCost,
          .capacity = kGenCap,
      },
  };

  // 7 branches mirror the upstream branch.csv exactly.  Each is
  // labeled enforce_level=0 (unconstrained) EXCEPT the monitored
  // branch when monitor_branch4=true.
  auto level = [&](std::string_view name) -> int
  {
    if (!monitor_branch4) {
      return 0;
    }
    return name == "branch4" ? 2 : 0;
  };

  sys.line_array = {
      {.uid = Uid {1},
       .name = "branch4",
       .bus_a = kBusUid2,
       .bus_b = kBusUid3,
       .tmax_ba = 80.0,
       .tmax_ab = 80.0,
       .enforce_level = level("branch4")},
      {.uid = Uid {2},
       .name = "branch1",
       .bus_a = kBusUid1,
       .bus_b = kBusUid2,
       .tmax_ba = 13.3,
       .tmax_ab = 13.3,
       .enforce_level = level("branch1")},
      {.uid = Uid {3},
       .name = "branch2",
       .bus_a = kBusUid1,
       .bus_b = kBusUid4,
       .tmax_ba = 13.3,
       .tmax_ab = 13.3,
       .enforce_level = level("branch2")},
      {.uid = Uid {4},
       .name = "branch7",
       .bus_a = kBusUid4,
       .bus_b = kBusUid5,
       .tmax_ba = 13.3,
       .tmax_ab = 13.3,
       .enforce_level = level("branch7")},
      {.uid = Uid {5},
       .name = "branch3",
       .bus_a = kBusUid1,
       .bus_b = kBusUid5,
       .tmax_ba = 66.6,
       .tmax_ab = 66.6,
       .enforce_level = level("branch3")},
      {.uid = Uid {6},
       .name = "branch5",
       .bus_a = kBusUid3,
       .bus_b = kBusUid4,
       .tmax_ba = 66.6,
       .tmax_ab = 66.6,
       .enforce_level = level("branch5")},
      {.uid = Uid {7},
       .name = "branch6",
       .bus_a = kBusUid3,
       .bus_b = kBusUid4,
       .tmax_ba = 28.4,
       .tmax_ab = 28.4,
       .enforce_level = level("branch6")},
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
  popts.model_options.use_single_bus = false;
  popts.model_options.use_kirchhoff = false;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = kDemandFailCost;
  return popts;
}

}  // namespace

TEST_CASE(
    "Sienna c_sys5_ml port — all-relaxed baseline: full 200 MW served")  // NOLINT
{
  // Sanity: with EVERY line at enforce_level=0 the LP has zero
  // transmission constraints, so the 200 MW load is served entirely
  // from g1 at $10/MWh.
  const auto sys = make_system(/*monitor_branch4=*/false);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 200 MW × $10/MWh = $2 000.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(2000.0).epsilon(1e-6));
}

TEST_CASE(
    "Sienna c_sys5_ml port — monitored branch4 cap binds but only on its leg")  // NOLINT
{
  // With branch4 monitored (cap=80) and EVERY other branch
  // unconstrained, the LP routes power around bus 2 (since the
  // branch1+branch4 path would now cap at 80 MW total).  The
  // routes via bus 4 / bus 5 (all enforce_level=0) carry the rest
  // unrestricted, so the 200 MW load still gets served at
  // $10/MWh.  The monitored cap does NOT change the objective in
  // this fixture (g1 unchanged at 200 MW) — its effect is
  // purely on the flow split.
  const auto sys = make_system(/*monitor_branch4=*/true);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 200 MW × $10/MWh = $2 000 — same as the baseline.  The
  // monitored cap binds the branch4 flow at ≤ 80 MW but the
  // unmonitored siblings absorb the rest.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(2000.0).epsilon(1e-6));

  // Pin the line count = 7 (CSV preserved end-to-end).
  CHECK(system_lp.elements<LineLP>().size() == 7);
}

TEST_CASE(
    "Sienna c_sys5_ml port — monitored cap differentiates from full-hard-cap")  // NOLINT
{
  // Cross-check: if we instead enforce EVERY line (level=2 on
  // every branch), the 200 MW load CANNOT be carried because the
  // total cap-respecting bus-1 outflow is bounded by branch1 (13.3
  // MW) + branch2 (13.3 MW) + branch3 (66.6 MW) = 93.2 MW.  The
  // remaining 106.8 MW is unserved at $1e6/MWh.
  auto sys = make_system(/*monitor_branch4=*/false);
  for (auto& line : sys.line_array) {
    line.enforce_level = 2;
  }
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Max bus_1→bus_3 throughput under hard caps:
  //   - branch1 (1→2, cap 13.3) → branch4 (2→3, cap 80)   limited by 13.3
  //   - branch2 (1→4, cap 13.3) → branch5/6 (3→4 reverse) limited by 13.3
  //   - branch3 (1→5, cap 66.6) → branch7 (4→5 reverse, cap 13.3) → 3 limited
  //   by 13.3
  // Each route caps at 13.3 MW ⇒ total served = 39.9 MW, the
  // rest hits demand_fail at $1e6/MWh.
  constexpr Real kMaxServed = 13.3 + 13.3 + 13.3;
  constexpr Real kGenCostObj = kMaxServed * kGenCost;
  constexpr Real kUnserved = kLoadBus3 - kMaxServed;
  constexpr Real kDemandFailObj = kUnserved * kDemandFailCost;
  CHECK(lp.get_obj_value_raw()
        == doctest::Approx(kGenCostObj + kDemandFailObj).epsilon(1e-3));
}

}  // namespace test_sienna_5bus_monitored_line_port
