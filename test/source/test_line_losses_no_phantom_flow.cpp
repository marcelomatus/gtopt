// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_no_phantom_flow.cpp — regression guard for the
// "phantom bidirectional flow" bug fixed in commit bb80284e1.
//
// Background
// ----------
// Before bb80284e1, the legacy ``piecewise`` mode used a SINGLE shared
// ``loss`` column over ``fp + fn`` with two link rows:
//
//   linking:    fp + fn − Σ seg_k = 0
//   loss-track: loss − Σ loss_k · seg_k = 0
//
// Nothing in the LP forced ``fp · fn == 0``.  On meshed networks with
// negative-LMP receivers, the LP-relax discovered it could profit by
// setting BOTH directions nonzero on the same line in the same block —
// the bus balance correctly used the net ``fp − fn``, but the PWL input
// was the sum ``fp + fn``, so inflating the sum let the LP dump
// must-dispatch surplus into a fictitious loss variable that had no
// objective cost.
//
// Empirical scope on CEN PCP v0407 LP-relax (midpoint K=5, original
// piecewise mode):
//   * 78 of 79 dispatched lines (99 %) carried both directions > 0.5 MW
//     in > 50 % of their active blocks
//   * 220 GWh of fictitious bidirectional "waste" energy system-wide
//   * Reported ``loss_sol`` 130 GWh vs PLEXOS 20 GWh (×6.35)
//
// The fix folds ``piecewise`` into ``bidirectional`` for every
// non-``tangent`` layout: per-direction PWL ladders where the convex
// quadratic in each direction self-penalizes mutual flow.  The LP
// self-organizes to single-direction without needing complementarity,
// binaries, or SOS1 (still pure LP).
//
// This file pins three invariants of the fix:
//   1) PRIMARY — for every (line, block) pair, NOT both fp > eps AND
//      fn > eps simultaneously.
//   2) STRUCTURAL — ``piecewise`` and ``bidirectional`` produce
//      identical LP column and row counts on the same fixture.
//   3) REGRESSION — ``piecewise`` and ``bidirectional`` produce
//      byte-identical objective values on the same fixture.

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

namespace no_phantom_flow_test
{

// ─── Fixture A: 2-bus, used for structural + objective parity ─────────
//
// Identical to the canonical fixture in test_line_losses.cpp, but kept
// local so this file is self-contained and can be read in isolation.

struct TwoBusFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit TwoBusFixture(std::string_view mode_name,
                         int loss_segments = 3,
                         double R = 0.01,
                         double V = 100.0,
                         double tmax = 200.0)
      : system {
            .name = "PhantomFlow2Bus",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                },
            .demand_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {2},
                        .capacity = 100.0,
                    },
                },
            .generator_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "g1",
                        .bus = Uid {1},
                        .gcost = 10.0,
                        .capacity = 500.0,
                    },
                },
            .line_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "l1",
                        .bus_a = Uid {1},
                        .bus_b = Uid {2},
                        .voltage = V,
                        .resistance = R,
                        .line_losses_mode =
                            OptName {std::string(mode_name)},
                        .loss_segments = loss_segments,
                        .tmax_ba = tmax,
                        .tmax_ab = tmax,
                        .capacity = tmax,
                    },
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options())
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  PlanningOptionsLP make_options()
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    return bo;
  }
};

// ─── Fixture B: 3-bus loop, used for phantom-flow assertion ───────────
//
//   bus_A (cheap gen, surplus capability)
//    │
//    ├─ line1 (R>0) ── bus_B (load)
//    │                    │
//    └─ line2 (R>0) ── bus_C ── line3 ── bus_B
//
// Three lines form the loop A-B-C-A.  With ``use_kirchhoff = false``
// the LP has no KVL constraint linking the three lines, so each line
// is a free transport arc.  Under the OLD piecewise formulation, any
// line where dual pressure favoured both directions could host
// phantom flow.  The fix removes that degree of freedom on EVERY line.

[[nodiscard]] auto build_three_bus_loop(std::string_view mode_name,
                                        int loss_segments)
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

  // Cheap surplus-capable generator on bA, expensive backup on bC so
  // that any free-loss arbitrage at the *receiver* side could be
  // exploited.  Generator on bC priced just below ``demand_fail_cost``
  // so curtailment is not preferred over either backup gen or fictitious
  // loss; this is the worst case for the old phantom-flow bug.
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

  // Three lossy lines forming the loop A-B / A-C / C-B.
  // line1 (A→B) capacity 150 — capacity-binding on the direct path
  //   to force some flow through the A-C-B detour.
  // line2 (A→C) capacity 400 — wide enough to host phantom flow.
  // line3 (C→B) capacity 400 — wide enough to host phantom flow.
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
          .capacity = 400.0,
      },
  };

  System system = {
      .name = "PhantomFlow3BusLoop",
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

// ─── Helpers ──────────────────────────────────────────────────────────

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

/// Collect column indices whose name contains ``substr`` but NOT
/// ``exclude``.  Used to isolate the per-direction aggregator column
/// (``line_flowp_<uid>_...``) from its K segment children
/// (``line_flowp_seg_<k>_<uid>_...``).
[[nodiscard]] auto cols_containing(const LinearInterface& li,
                                   std::string_view substr,
                                   std::string_view exclude)
    -> std::vector<ColIndex>
{
  std::vector<ColIndex> out;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr) && !name.contains(exclude)) {
      out.push_back(idx);
    }
  }
  return out;
}

