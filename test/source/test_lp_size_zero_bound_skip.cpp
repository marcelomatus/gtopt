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

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>
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
    popts.model_options.lp_reduction = true;  // tests the elimination feature
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

  // Per block, post-P0 demand-failure substitution baseline emits:
  //   - 1 load column (carrying both load and fail semantics via
  //     the `−fail_cost × ecost` cost coefficient + `add_obj_constant`
  //     baseline)
  // The zero case skips it.  Two blocks → 2 cols saved.
  // No `balance` row anymore (substituted away); no row diff.
  CHECK(baseline_cols - zero_cols == 2);
  CHECK(baseline_rows - zero_rows == 0);
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

// ---------------------------------------------------------------------------
// Optional-returning lookup accessors added in commit 209ac0fc.
//
// The P1 zero-bound skip in `generator_lp.cpp` / `waterway_lp.cpp` can elide
// per-block LP columns when both dispatch bounds collapse to `[0, 0]`.
// Downstream consumers (TurbineLP and friends) used to call
// `gen_cols.at(buid)` / `flow_cols.at(buid)` which threw
// `std::out_of_range` on missing keys.  The tests below pin the
// optional-returning accessors that replaced those throwing calls and the
// end-to-end "turbine survives an offline generator" integration contract.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "GeneratorLP::lookup_generation_col handles zero-pmax skip")
{
  // Two generators on a single bus: a zero-capacity unit (entirely
  // skipped — outer (s, t) key is absent) and a normal backup whose
  // columns are present.  Test both the per-block and per-(s,t)
  // tolerant accessors.
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
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g_zero",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 0.0,
      },
      {
          .uid = Uid {2},
          .name = "g_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };

  const auto simulation = make_two_block_sim();
  const System system = {
      .name = "LookupGenZeroPmax",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.model_options.lp_reduction = true;  // tests the elimination feature
  popts.model_options.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  const auto& gen_lps = sys_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);
  const auto& g_zero = gen_lps[0];
  const auto& g_live = gen_lps[1];
  REQUIRE(g_zero.uid() == Uid {1});
  REQUIRE(g_live.uid() == Uid {2});

  const auto& scenario_lp = sim_lp.scenarios().front();
  const auto& stage_lp = sim_lp.stages().front();

  // Per-block lookup on the zero-cap generator: both blocks elided.
  CHECK_FALSE(
      g_zero.lookup_generation_col(scenario_lp, stage_lp, make_uid<Block>(1))
          .has_value());
  CHECK_FALSE(
      g_zero.lookup_generation_col(scenario_lp, stage_lp, make_uid<Block>(2))
          .has_value());

  // Per-(s,t) lookup on the zero-cap generator: returns an empty
  // inner map (the find_or_empty_inner fallback static).
  CHECK(g_zero.lookup_generation_cols(scenario_lp, stage_lp).empty());

  // Live generator: block 1 has a column, block 99 (missing uid) does
  // not, and the per-(s,t) inner map is the populated one (size 2).
  CHECK(g_live.lookup_generation_col(scenario_lp, stage_lp, make_uid<Block>(1))
            .has_value());
  CHECK_FALSE(
      g_live.lookup_generation_col(scenario_lp, stage_lp, make_uid<Block>(99))
          .has_value());
  CHECK(g_live.lookup_generation_cols(scenario_lp, stage_lp).size() == 2);
}

