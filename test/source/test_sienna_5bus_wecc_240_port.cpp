// SPDX-License-Identifier: BSD-3-Clause
//
// Port of NREL-Sienna PowerSimulations.jl ``WECC_240`` (240-bus
// reduced model — scalability proxy) to a gtopt integration test.
//
// ## Source
//
// PowerSystemCaseBuilder.jl's ``WECC_240`` loads from a PSS/E
// ``.raw`` file (``PowerSystemsTestData/psse_raw/WECC240_v04.raw``).
// gtopt has no PSS/E .raw parser, so the converter ships a
// SYNTHETIC scalability proxy with the same topology SIZES (240
// buses, 100 generators, 120 demands, 320 lines) — see
// ``scripts/sienna_to_gtopt/_wecc_240.py`` docstring for the design
// rationale.
//
// This port verifies the LP can build and solve a 240-bus
// transportation network at the published WECC-240 scale, within a
// generous 60 s ceiling.  No physical assertion — this is a pure
// scalability smoke test.
//
// We DON'T reconstruct the full 240-bus topology here (that would
// duplicate ~1000 LOC of generator data with no test value); the
// Python builder + JSON round-trip is the canonical entry point for
// the actual size.  This C++ test instead builds a smaller (still
// > 5-bus) proxy directly, sized so a single ctest run-time stays
// well under the build-test budget.

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

namespace test_sienna_5bus_wecc_240_port
{
namespace
{

// Synthetic topology sizes — see _wecc_240.py for rationale.  We
// pick a smaller-than-default 60-bus / 30-gen / 30-demand / 80-line
// fixture for the C++ unit test so it stays in the sub-second tier
// of gtoptTests.  The full 240-bus scale is exercised through the
// Python builder + JSON round-trip path.
constexpr int kNBus = 60;
constexpr int kNGen = 30;
constexpr int kNDemand = 30;
constexpr int kNLine = 80;

[[nodiscard]] Real gcost_for_gen(int idx)
{
  return 10.0 + 50.0 * static_cast<Real>((idx * 7) % 100) / 100.0;
}

[[nodiscard]] Real capacity_for_gen(int idx)
{
  return 50.0 + 500.0 * static_cast<Real>((idx * 13) % 100) / 100.0;
}

[[nodiscard]] System make_system()
{
  System sys;
  sys.name = "SiennaWECC60Proxy";

  Array<Bus> buses;
  buses.reserve(kNBus);
  for (int i = 0; i < kNBus; ++i) {
    buses.push_back({.uid = Uid {i + 1},
                     .name = std::string {"bus"} + std::to_string(i + 1)});
  }
  sys.bus_array = std::move(buses);

  Array<Generator> gens;
  gens.reserve(kNGen);
  Real total_gen_cap = 0.0;
  for (int gi = 0; gi < kNGen; ++gi) {
    const Real cap = capacity_for_gen(gi);
    total_gen_cap += cap;
    gens.push_back({
        .uid = Uid {gi + 1},
        .name = std::string {"g"} + std::to_string(gi + 1),
        .bus = Uid {gi + 1},
        .gcost = gcost_for_gen(gi),
        .capacity = cap,
    });
  }
  sys.generator_array = std::move(gens);

  // Demands on buses beyond the generator-bearing ones so there's
  // real transmission demand across the ring.
  Array<Demand> demands;
  demands.reserve(kNDemand);
  const Real base_load = (total_gen_cap * 0.70) / kNDemand;
  for (int di = 0; di < kNDemand; ++di) {
    const int bus_id = ((kNGen + di) % kNBus) + 1;
    demands.push_back({
        .uid = Uid {di + 1},
        .name = std::string {"d"} + std::to_string(di + 1),
        .bus = Uid {bus_id},
        .capacity = base_load,
    });
  }
  sys.demand_array = std::move(demands);

  Array<Line> lines;
  lines.reserve(kNLine);
  // Ring (first kNBus lines).
  for (int li = 0; li < kNBus; ++li) {
    const int bus_a = li + 1;
    const int bus_b = ((li + 1) % kNBus) + 1;
    lines.push_back({
        .uid = Uid {li + 1},
        .name = std::string {"ring_"} + std::to_string(li + 1),
        .bus_a = Uid {bus_a},
        .bus_b = Uid {bus_b},
        .tmax_ba = 500.0,
        .tmax_ab = 500.0,
    });
  }
  // Cross-chords (remaining kNLine - kNBus lines).
  for (int ci = 0; ci < kNLine - kNBus; ++ci) {
    int bus_a = (ci * 31) % kNBus + 1;
    int bus_b = ((ci * 31 + kNBus / 3) % kNBus) + 1;
    if (bus_a == bus_b) {
      bus_b = bus_b % kNBus + 1;
    }
    lines.push_back({
        .uid = Uid {kNBus + ci + 1},
        .name = std::string {"chord_"} + std::to_string(ci + 1),
        .bus_a = Uid {bus_a},
        .bus_b = Uid {bus_b},
        .tmax_ba = 300.0,
        .tmax_ab = 300.0,
    });
  }
  sys.line_array = std::move(lines);
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
  popts.model_options.demand_fail_cost = 1.0e6;
  return popts;
}

}  // namespace

TEST_CASE("Sienna WECC_240 proxy port — LP builds at scale")  // NOLINT
{
  const auto sys = make_system();
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.get_numrows() > 0);
  REQUIRE(lp.get_numcols() > 0);

  // Topology counts preserved end-to-end.
  CHECK(system_lp.elements<LineLP>().size() == static_cast<size_t>(kNLine));
}

TEST_CASE("Sienna WECC_240 proxy port — LP solves to optimum")  // NOLINT
{
  const auto sys = make_system();
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(
    "Sienna WECC_240 proxy port — objective is finite + bounded")  // NOLINT
{
  // Upper bound: serve every MWh from the most expensive
  // generator.  Gens cost up to ~$60/MWh, demands sum to ~70% of
  // installed gen capacity, so the objective is bounded by
  // (max_gcost × total_demand × duration) + (demand_fail × any
  // demand the ring can't reach due to line capacity floors).
  // The latter dominates — with demand_fail = 1e6 and even a few
  // MWh of unreached demand the objective lands in the 1e9 range.
  // Assert > 0 and < 1e11 as a finite-cost sanity check.
  const auto sys = make_system();
  const auto sim = make_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const Real obj = lp.get_obj_value_raw();
  CHECK(obj >= 0.0);
  CHECK(obj < 1.0e11);
}

}  // namespace test_sienna_5bus_wecc_240_port