// ─── Tests ────────────────────────────────────────────────────────────

TEST_CASE(
    "piecewise mode does not produce phantom bidirectional flow")  // NOLINT
{
  // Tolerance: 0.01 MW.  Solver primal feasibility is ~1e-7, so
  // anything above 0.01 MW on BOTH directions is a true positive for
  // phantom flow (and historically the bug produced hundreds of MW
  // per block).
  constexpr double EPS = 0.01;

  SUBCASE("piecewise + 2-bus: only forward direction is dispatched")
  {
    // Pure unidirectional case: gen at bA, demand at bB.  Even under
    // the old single-shared-loss bug a 2-bus radial line had no
    // phantom-flow incentive (no closed loop, no negative-LMP
    // receiver), so this case ALSO passed pre-fix.  It is included
    // here as the easiest baseline: post-fix it must still pass.
    TwoBusFixture fix("piecewise", /*loss_segments=*/3);
    auto& li = fix.lp();
    auto result = li.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    const auto sol = li.get_col_sol_raw();
    const auto flowp_cols = cols_containing(li, "line_flowp_");
    const auto flown_cols = cols_containing(li, "line_flown_");

    // The fix introduces per-direction segment cols (and a per-line
    // aggregator), so we expect 1 aggregator + K segments = 4 in each
    // direction.  We do NOT depend on that exact count here — only on
    // the no-phantom-flow invariant against the aggregator cols.
    const auto fp = cols_containing(li, "line_flowp_", "_seg_");
    const auto fn = cols_containing(li, "line_flown_", "_seg_");
    REQUIRE(fp.size() == 1);
    REQUIRE(fn.size() == 1);

    const double fp_val = sol[value_of(fp.front())];
    const double fn_val = sol[value_of(fn.front())];
    CAPTURE(fp_val);
    CAPTURE(fn_val);
    // forward should carry ~100 MW + loss; reverse must be ≈ 0.
    CHECK(fp_val > 50.0);
    CHECK(fn_val < EPS);
    CHECK((fp_val <= EPS || fn_val <= EPS));
  }

  SUBCASE("piecewise + 3-bus loop: every line is single-direction")
  {
    // A 3-bus loop fixture creates the topological precondition that
    // exposed the pre-fix bug on CEN PCP v0407 — the LP can split
    // 300 MW of demand between the direct A→B path and the detour
    // A→C→B without any KVL constraint to pin a unique split.  Under
    // the OLD piecewise formulation, on any single line the LP could
    // freely add a "phantom" reverse component to inflate the
    // shared-loss column.  Under the FIX, each line's two-direction
    // ladders self-penalize, so the LP picks one direction per line.
    auto [system, simulation] =
        build_three_bus_loop("piecewise", /*loss_segments=*/3);

    PlanningOptions opts;
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;

    const PlanningOptionsLP options(opts);
    SimulationLP sim_lp(simulation, options);
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    SystemLP sys_lp(system, sim_lp, bo);

    auto& li = sys_lp.linear_interface();
    auto result = li.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    const auto sol = li.get_col_sol_raw();

    // Pair (flowp aggregator, flown aggregator) by line uid by
    // matching on the trailing "_<uid>_" segment in the name.  We
    // cannot rely on cols_containing's overload because we need both
    // directions for the SAME line.
    struct LineAggCol
    {
      std::string name;
      ColIndex idx;
      Uid uid;
    };

    std::vector<LineAggCol> fp_aggs;
    std::vector<LineAggCol> fn_aggs;
    for (const auto& [name, idx] : li.col_name_map()) {
      const bool is_seg = name.contains("_seg_");
      if (is_seg) {
        continue;
      }
      if (name.contains("line_flowp_")) {
        // Extract the trailing uid token: line names follow the
        // pattern `line_flowp_<line_uid>_<scn>_<stg>_<blk>`; we just
        // need a stable per-line identifier, which the full string
        // (excluding the directional prefix) provides.
        fp_aggs.push_back(
            {.name = std::string(name), .idx = idx, .uid = Uid {0}});
      } else if (name.contains("line_flown_")) {
        fn_aggs.push_back(
            {.name = std::string(name), .idx = idx, .uid = Uid {0}});
      }
    }

    // 3 lines × 1 block × 1 scenario × 1 stage = 3 aggregator cols
    // per direction.
    REQUIRE(fp_aggs.size() == 3);
    REQUIRE(fn_aggs.size() == 3);

    // Match by stripping the directional infix so the "tail" (uid +
    // scn + stg + blk) is comparable.
    const auto tail = [](std::string_view name) -> std::string
    {
      const auto pos = name.find("flowp_");
      if (pos != std::string_view::npos) {
        return std::string(name.substr(pos + 6));
      }
      const auto pos_n = name.find("flown_");
      return pos_n != std::string_view::npos
          ? std::string(name.substr(pos_n + 6))
          : std::string {};
    };

    int phantom_lines = 0;
    int dispatched_lines = 0;
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
      if (fp_val > EPS && fn_val > EPS) {
        ++phantom_lines;
        // PRIMARY assertion — single-line failure mode.
        FAIL("phantom flow detected on " << fp.name << ": fp=" << fp_val
                                         << " fn=" << fn_val);
      }
    }

    // Sanity: the demand is 300 MW so at least the A→B path and the
    // detour both carry flow.  Without dispatch this whole test
    // proves nothing.
    CHECK(dispatched_lines >= 2);
    CHECK(phantom_lines == 0);
  }

  SUBCASE("piecewise and bidirectional produce identical LP shape")
  {
    // STRUCTURAL invariant: after bb80284e1, `piecewise` is a thin
    // wrapper around `bidirectional` for every non-`tangent` layout,
    // so the two modes must produce the same LP column and row
    // counts.  This pins the wrapper.
    TwoBusFixture fix_pw("piecewise", /*loss_segments=*/3);
    TwoBusFixture fix_bi("bidirectional", /*loss_segments=*/3);
    auto& li_pw = fix_pw.lp();
    auto& li_bi = fix_bi.lp();

    CHECK(li_pw.get_numcols() == li_bi.get_numcols());
    CHECK(li_pw.get_numrows() == li_bi.get_numrows());

    // Per-category counts (these are the categories add_bidirectional
    // creates; if piecewise ever stops wrapping bidirectional, the
    // mismatch here pinpoints which category drifted).
    for (const std::string_view category : {"line_flowp_",
                                            "line_flown_",
                                            "line_flowp_seg_",
                                            "line_flown_seg_",
                                            "line_lossp_",
                                            "line_lossn_"})
    {
      CAPTURE(category);
      CHECK(cols_containing(li_pw, category).size()
            == cols_containing(li_bi, category).size());
    }
    for (const std::string_view category : {"line_flowp_link_",
                                            "line_flown_link_",
                                            "line_lossp_link_",
                                            "line_lossn_link_"})
    {
      CAPTURE(category);
      // Row counts via the same containment query, on row_name_map.
      const auto count = [&](const LinearInterface& li) -> std::size_t
      {
        std::size_t c = 0;
        for (const auto& [name, _idx] : li.row_name_map()) {
          if (name.contains(category)) {
            ++c;
          }
        }
        return c;
      };
      CHECK(count(li_pw) == count(li_bi));
    }
  }

  SUBCASE("piecewise and bidirectional produce identical objective")
  {
    // REGRESSION invariant from the bb80284e1 commit message:
    // "piecewise and bidirectional now byte-identical in obj + flow
    //  output".  Verify on the 2-bus fixture (deterministic, no
    //  multiple optima to confuse the comparison).
    TwoBusFixture fix_pw("piecewise", /*loss_segments=*/3);
    TwoBusFixture fix_bi("bidirectional", /*loss_segments=*/3);

    auto& li_pw = fix_pw.lp();
    auto& li_bi = fix_bi.lp();

    auto r_pw = li_pw.resolve();
    auto r_bi = li_bi.resolve();
    REQUIRE(r_pw.has_value());
    REQUIRE(r_bi.has_value());
    REQUIRE(r_pw.value() == 0);
    REQUIRE(r_bi.value() == 0);

    const double obj_pw = li_pw.get_obj_value_raw();
    const double obj_bi = li_bi.get_obj_value_raw();
    CAPTURE(obj_pw);
    CAPTURE(obj_bi);
    // Same LP, same solver, same cold start → byte-identical
    // expected, but allow a 1e-9 relative slack against possible
    // re-ordering of equivalent rows that some backends apply.
    CHECK(obj_pw == doctest::Approx(obj_bi).epsilon(1e-9));
  }
}

}  // namespace no_phantom_flow_test

}  // namespace