TEST_CASE(  // NOLINT
    "WaterwayLP::lookup_flow_col handles zero-flow skip")
{
  // One waterway between two junctions.  Vary fmin/fmax between
  // (0, 0) — entire flow column elided — and (0, 100) — normal.
  // A thermal generator + demand keep the LP feasible in both cases.
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
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
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
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto build_and_lookup = [&](double fmax_val)
  {
    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = fmax_val,
        },
    };

    const System system = {
        .name = "LookupWaterway",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    const auto& ww_lps = sys_lp.elements<WaterwayLP>();
    REQUIRE(ww_lps.size() == 1);
    const auto& ww_lp = ww_lps.front();
    const auto& scenario_lp = sim_lp.scenarios().front();
    const auto& stage_lp = sim_lp.stages().front();

    return std::tuple {
        ww_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(1)),
        ww_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(2)),
        ww_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(99)),
    };
  };

  SUBCASE("zero fmax elides every block")
  {
    const auto [b1, b2, b99] = build_and_lookup(0.0);
    CHECK_FALSE(b1.has_value());
    CHECK_FALSE(b2.has_value());
    CHECK_FALSE(b99.has_value());
  }

  SUBCASE("non-zero fmax keeps populated blocks and rejects missing uids")
  {
    const auto [b1, b2, b99] = build_and_lookup(100.0);
    CHECK(b1.has_value());
    CHECK(b2.has_value());
    CHECK_FALSE(b99.has_value());
  }
}

TEST_CASE(  // NOLINT
    "FlowLP::lookup_flow_col returns value for live blocks, nullopt otherwise")
{
  // A FlowLP always creates a column per block (lowb=uppb=discharge),
  // so the happy-path lookup must succeed; only missing block uids
  // hit the nullopt path.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
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
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_pasada",
          .drain = true,
      },
  };
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_flow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 30.0,
      },
  };

  const auto simulation = make_two_block_sim();
  const System system = {
      .name = "LookupFlow",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .flow_array = flow_array,
  };

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 10000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  const auto& flow_lps = sys_lp.elements<FlowLP>();
  REQUIRE(flow_lps.size() == 1);
  const auto& flow_lp = flow_lps.front();
  const auto& scenario_lp = sim_lp.scenarios().front();
  const auto& stage_lp = sim_lp.stages().front();

  // Both real blocks have flow columns.
  const auto col_b1 =
      flow_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(1));
  const auto col_b2 =
      flow_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(2));
  CHECK(col_b1.has_value());
  CHECK(col_b2.has_value());

  // Missing block uid → nullopt (inner map lookup miss).
  CHECK_FALSE(
      flow_lp.lookup_flow_col(scenario_lp, stage_lp, make_uid<Block>(99))
          .has_value());
}

TEST_CASE(  // NOLINT
    "TurbineLP::add_to_lp survives offline generator (zero capacity)")
{
  // End-to-end contract: a Turbine attached to a zero-capacity Generator
  // must not throw during SystemLP construction.  Pre-fix the missing
  // gen column tripped a `flat_map::at` in the turbine's add_to_lp;
  // post-fix the turbine skips the conversion row via the optional
  // lookup and the LP solves with the thermal backup carrying the load.
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
          .capacity = 50.0,
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
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve = [&](double hydro_capacity)
  {
    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "hydro_gen",
            .bus = Uid {1},
            .gcost = 5.0,
            .capacity = hydro_capacity,
        },
        {
            .uid = Uid {2},
            .name = "thermal_backup",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 200.0,
        },
    };
    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur1",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };
    const System system = {
        .name = "TurbineOfflineGen",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions popts;
    popts.model_options.lp_reduction = true;  // tests the elimination feature
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);

    // CRITICAL: building the SystemLP must not throw, even when the
    // hydro generator's columns are entirely elided by the P1 skip.
    REQUIRE_NOTHROW(SystemLP {system, sim_lp});

    SystemLP sys_lp(system, sim_lp);
    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return std::pair {lp.get_numrows(), lp.get_numcols()};
  };

  const auto [base_rows, base_cols] = solve(/*hydro_capacity=*/100.0);
  const auto [zero_rows, zero_cols] = solve(/*hydro_capacity=*/0.0);

  // With the hydro generator offline the turbine must not emit any
  // conversion rows: the zero-capacity LP must have strictly fewer
  // rows than the baseline (the two block-level conversion rows are
  // absent, alongside the generation columns and waterway flow columns
  // which P1 already elides).
  CHECK(base_rows > zero_rows);
  CHECK(base_cols > zero_cols);
}

