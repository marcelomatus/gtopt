// SPDX-License-Identifier: BSD-3-Clause
//
// Port of NREL-Sienna PowerSimulations.jl ``c_sys5_ml`` (Monitored
// Line) to a gtopt integration test.
//
// ## Source
//
// PowerSimulations.jl ships ``MonitoredLine`` to model branches
// whose thermal limit is enforced; sibling ``Line`` rows on the
// same case are treated as unconstrained.  gtopt ONCE mirrored this
// knob with ``Line.enforce_level`` (level=0 = unconstrained,
// level=2 = hard cap).
//
// RETIRED 2026-06-10: ``Line.enforce_level`` is now a NO-OP.
// ``source/line_lp.cpp`` passes ``enforce_capacity = true``
// unconditionally, so EVERY line's ``tmax`` ALWAYS binds regardless
// of the (ignored) ``enforce_level`` value — there is no longer an
// "unconstrained" line.  This test was originally written to prove
// EL=0 freed the cap; it is repurposed here to pin the post-retirement
// invariant: ``enforce_level`` no longer changes the LP, and the hard
// cap binds on every branch.  The all-hard-cap throughput limit is the
// only outcome the LP can produce now.
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

using namespace gtopt;

namespace test_sienna_5bus_monitored_line_port
{
namespace
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

// Max bus_1→bus_3 throughput under hard caps (transport model, every
// branch's tmax binds).  Each of the three bus-1 export routes is
// throttled to 13.3 MW by its tightest leg:
//   - branch1 (1→2, cap 13.3) → branch4 (2→3, cap 80)   limited by 13.3
//   - branch2 (1→4, cap 13.3) → branch5/6 (3→4 reverse) limited by 13.3
//   - branch3 (1→5, cap 66.6) → branch7 (4→5 reverse, cap 13.3) → 3
//                                                         limited by 13.3
// ⇒ total served = 39.9 MW; the rest hits demand_fail at $1e6/MWh.
constexpr Real kMaxServed = 13.3 + 13.3 + 13.3;
constexpr Real kHardCapObj =
    (kMaxServed * kGenCost) + ((kLoadBus3 - kMaxServed) * kDemandFailCost);

TEST_CASE(
    "Sienna c_sys5_ml port — enforce_level=0 is a no-op: caps still bind")  // NOLINT
{
  // RETIRED-flag invariant: even with EVERY line at enforce_level=0,
  // the caps ALL bind (enforce_level no longer frees the flow column).
  // The 200 MW load CANNOT be served — bus-1 export is throttled to
  // 39.9 MW by the three 13.3-MW route bottlenecks, identical to the
  // all-hard-cap (level=2) outcome below.
  const auto sys = make_system(/*monitor_branch4=*/false);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // enforce_level=0 binds exactly like level=2: served = 39.9 MW, the
  // remaining 160.1 MW is unserved at $1e6/MWh.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(kHardCapObj).epsilon(1e-3));
}

TEST_CASE(
    "Sienna c_sys5_ml port — monitored branch4 no longer differs from EL=0")  // NOLINT
{
  // With branch4 "monitored" (enforce_level=2) and every other branch
  // at enforce_level=0, the result is IDENTICAL to the all-EL=0 case:
  // since enforce_level is a no-op, all seven caps bind either way.
  // The monitored/un-monitored distinction has no effect on the LP.
  const auto sys = make_system(/*monitor_branch4=*/true);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Same throughput as the all-EL=0 case — enforce_level is ignored.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(kHardCapObj).epsilon(1e-3));

  // Pin the line count = 7 (CSV preserved end-to-end).
  CHECK(system_lp.elements<LineLP>().size() == 7);
}

TEST_CASE(
    "Sienna c_sys5_ml port — explicit all-level=2 matches the no-op result")  // NOLINT
{
  // Cross-check: explicitly setting EVERY line to enforce_level=2
  // produces the SAME objective as the EL=0 cases above — confirming
  // enforce_level is inert.  The 200 MW load is throttled to 39.9 MW
  // by the three 13.3-MW route bottlenecks.
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

  // Same 39.9-MW throughput limit derived at file scope (kHardCapObj).
  CHECK(lp.get_obj_value_raw() == doctest::Approx(kHardCapObj).epsilon(1e-3));
}

}  // namespace test_sienna_5bus_monitored_line_port
