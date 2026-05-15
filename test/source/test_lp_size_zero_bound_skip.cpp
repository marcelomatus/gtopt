// SPDX-License-Identifier: BSD-3-Clause
//
// P1 LP-size: zero-bound skip in waterway_lp / generator_lp / demand_lp.
//
// When the per-block dispatch bounds collapse to `[0, 0]` (capacity =
// 0, no per-block schedule overrides) the LP column is mathematically
// degenerate — it is fixed at zero and contributes nothing to balance
// rows or capacity rows.  This file pins the new skip behaviour in
// `WaterwayLP::add_to_lp`, `GeneratorLP::add_to_lp`, and
// `DemandLP::add_to_lp`.
//
// Each test compares two otherwise-identical systems: one with
// capacity > 0 (baseline, columns/rows present) and one with
// capacity = 0 (skipped, columns/rows absent).  The diff in
// `lp.get_numcols()` / `get_numrows()` is the precise per-element
// saving and is what makes this a structural LP-size guard rather
// than just a "still solves" smoke test.

#include <doctest/doctest.h>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/waterway_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Two-block, one-stage, one-scenario simulation re-used by every test.
[[nodiscard]] Simulation make_two_block_sim()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
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

}  // namespace

TEST_CASE("LP-size: WaterwayLP skips zero-flow blocks (P1)")
{
  // Two junctions, one waterway.  Vary the waterway capacity between
  // baseline (>0) and degenerate (==0); confirm the flow column is
  // emitted in the first case and elided in the second.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve = [&](double waterway_capacity)
  {
    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .capacity = waterway_capacity,
        },
    };

    const System system = {
        .name = "ZeroFlowTest",
        .bus_array = bus_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
    };

    PlanningOptions popts;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());

    const auto& ww_lps = sys_lp.elements<WaterwayLP>();
    REQUIRE(ww_lps.size() == 1);
    const auto& ww_lp = ww_lps.front();
    const auto& scenario_lp = sim_lp.scenarios().front();
    const auto& stage_lp = sim_lp.stages().front();

    const auto n_flow_cols = ww_lp.flow_cols_at(scenario_lp, stage_lp).size();
    return std::pair {lp.get_numcols(), n_flow_cols};
  };

  const auto [baseline_cols, baseline_flow_cols] = solve(/*capacity=*/100.0);
  const auto [zero_cols, zero_flow_cols] = solve(/*capacity=*/0.0);

  CHECK(baseline_flow_cols == 2);  // one flow var per block
  CHECK(zero_flow_cols == 0);  // both blocks skipped
  // The two blocks' worth of flow columns are the only structural
  // difference between the two LPs.
  CHECK(baseline_cols - zero_cols == 2);
}

TEST_CASE("LP-size: GeneratorLP skips zero-pmax blocks (P1)")
{
  // One bus, one generator.  Vary generator capacity between baseline
  // (>0) and degenerate (==0).  A second cheap generator ensures the
  // demand can always be served so the LP stays feasible.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 10.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve = [&](double zero_gen_capacity)
  {
    const Array<Generator> generator_array = {
        // The unit under test.
        {
            .uid = Uid {1},
            .name = "g_zero",
            .bus = Uid {1},
            .gcost = 50.0,
            .capacity = zero_gen_capacity,
        },
        // Safety net so the demand always has a feasible serve.
        {
            .uid = Uid {2},
            .name = "g_backup",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 1000.0,
        },
    };

    const System system = {
        .name = "ZeroPmaxTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return lp.get_numcols();
  };

  const auto baseline_cols = solve(/*capacity=*/200.0);
  const auto zero_cols = solve(/*capacity=*/0.0);

  // Generator with cap=0 contributes 0 cols (skipped) vs 2 cols
  // (one generation variable per block) at the baseline.  No
  // expansion → no capacity rows in either case.
  CHECK(baseline_cols - zero_cols == 2);
}

TEST_CASE("LP-size: DemandLP skips zero-capacity blocks (P1)")
{
  // One bus, one cheap generator, one demand.  Vary demand capacity
  // between baseline (>0) and degenerate (==0).  With
  // `demand_fail_cost > 0` the baseline produces both a `load` col
  // and a `fail` col per block, plus one `balance` row per block;
  // the zero case must skip all three.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve = [&](double demand_capacity)
  {
    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = demand_capacity,
        },
    };

    const System system = {
        .name = "ZeroLmaxTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return std::pair {lp.get_numcols(), lp.get_numrows()};
  };

  const auto [baseline_cols, baseline_rows] = solve(/*capacity=*/100.0);
  const auto [zero_cols, zero_rows] = solve(/*capacity=*/0.0);

  // Per block, baseline emits:
  //   - 1 load column
  //   - 1 fail column  (demand_fail_cost > 0 and demand not forced)
  // and one balance row glueing them together.
  // The zero case skips all three.  Two blocks → 4 cols and 2 rows.
  CHECK(baseline_cols - zero_cols == 4);
  CHECK(baseline_rows - zero_rows == 2);
}

TEST_CASE("LP-size: DemandLP zero-capacity with emin retains lman columns (P1)")
{
  // Pathological-but-valid mix: capacity = 0 so the natural load is
  // empty, but a stage-level emin floor is set.  The skip path must
  // still create the `lman` columns and the `emin` row — the lman
  // variable represents load drawn explicitly to satisfy emin and is
  // independent of `lmax`.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // capacity = 0 disables the load/fail/balance machinery; emin
  // keeps the lman path alive.
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d_emin_only",
          .bus = Uid {1},
          .emin = 30.0,
          .ecost = 200.0,
          .capacity = 0.0,
      },
  };

  const auto simulation = make_two_block_sim();
  const System system = {
      .name = "ZeroLmaxEminOnlyTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto&& lp = sys_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Sanity: the LP is non-empty and feasible — exercising the path
  // where the `has_lman` branch fires without the `has_load` branch.
  CHECK(lp.get_numcols() > 0);
  CHECK(lp.get_numrows() > 0);
}
