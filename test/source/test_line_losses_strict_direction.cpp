// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_strict_direction.cpp — pure-LP strict directional
// flow via a tiny ε cost on the per-direction loss columns.
//
// Background
// ----------
// After commits ``bb80284e1`` (piecewise → bidirectional wrapper) and
// ``de847646a`` (adaptive default flip), the LP-relax still admits
// residual phantom bidirectional flow due to LP DEGENERACY: in
// ``bidirectional`` / ``piecewise`` modes the per-direction loss
// columns ``loss_p`` / ``loss_n`` are convex in their own direction's
// flow, but among the SET of primal-optimal solutions sharing the same
// net dispatch the LP is free to pick ANY ``(fp, fn)`` pair.
//
// The fix is mathematically simple: stamp a tiny positive cost
// ``ε > 0`` on each per-direction loss column.  For any required net
// flow ``f = fp − fn ≥ 0`` the convex PWL of the loss curve is
// STRICTLY minimised at ``fn = 0`` (single-direction dispatch).
// Adding ``ε`` on both loss columns makes the LP STRICTLY pick that
// single-direction optimum.
//
// ``ε`` should be small (recommended ``1e-6`` $/MWh) so the objective
// impact is well below LP optimality tolerance (~1e-9 relative); the
// LP solver doesn't care about the magnitude — it just breaks the
// degeneracy.
//
// This file pins three invariants:
//   1) DEFAULT (``loss_cost_eps = 0``) — loss columns carry ``cost = 0``
//      in the LP matrix (byte-identical to legacy behaviour).
//   2) ENABLED (``loss_cost_eps = 1e-6``) — loss columns carry the
//      block-scaled ε cost on EVERY per-direction loss column.
//   3) PHANTOM-FREE (``loss_cost_eps = 1e-6``) — on the 3-bus loop
//      fixture, EVERY (line, block) pair has ``fp == 0`` OR ``fn == 0``
//      (no phantom bidirectional flow).

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

namespace strict_direction_test
{

// ─── Fixture: 3-bus loop with cheap surplus on bA, expensive backup ───
//
// Same topology as ``test_line_losses_no_phantom_flow.cpp`` but the
// generators and demand are scaled so the LP has BOTH detour and
// direct path active simultaneously — the regime where phantom flow
// was historically expressed.

[[nodiscard]] auto build_three_bus_loop(std::string_view mode_name,
                                        int loss_segments,
                                        OptReal loss_cost_eps)
    -> std::tuple<System, Simulation>
{
  Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bA",
      },
      {
          .uid = Uid {2},
          .name = "bB",
      },
      {
          .uid = Uid {3},
          .name = "bC",
      },
  };

  Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gA",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 1000.0,
      },
      {
          .uid = Uid {2},
          .name = "gC_backup",
          .bus = Uid {3},
          .gcost = 500.0,
          .capacity = 200.0,
      },
  };

  Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "dB",
          .bus = Uid {2},
          .capacity = 300.0,
      },
  };

  Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "line1_AB",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .line_losses_mode = OptName {std::string(mode_name)},
          .loss_segments = loss_segments,
          .tmax_ba = 150.0,
          .tmax_ab = 150.0,
          .loss_cost_eps = loss_cost_eps,
          .capacity = 150.0,
      },
      {
          .uid = Uid {2},
          .name = "line2_AC",
          .bus_a = Uid {1},
          .bus_b = Uid {3},
          .voltage = 100.0,
          .resistance = 0.01,
          .line_losses_mode = OptName {std::string(mode_name)},
          .loss_segments = loss_segments,
          .tmax_ba = 400.0,
          .tmax_ab = 400.0,
          .loss_cost_eps = loss_cost_eps,
          .capacity = 400.0,
      },
      {
          .uid = Uid {3},
          .name = "line3_CB",
          .bus_a = Uid {3},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .line_losses_mode = OptName {std::string(mode_name)},
          .loss_segments = loss_segments,
          .tmax_ba = 400.0,
          .tmax_ab = 400.0,
          .loss_cost_eps = loss_cost_eps,
          .capacity = 400.0,
      },
  };

  System system = {
      .name = "StrictDirection3BusLoop",
      .bus_array = std::move(bus_array),
      .demand_array = std::move(demand_array),
      .generator_array = std::move(generator_array),
      .line_array = std::move(line_array),
  };

  Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  return {std::move(system), std::move(simulation)};
}

[[nodiscard]] auto build_options() -> PlanningOptions
{
  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;
  opts.lp_matrix_options.col_with_names = true;
  opts.lp_matrix_options.row_with_names = true;
  opts.lp_matrix_options.col_with_name_map = true;
  opts.lp_matrix_options.row_with_name_map = true;
  return opts;
}

[[nodiscard]] auto build_matrix_opts() -> LpMatrixOptions
{
  LpMatrixOptions bo;
  bo.col_with_names = true;
  bo.col_with_name_map = true;
  bo.row_with_names = true;
  bo.row_with_name_map = true;
  return bo;
}

/// Collect column indices whose name contains ``substr``.
[[nodiscard]] auto cols_containing(const LinearInterface& li,
                                   std::string_view substr)
    -> std::vector<ColIndex>
{
  std::vector<ColIndex> out;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      out.push_back(idx);
    }
  }
  return out;
}

// ─── Tests ────────────────────────────────────────────────────────────

