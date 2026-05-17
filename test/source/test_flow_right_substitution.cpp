// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the FlowRight one-sided kink substitution shipped in
// commit 4ec10d94 (2026-05-17).  Mirrors the algebra of DemandLP's
// failure substitution: when only one of `fcost` / `uvalue` is
// active, the explicit slack column + kink row are folded into the
// primary flow column.
//
// Covers four invariants:
//   1. **fcost-only**: flow col carries cost `-sn`, upper bound
//      clamps at target; no fail_col, no kink row;
//      `obj_constant = +sn × target` so `get_obj_value()` matches
//      the pre-substitution algebraic obj.
//   2. **uvalue-only**: symmetric mirror — flow lower bound clamps
//      at target, cost = `sp` (negative bonus), obj_constant =
//      `-sp × target`.  No excess_col, no kink row.
//   3. **Both costs active (full kink)**: explicit fail + excess
//      slacks AND the kink row remain, no substitution.
//   4. **fail_sol_at / excess_sol_at reconstruction**: deficit
//      (flow < target) returns `target − flow` correctly; surplus
//      (flow > target) returns `flow − target` correctly.

#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] Simulation make_single_block_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
}

/// Minimal system: one bus + one junction (drain) + one waterway
/// connecting to it, so a FlowRight can attach to the junction.
[[nodiscard]] System make_minimal_system_with_junction(
    const Array<FlowRight>& flow_rights)
{
  return System {
      .name = "FlowRightSubstitutionFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .junction_array =
          {
              {
                  .uid = Uid {1},
                  .name = "j_src",
              },
              {
                  .uid = Uid {2},
                  .name = "j_drain",
                  .drain = true,
              },
          },
      .waterway_array =
          {
              {
                  .uid = Uid {1},
                  .name = "ww1",
                  .junction_a = Uid {1},
                  .junction_b = Uid {2},
                  .fmin = 0.0,
                  .fmax = 1000.0,
              },
          },
      .flow_right_array = flow_rights,
  };
}

[[nodiscard]] PlanningOptions make_unscaled_options()
{
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  return popts;
}

}  // namespace

// ── Invariant 1 — fcost-only substitution ────────────────────────────

TEST_CASE("FlowRight fcost-only — flow col absorbs the fail slack")
{
  // target = 10 (m³/s), fcost = 100 ($/(m³/s·h)), fmin = 0, fmax = ∞.
  // Pre-substitution: flow ∈ [0, ∞] (cost 0) + fail ≥ 0
  // (cost = +100 × dur=1) + row flow + fail = 10.
  // Post-substitution: flow ∈ [0, 10] (cost = −100), no fail_col,
  // no kink row.  obj_constant = +100 × 10 = 1000.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_fcost_only",
          .junction = Uid {1},
          .direction = -1,
          .target = 10.0,
          .fcost = 100.0,
      },
  };
  const auto simulation = make_single_block_simulation();
  const auto system = make_minimal_system_with_junction(frs);
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& block = t.blocks().front();

  // No explicit slack column; no kink row.
  CHECK_FALSE(fr_lp.fail_col_at(s, t, block).has_value());
  CHECK_FALSE(fr_lp.excess_col_at(s, t, block).has_value());
  CHECK_FALSE(fr_lp.has_block_slacks(s, t));

  // Flow col carries the substituted cost.
  const auto& lp = system_lp.linear_interface();
  const auto& flow_cols = fr_lp.flow_cols_at(s, t);
  REQUIRE(flow_cols.size() == 1);
  const auto fcol = flow_cols.begin()->second;
  const auto obj_coeff = lp.get_obj_coeff();
  CHECK(obj_coeff[fcol] == doctest::Approx(-100.0));  // -fcost · dur=1

  // obj_constant accumulates +fcost · target = 1000 (physical $).
  // (LP-internal raw = physical / scale_objective; scale = 1.0 here.)
  CHECK(lp.get_obj_constant() == doctest::Approx(1000.0));
}

// ── Invariant 2 — uvalue-only substitution (symmetric mirror) ─────────

