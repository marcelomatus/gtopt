// SPDX-License-Identifier: BSD-3-Clause
//
// Port of NREL-Sienna PowerSimulations.jl ``c_sys5_dc`` (HVDC link)
// to a gtopt integration test.
//
// ## Source
//
// PowerSimulations.jl models HVDC branches with
// ``TwoTerminalHVDCLine`` (formerly ``HVDCLine``).  gtopt's
// equivalent mechanism is structural: a ``Line`` row with NO
// ``reactance`` value is excluded from the Kirchhoff voltage-law
// row assembly (see ``source/planning_lp.cpp`` line 132 — ``if
// (!line.reactance.has_value()) continue;``), so it behaves as a
// pure flow-capacity element — exactly the DC-link contract.
//
// ``c_sys5_dc`` upgrades ONE branch of the 5-bus case to an HVDC
// link.  The branch picked here is ``branch4`` (bus 2 → bus 3,
// rated 80 MW), matching the converter's
// ``_hvdc.build_hvdc(case)`` default.
//
// This integration test exercises:
//
//   * The KVL row count drops by exactly ONE per block (the
//     reactance-less DC line contributes no KVL row).
//   * The LP still solves to optimum.
//   * Removing reactance from a single line does not perturb the
//     objective by more than 5% compared to the all-AC baseline
//     (the loss-free, lossless flow is feasible in both).

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

namespace test_sienna_5bus_hvdc_port
{
namespace
{

constexpr Uid kBusUid1 {1};
constexpr Uid kBusUid2 {2};
constexpr Uid kBusUid3 {3};
constexpr Uid kBusUid4 {4};
constexpr Uid kBusUid5 {10};

constexpr Real kLoadBus2 = 21.4285714;
constexpr Real kLoadBus3 = 21.4285714;
constexpr Real kLoadBus4 = 28.5714286;
constexpr Real kTotalLoad = kLoadBus2 + kLoadBus3 + kLoadBus4;

constexpr Real kGenCost = 10.0;
constexpr Real kGenCap = 500.0;

// Bundle's branch.csv reactances (X column, p.u.).
constexpr Real kXBranch4 = 0.0108;
constexpr Real kXBranch1 = 0.0281;
constexpr Real kXBranch2 = 0.0304;
constexpr Real kXBranch7 = 0.0297;
constexpr Real kXBranch3 = 0.0064;
constexpr Real kXBranch5 = 0.0297;
constexpr Real kXBranch6 = 0.03274425;

// Loose envelope ratings so the LP can route flow either way.
constexpr Real kBigRating = 500.0;

[[nodiscard]] System make_system(bool dc_branch4)
{
  System sys;
  sys.name = dc_branch4 ? "SiennaC5DcWithHVDC" : "SiennaC5DcAllAc";
  sys.bus_array = {
      {.uid = kBusUid1, .name = "bus1"},
      {.uid = kBusUid2, .name = "bus2"},
      {.uid = kBusUid3, .name = "bus3"},
      {.uid = kBusUid4, .name = "bus4"},
      {.uid = kBusUid5, .name = "bus5"},
  };
  sys.demand_array = {
      {.uid = Uid {1},
       .name = "load_bus2",
       .bus = kBusUid2,
       .capacity = kLoadBus2},
      {.uid = Uid {2},
       .name = "load_bus3",
       .bus = kBusUid3,
       .capacity = kLoadBus3},
      {.uid = Uid {3},
       .name = "load_bus4",
       .bus = kBusUid4,
       .capacity = kLoadBus4},
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

  // 7 branches, all AC by default.  When dc_branch4 is true,
  // branch4 (the first row) is built with NO reactance field —
  // making it an HVDC link in gtopt's KVL-skip semantics.
  auto ac = [&](Uid u, std::string_view name, Uid a, Uid b, Real x)
  {
    return Line {
        .uid = u,
        .name = std::string {name},
        .type = std::string {"ac"},
        .bus_a = a,
        .bus_b = b,
        .reactance = x,
        .tmax_ba = kBigRating,
        .tmax_ab = kBigRating,
    };
  };
  auto dc = [&](Uid u, std::string_view name, Uid a, Uid b)
  {
    return Line {
        .uid = u,
        .name = std::string {name},
        .type = std::string {"dc"},
        .bus_a = a,
        .bus_b = b,
        .tmax_ba = kBigRating,
        .tmax_ab = kBigRating,
        // No `reactance` — KVL skip.
    };
  };

  sys.line_array.reserve(7);
  if (dc_branch4) {
    sys.line_array.push_back(dc(Uid {1}, "branch4", kBusUid2, kBusUid3));
  } else {
    sys.line_array.push_back(
        ac(Uid {1}, "branch4", kBusUid2, kBusUid3, kXBranch4));
  }
  sys.line_array.push_back(
      ac(Uid {2}, "branch1", kBusUid1, kBusUid2, kXBranch1));
  sys.line_array.push_back(
      ac(Uid {3}, "branch2", kBusUid1, kBusUid4, kXBranch2));
  sys.line_array.push_back(
      ac(Uid {4}, "branch7", kBusUid4, kBusUid5, kXBranch7));
  sys.line_array.push_back(
      ac(Uid {5}, "branch3", kBusUid1, kBusUid5, kXBranch3));
  sys.line_array.push_back(
      ac(Uid {6}, "branch5", kBusUid3, kBusUid4, kXBranch5));
  sys.line_array.push_back(
      ac(Uid {7}, "branch6", kBusUid3, kBusUid4, kXBranch6));
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
  popts.model_options.use_kirchhoff = true;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1.0e6;
  return popts;
}

}  // namespace

TEST_CASE("Sienna c_sys5_dc port — all-AC baseline solves")  // NOLINT
{
  const auto sys = make_system(/*dc_branch4=*/false);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 71.43 MW × $10/MWh.
  CHECK(lp.get_obj_value_raw()
        == doctest::Approx(kTotalLoad * kGenCost).epsilon(1e-4));
  CHECK(system_lp.elements<LineLP>().size() == 7);
}

TEST_CASE(
    "Sienna c_sys5_dc port — HVDC branch4 still solves to same objective")  // NOLINT
{
  // The HVDC variant relaxes one KVL constraint (branch4's).  In a
  // meshed 5-bus network the LP gains an extra flow degree of
  // freedom but the demand-cost merit order is unchanged (g1 is
  // the only generator), so the objective must match the all-AC
  // baseline.
  const auto sys = make_system(/*dc_branch4=*/true);
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  CHECK(lp.get_obj_value_raw()
        == doctest::Approx(kTotalLoad * kGenCost).epsilon(1e-4));
}

TEST_CASE(
    "Sienna c_sys5_dc port — KVL row count drops by 1 vs the all-AC build")  // NOLINT
{
  // The decisive structural test: removing reactance from a single
  // line drops exactly ONE KVL row per block from the LP (gtopt
  // assembles one KVL equality row per reactance-bearing line per
  // block).  We solve both variants and compare row counts.
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());

  auto solve_and_count = [&](const System& sys) -> int
  {
    SimulationLP simulation_lp(sim, options);
    SystemLP system_lp(sys, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    return static_cast<int>(lp.get_numrows());
  };

  const int ac_rows = solve_and_count(make_system(/*dc_branch4=*/false));
  const int dc_rows = solve_and_count(make_system(/*dc_branch4=*/true));

  // Single block ⇒ row delta = exactly 1 (one KVL equality per
  // reactance-bearing line per block).
  CHECK(ac_rows - dc_rows == 1);
}

}  // namespace test_sienna_5bus_hvdc_port