// ---------------------------------------------------------------------------
// Full-path parquet round-trip tests: build → solve → write parquet → read
// back and assert what an elided element produced (or did not produce).
//
// Output-file layout reminder (see test_output_context.cpp): each
// `<Class>/<field>_sol.parquet/` is a Hive-partitioned directory with
// `scene=<s>/phase=<p>/part.parquet` leaf files.  Each leaf table is
// long: `scenario, stage, block, uid, value` with one row per non-zero
// cell.  When a P1 skip elides every block for an element — or the value
// is identically zero — the writer emits **no rows** for that uid (zeros
// are dropped).  The tests below pin that contract.
// ---------------------------------------------------------------------------

namespace
{

// Resolve the on-disk leaf path for `(scene=0, phase=0)`.  Returns the
// path with the `.parquet` extension (for filesystem::exists checks).
[[nodiscard]] std::filesystem::path leaf_parquet(
    const std::filesystem::path& dataset_dir)
{
  return dataset_dir / "scene=0" / "phase=0" / "part.parquet";
}

// Same path without the `.parquet` extension — `parquet_read_table()`
// auto-appends the suffix.
[[nodiscard]] std::filesystem::path leaf_parquet_stem(
    const std::filesystem::path& dataset_dir)
{
  return dataset_dir / "scene=0" / "phase=0" / "part";
}

// Long output reader: return the `value` rows whose `uid` column equals
// `uid`.  Zeros are dropped at write time, so an all-zero element yields an
// empty vector (the long-form counterpart of "no `uid:N` column").  Handles
// the uint16 uid column and the float32 / float64 value column.
[[nodiscard]] std::vector<double> long_uid_values(const ArrowTable& table,
                                                  int uid)
{
  std::vector<double> out;
  const int uid_idx = table->schema()->GetFieldIndex("uid");
  const int val_idx = table->schema()->GetFieldIndex("value");
  if (uid_idx < 0 || val_idx < 0) {
    return out;
  }
  const auto combined = table->CombineChunks().ValueOr(table);
  const auto uchunk = combined->column(uid_idx)->chunk(0);
  const auto vchunk = combined->column(val_idx)->chunk(0);
  if (!uchunk || !vchunk) {
    return out;
  }
  const auto read_uid = [&](int64_t i) -> int64_t
  {
    switch (uchunk->type_id()) {
      case arrow::Type::UINT16:
        return std::static_pointer_cast<arrow::UInt16Array>(uchunk)->Value(i);
      case arrow::Type::INT32:
        return std::static_pointer_cast<arrow::Int32Array>(uchunk)->Value(i);
      case arrow::Type::INT64:
        return std::static_pointer_cast<arrow::Int64Array>(uchunk)->Value(i);
      default:
        return -1;
    }
  };
  const auto read_val = [&](int64_t i) -> double
  {
    switch (vchunk->type_id()) {
      case arrow::Type::FLOAT:
        return std::static_pointer_cast<arrow::FloatArray>(vchunk)->Value(i);
      case arrow::Type::DOUBLE:
        return std::static_pointer_cast<arrow::DoubleArray>(vchunk)->Value(i);
      default:
        return 0.0;
    }
  };
  for (int64_t i = 0; i < uchunk->length(); ++i) {
    if (read_uid(i) == uid) {
      out.push_back(read_val(i));
    }
  }
  return out;
}

}  // namespace