TEST_CASE("loss_cost_eps default 0 keeps loss column costs at 0")  // NOLINT
{
  auto [system, simulation] = build_three_bus_loop("bidirectional",
                                                   /*loss_segments=*/3,
                                                   /*loss_cost_eps=*/ {});

  PlanningOptions opts = build_options();
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp, build_matrix_opts());

  auto& li = sys_lp.linear_interface();
  const auto costs = li.get_col_cost_raw();

  const auto lossp_cols = cols_containing(li, "line_lossp_");
  const auto lossn_cols = cols_containing(li, "line_lossn_");
  REQUIRE(!lossp_cols.empty());
  REQUIRE(!lossn_cols.empty());

  for (const auto& idx : lossp_cols) {
    const double cost = costs[value_of(idx)];
    CAPTURE(value_of(idx));
    CHECK(cost == doctest::Approx(0.0));
  }
  for (const auto& idx : lossn_cols) {
    const double cost = costs[value_of(idx)];
    CAPTURE(value_of(idx));
    CHECK(cost == doctest::Approx(0.0));
  }
}

TEST_CASE("loss_cost_eps stamps per-direction loss columns")  // NOLINT
{
  constexpr double eps = 1e-6;
  auto [system, simulation] = build_three_bus_loop(
      "bidirectional", /*loss_segments=*/3, /*loss_cost_eps=*/OptReal {eps});

  PlanningOptions opts = build_options();
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp, build_matrix_opts());

  auto& li = sys_lp.linear_interface();
  const auto costs = li.get_col_cost_raw();

  const auto lossp_cols = cols_containing(li, "line_lossp_");
  const auto lossn_cols = cols_containing(li, "line_lossn_");
  REQUIRE(!lossp_cols.empty());
  REQUIRE(!lossn_cols.empty());

  // Sanity: 3 lines × 1 block × 1 scenario × 1 stage = 3 per dir
  CHECK(lossp_cols.size() == 3);
  CHECK(lossn_cols.size() == 3);

  // ``LinearProblem::flatten()`` divides every objective coefficient
  // by ``scale_objective = 1000``; the raw column cost is therefore
  // ``ε × duration / scale_objective = 1e-6 / 1000 = 1e-9``.
  constexpr double scale_objective = 1000.0;
  constexpr double expected = eps / scale_objective;
  for (const auto& idx : lossp_cols) {
    const double cost = costs[value_of(idx)];
    CAPTURE(value_of(idx));
    CHECK(cost == doctest::Approx(expected).epsilon(1e-3));
  }
  for (const auto& idx : lossn_cols) {
    const double cost = costs[value_of(idx)];
    CAPTURE(value_of(idx));
    CHECK(cost == doctest::Approx(expected).epsilon(1e-3));
  }
}

TEST_CASE("loss_cost_eps eliminates phantom bidirectional flow")  // NOLINT
{
  constexpr double EPS = 0.01;  // 0.01 MW phantom-flow tolerance
  auto [system, simulation] = build_three_bus_loop(
      "bidirectional", /*loss_segments=*/3, /*loss_cost_eps=*/OptReal {1e-6});

  PlanningOptions opts = build_options();
  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp, build_matrix_opts());

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();

  // Pair (flowp, flown) aggregators by stripping the directional infix.
  struct LineAggCol
  {
    std::string name;
    ColIndex idx;
  };

  std::vector<LineAggCol> fp_aggs;
  std::vector<LineAggCol> fn_aggs;
  for (const auto& [name, idx] : li.col_name_map()) {
    const bool is_seg = name.contains("_seg_");
    if (is_seg) {
      continue;
    }
    if (name.contains("line_flowp_")) {
      fp_aggs.push_back({
          .name = std::string(name),
          .idx = idx,
      });
    } else if (name.contains("line_flown_")) {
      fn_aggs.push_back({
          .name = std::string(name),
          .idx = idx,
      });
    }
  }

  REQUIRE(fp_aggs.size() == 3);
  REQUIRE(fn_aggs.size() == 3);

  const auto tail = [](std::string_view name) -> std::string
  {
    const auto pos = name.find("flowp_");
    if (pos != std::string_view::npos) {
      return std::string(name.substr(pos + 6));
    }
    const auto pos_n = name.find("flown_");
    return pos_n != std::string_view::npos ? std::string(name.substr(pos_n + 6))
                                           : std::string {};
  };

  int dispatched_lines = 0;
  int phantom_lines = 0;
  for (const auto& fp : fp_aggs) {
    const auto fp_tail = tail(fp.name);
    const auto fn_it = std::ranges::find_if(
        fn_aggs, [&](const auto& fn) { return tail(fn.name) == fp_tail; });
    REQUIRE(fn_it != fn_aggs.end());

    const double fp_val = sol[value_of(fp.idx)];
    const double fn_val = sol[value_of(fn_it->idx)];
    CAPTURE(fp.name);
    CAPTURE(fp_val);
    CAPTURE(fn_val);
    if (fp_val > EPS || fn_val > EPS) {
      ++dispatched_lines;
    }
    CHECK((fp_val <= EPS || fn_val <= EPS));
    if (fp_val > EPS && fn_val > EPS) {
      ++phantom_lines;
    }
  }

  CHECK(dispatched_lines >= 2);
  CHECK(phantom_lines == 0);
}

}  // namespace strict_direction_test

}  // namespace