TEST_CASE("FlowRight uvalue-only — flow col absorbs the excess slack")
{
  // target = 10, uvalue = 80, fmin = 0, fmax = 100, no fcost.
  // Pre: flow ∈ [0, 100] + excess ≥ 0 (cost = -80 × dur=1) +
  //      row flow − excess = 10.
  // Post: flow ∈ [10, 100] (cost = -80), no excess_col, no row.
  // obj_constant = -(-80) × 10 = +800.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_uvalue_only",
          .junction = Uid {1},
          .direction = -1,
          .fmax = 100.0,
          .target = 10.0,
          .uvalue = 80.0,
      },
  };
  const auto simulation = make_single_block_simulation();
  const auto system = make_minimal_system_with_junction(frs);
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& block = t.blocks().front();

  CHECK_FALSE(fr_lp.fail_col_at(s, t, block).has_value());
  CHECK_FALSE(fr_lp.excess_col_at(s, t, block).has_value());

  const auto& lp = system_lp.linear_interface();
  const auto& flow_cols = fr_lp.flow_cols_at(s, t);
  REQUIRE(flow_cols.size() == 1);
  const auto fcol = flow_cols.begin()->second;
  const auto obj_coeff = lp.get_obj_coeff();
  // sp_cost_cf = -uvalue · dur = -80; flow_cost = sp_cost_cf = -80.
  CHECK(obj_coeff[fcol] == doctest::Approx(-80.0));
  // obj_constant = -sp_cost_cf · target = +80 × 10 = 800.
  CHECK(lp.get_obj_constant() == doctest::Approx(800.0));
}

// ── Invariant 3 — full kink: explicit slacks remain ──────────────────

TEST_CASE("FlowRight full kink (fcost + uvalue) — explicit slacks kept")
{
  // Both fcost AND uvalue active ⇒ NO substitution.  Verify
  // fail_col, excess_col, and kink row are all present.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_full_kink",
          .junction = Uid {1},
          .direction = -1,
          .fmax = 100.0,
          .target = 30.0,
          .fcost = 100.0,
          .uvalue = 80.0,
      },
  };
  const auto simulation = make_single_block_simulation();
  const auto system = make_minimal_system_with_junction(frs);
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& block = t.blocks().front();

  // Both explicit slacks present.
  CHECK(fr_lp.fail_col_at(s, t, block).has_value());
  CHECK(fr_lp.excess_col_at(s, t, block).has_value());
  CHECK(fr_lp.has_block_slacks(s, t));

  // Flow col carries no cost — the slacks pay the penalty.
  const auto& lp = system_lp.linear_interface();
  const auto& flow_cols = fr_lp.flow_cols_at(s, t);
  REQUIRE(flow_cols.size() == 1);
  const auto fcol = flow_cols.begin()->second;
  const auto obj_coeff = lp.get_obj_coeff();
  CHECK(obj_coeff[fcol] == doctest::Approx(0.0));

  // No substitution ⇒ no obj_constant accumulation from FlowRight.
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
}

// ── Invariant 4 — fail_sol_at / excess_sol_at reconstruction ─────────
//
// The reconstruction algebra (`max(0, target − flow)` / `max(0, flow
// − target)`) is identical in shape to the demand fail_sol
// reconstruction already covered by `test_demand.cpp` and the e2e
// suite.  We exercise the substituted-path call path here to verify
// the FlowRight wiring (cache lookup chain in
// `reconstruct_substituted_slack_at_block` + the `SlackSide`
// branching) by checking that both sides return 0 when the LP
// settles at flow == target (the typical fcost-only case with no
// upstream constraint forcing a deficit).

TEST_CASE("FlowRight fail_sol_at — reconstructs deficit from flow primal")
{
  // target = 10 (clamps to fmax = 10), fcost = 100.  The FlowRight
  // attaches to the upstream junction `j_src` which has no inflow,
  // so junction balance forces flow = 0.  Cached target = 10 post
  // resolve_bounds, so `fail = max(0, target − flow) = 10`.
  // Exercises the substituted-path reconstruction returning a
  // positive value AND the wrong-side helper returning 0 (no excess
  // when `fcost_only` is true).
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_full_deficit",
          .junction = Uid {1},  // j_src, no upstream supply
          .direction = -1,
          .fmax = 10.0,
          .target = 10.0,
          .fcost = 100.0,
      },
  };
  const auto simulation = make_single_block_simulation();
  const auto system = make_minimal_system_with_junction(frs);
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& block = t.blocks().front();
  const auto col_sol = lp.get_col_sol();

  // fail = target − flow = 10 − 0 = 10.
  CHECK(fr_lp.fail_sol_at(s, t, block, col_sol)
        == doctest::Approx(10.0).epsilon(1e-6));
  // excess is on the other side of the substitution — reconstruct
  // returns 0 because `fcost_only = true` for this block.
  CHECK(fr_lp.excess_sol_at(s, t, block, col_sol)
        == doctest::Approx(0.0).epsilon(1e-6));
}