TEST_CASE(  // NOLINT
    "LP-size: zero-pmax generator produces no uid column in generation_sol "
    "parquet")
{
  // Two generators on a single bus.  `g_zero` (uid=1) has capacity=0 →
  // entirely skipped by the P1 generator_lp zero-pmax guard.
  // `g_backup` (uid=2) carries the load with `gcost=100, capacity=1000`.
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

  auto solve_and_write =
      [&](double zero_gen_capacity, const std::filesystem::path& outdir)
  {
    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "g_zero",
            .bus = Uid {1},
            .gcost = 50.0,
            .capacity = zero_gen_capacity,
        },
        {
            .uid = Uid {2},
            .name = "g_backup",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 1000.0,
        },
    };

    const System system = {
        .name = "ZeroPmaxParquetTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    PlanningOptions popts;
    popts.model_options.lp_reduction = true;  // tests the elimination feature
    popts.model_options.demand_fail_cost = 10000.0;
    popts.output_directory = outdir.string();
    popts.output_format = DataFormat::parquet;
    // Long output: a skipped element emits no rows (its zeros are dropped),
    // the long-form counterpart of the wide "no `uid:N` column" contract.

    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    sys_lp.write_out();
  };

  const auto base_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_zero_gen_base";
  const auto zero_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_zero_gen_skip";
  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
  std::filesystem::create_directories(base_dir);
  std::filesystem::create_directories(zero_dir);

  solve_and_write(/*zero_gen_capacity=*/200.0, base_dir);
  solve_and_write(/*zero_gen_capacity=*/0.0, zero_dir);

  // Baseline: `g_zero` (uid 1) is the cheap unit and covers the full 10 MW;
  // `g_backup` (uid 2) generates 0, so its zero rows are dropped in long.
  const auto base_dataset = base_dir / "Generator" / "generation_sol.parquet";
  REQUIRE(std::filesystem::exists(leaf_parquet(base_dataset)));
  const auto base_table = parquet_read_table(leaf_parquet_stem(base_dataset));
  REQUIRE(base_table.has_value());

  const auto base_g_zero = long_uid_values(*base_table, 1);
  const auto base_g_backup = long_uid_values(*base_table, 2);
  // Two blocks, one scenario, one stage → two rows for the active unit.
  CHECK(base_g_zero.size() == 2);
  CHECK(base_g_backup.empty());  // 0 generation → dropped
  for (const auto v : base_g_zero) {
    CHECK(v == doctest::Approx(10.0));
  }

  // Zero-cap variant: `g_zero` is elided (capacity 0) → no rows for uid 1;
  // `g_backup` becomes the only feasible unit and carries the whole 10 MW.
  const auto zero_dataset = zero_dir / "Generator" / "generation_sol.parquet";
  REQUIRE(std::filesystem::exists(leaf_parquet(zero_dataset)));
  const auto zero_table = parquet_read_table(leaf_parquet_stem(zero_dataset));
  REQUIRE(zero_table.has_value());

  CHECK(long_uid_values(*zero_table, 1).empty());
  const auto zero_g_backup = long_uid_values(*zero_table, 2);
  CHECK(zero_g_backup.size() == 2);
  for (const auto v : zero_g_backup) {
    CHECK(v == doctest::Approx(10.0));
  }

  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
}

TEST_CASE(  // NOLINT
    "LP-size: zero-flow waterway produces no uid column in flow_sol parquet")
{
  // One waterway between two junctions, plus a thermal generator and a
  // demand to keep the LP feasible.  Vary the waterway capacity between
  // baseline (>0) and degenerate (==0).
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
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
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

  auto solve_and_write =
      [&](double waterway_capacity, const std::filesystem::path& outdir)
  {
    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .capacity = waterway_capacity,
            .fmin = 0.0,
            .fmax = waterway_capacity,
        },
    };

    const System system = {
        .name = "ZeroFlowParquetTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    popts.output_directory = outdir.string();
    popts.output_format = DataFormat::parquet;
    // Long output: a skipped element emits no rows (its zeros are dropped),
    // the long-form counterpart of the wide "no `uid:N` column" contract.

    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    sys_lp.write_out();
  };

  const auto base_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_zero_ww_base";
  const auto zero_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_zero_ww_skip";
  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
  std::filesystem::create_directories(base_dir);
  std::filesystem::create_directories(zero_dir);

  solve_and_write(/*waterway_capacity=*/100.0, base_dir);
  solve_and_write(/*waterway_capacity=*/0.0, zero_dir);

  // Baseline: the single waterway carries 0 flow (no upstream source), so in
  // long output its zero rows are dropped — uid 1 has no flow rows, and the
  // dataset may not be written at all.
  const auto base_dataset = base_dir / "Waterway" / "flow_sol.parquet";
  const auto base_pq = leaf_parquet(base_dataset);
  if (std::filesystem::exists(base_pq)) {
    const auto base_table = parquet_read_table(leaf_parquet_stem(base_dataset));
    REQUIRE(base_table.has_value());
    CHECK(long_uid_values(*base_table, 1).empty());
  }

  // Zero-capacity variant: the P1 waterway zero-flow guard elides every
  // block, so uid 1 likewise has no rows and the dataset may be absent.
  const auto zero_dataset = zero_dir / "Waterway" / "flow_sol.parquet";
  const auto zero_pq = leaf_parquet(zero_dataset);
  if (std::filesystem::exists(zero_pq)) {
    const auto zero_table = parquet_read_table(leaf_parquet_stem(zero_dataset));
    REQUIRE(zero_table.has_value());
    CHECK(long_uid_values(*zero_table, 1).empty());
  }

  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
}

