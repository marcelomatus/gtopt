// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_flow_lp_reduction.cpp
 * @brief     Verify the fixed-Flow -> junction-balance-RHS fold under
 *            ``lp_reduction`` is dual-transparent (same objective + same
 *            balance dual as the explicit fixed-column form) and that it
 *            actually elides the flow column.
 */
#include <doctest/doctest.h>
#include <gtopt/flow_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Wrap in a uniquely-named namespace so unity-build batching of test files
// does not collide anonymous-namespace helpers.
namespace test_flow_lp_reduction
{

namespace
{

// Build the System once; only the lp_reduction flag varies per solve.
//
// Single bus, demand 50.  A fixed river inflow (discharge = 25) enters the
// NON-draining junction ``j_up`` and must route through a waterway to the
// draining ``j_down``; a turbine on that waterway (PF = 1) converts the
// 25 m3/s into 25 MW of cheap hydro (gcost 5), the remaining 25 MW is served
// by thermal (gcost 80).  The ``j_up`` balance dual is the marginal water
// value (≈ thermal − hydro), so it is a meaningful, non-degenerate dual to
// compare between the column form and the folded form.
struct SolveResult
{
  double obj {};
  std::vector<double> balance_duals;  // per block, for j_up
  std::size_t n_flow_cols {};
};

SolveResult solve(bool reduce)
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
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
          .drain = false,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 25.0,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  const Simulation simulation = {
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
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 2,
      }},
      .scenario_array = {{.uid = Uid {1}}},
  };

  const System system = {
      .name = "FlowReductionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.model_options.use_single_bus = true;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.model_options.lp_reduction = reduce;

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto& scene = sim_lp.scenes()[first_scene_index()];
  const auto& scenario = scene.scenarios()[0];
  const auto& phase = sim_lp.phases()[first_phase_index()];
  const auto& stage = phase.stages()[0];

  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& fcols = flow_lp.flow_cols_at(scenario, stage);

  // j_up is junction uid 1 → first JunctionLP element.
  const auto& junction_lp = sys_lp.elements<JunctionLP>()[0];
  const auto& brows = junction_lp.balance_rows_at(scenario, stage);

  const auto duals = li.get_row_dual();

  SolveResult out;
  out.obj = li.get_obj_value();
  out.n_flow_cols = fcols.size();
  for (const auto& [buid, row] : brows) {
    out.balance_duals.push_back(duals[static_cast<std::size_t>(row)]);
  }
  return out;
}

}  // namespace

TEST_CASE(
    "Fixed-Flow RHS fold under lp_reduction is dual-transparent")  // NOLINT
{
  const auto col_form = solve(/*reduce=*/false);
  const auto folded = solve(/*reduce=*/true);

  SUBCASE("column form emits the fixed flow column; folded form elides it")
  {
    CHECK(col_form.n_flow_cols == 2);  // one fixed column per block
    CHECK(folded.n_flow_cols == 0);  // folded into the balance RHS
  }

  SUBCASE("objective is identical")
  {
    CHECK(folded.obj == doctest::Approx(col_form.obj));
  }

  SUBCASE("junction balance duals are identical (dual-transparent)")
  {
    REQUIRE(folded.balance_duals.size() == col_form.balance_duals.size());
    for (std::size_t i = 0; i < col_form.balance_duals.size(); ++i) {
      CHECK(folded.balance_duals[i]
            == doctest::Approx(col_form.balance_duals[i]).epsilon(1e-6));
    }
  }
}

}  // namespace test_flow_lp_reduction