TEST_CASE(  // NOLINT
    "LP-size: turbine over offline generator survives + parquet shows no "
    "conversion")
{
  // End-to-end regression for commit 209ac0fc: a Turbine attached to a
  // zero-capacity Generator must
  //   (a) not throw during SystemLP construction (pre-fix: flat_map::at
  //       in turbine_lp tripped std::out_of_range), and
  //   (b) leave the parquet outputs in a self-consistent state: the
  //       offline generator's uid column is absent from generation_sol,
  //       and the waterway's flow_sol either omits the uid column or
  //       reports zero flow.
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
          .capacity = 50.0,
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
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .capacity = 100.0,
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const auto simulation = make_two_block_sim();

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 0.0,  // forces the P1 skip in generator_lp
      },
      {
          .uid = Uid {2},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
  const System system = {
      .name = "TurbineOfflineGenParquet",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  const auto outdir =
      std::filesystem::temp_directory_path() / "gtopt_test_turbine_offline";
  std::filesystem::remove_all(outdir);
  std::filesystem::create_directories(outdir);

  PlanningOptions popts;
  popts.model_options.lp_reduction = true;  // tests the elimination feature
  popts.model_options.demand_fail_cost = 10000.0;
  popts.output_directory = outdir.string();
  popts.output_format = DataFormat::parquet;

  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(simulation, options);

  // (a) construction-time contract: no out_of_range.
  REQUIRE_NOTHROW(SystemLP {system, sim_lp});

  SystemLP sys_lp(system, sim_lp);
  auto&& lp = sys_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  sys_lp.write_out();

  // (b) Generator/generation_sol parquet: hydro_gen (uid=1) elided → no rows;
  //     thermal_backup (uid=2) present with the full 50 MW load.
  const auto gen_dataset = outdir / "Generator" / "generation_sol.parquet";
  REQUIRE(std::filesystem::exists(leaf_parquet(gen_dataset)));
  const auto gen_table = parquet_read_table(leaf_parquet_stem(gen_dataset));
  REQUIRE(gen_table.has_value());
  CHECK(long_uid_values(*gen_table, 1).empty());

  const auto thermal_col = long_uid_values(*gen_table, 2);
  REQUIRE(thermal_col.size() == 2);
  for (const auto v : thermal_col) {
    CHECK(v == doctest::Approx(50.0));
  }

  // Waterway/flow_sol parquet: the waterway is not elided (fmax=100), but with
  // the turbine offline the optimal flow is 0 in every block.  In long output
  // those zeros are dropped, so uid 1 has no flow rows.
  const auto ww_dataset = outdir / "Waterway" / "flow_sol.parquet";
  const auto ww_pq = leaf_parquet(ww_dataset);
  if (std::filesystem::exists(ww_pq)) {
    const auto ww_table = parquet_read_table(leaf_parquet_stem(ww_dataset));
    REQUIRE(ww_table.has_value());
    CHECK(long_uid_values(*ww_table, 1).empty());
  }

  std::filesystem::remove_all(outdir);
}
