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

using namespace gtopt;

namespace
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
          .junction_a = Uid {1},
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
          .junction_a = Uid {1},
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
          .junction_a = Uid {1},
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

TEST_CASE("FlowRight full kink with uvalue > fcost stays bounded")  // NOLINT
{
  // Regression: a two-sided kink whose excess REWARD (uvalue) exceeds the
  // shortfall PENALTY (fcost) used to be UNBOUNDED — because the kink
  // `flow − excess + fail = target` lets the solver inflate excess_col and
  // fail_col together (flow fixed ⇒ Δexcess = Δfail) for a net per-unit
  // cost of `fcost − uvalue < 0`.  The slacks are now capped at their
  // physical maxima (excess ≤ fmax − target, fail ≤ target − fmin), so the
  // LP is bounded and solves to optimality.  Mirrors the real Maule
  // water-right `maule_gasto_normal_riego` (uvalue=1100 > fcost=1000) that
  // made the full-network stage-1 LP unbounded.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_reward_gt_penalty",
          .junction_a = Uid {1},
          .direction = -1,
          .fmax = 100.0,
          .target = 30.0,
          .fcost = 100.0,
          .uvalue = 200.0,  // reward > penalty ⇒ previously unbounded
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

  // Full kink: both explicit slacks present (no one-sided substitution).
  REQUIRE(fr_lp.excess_col_at(s, t, block).has_value());
  REQUIRE(fr_lp.fail_col_at(s, t, block).has_value());

  // The slacks are now capped at their physical maxima, so the LP is
  // bounded and solves to optimality (was unbounded before the fix).
  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(lp.is_optimal());
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
          .junction_a = Uid {1},  // j_src, no upstream supply
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

TEST_CASE("FlowRight excess_sol_at — reconstructs surplus from flow primal")
{
  // Mirror of the fail_sol_at deficit test, for the uvalue-only
  // (soft-cap) substitution side.  target = 5, fmax = 10,
  // uvalue = 80.  The substitution makes
  //   flow.lowb = max(fmin, target) = 5
  //   flow.cost = sp_cost_cf = -80 · dur=1  (negative bonus)
  // so the LP picks flow at fmax = 10 to maximise the reward;
  // `excess = max(0, flow − target) = 5`.
  //
  // Run without a junction so the flow col has no bus-balance
  // constraint forcing it down — flow is then free to hit fmax.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_full_surplus",
          .direction = -1,
          .fmax = 10.0,
          .target = 5.0,
          .uvalue = 80.0,
      },
  };
  // Minimal system without a junction reference on the FlowRight.
  const System system {
      .name = "FlowRightUvalueOnlyFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .flow_right_array = frs,
  };
  const auto simulation = make_single_block_simulation();
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

  // excess = flow − target = 10 − 5 = 5.
  CHECK(fr_lp.excess_sol_at(s, t, block, col_sol)
        == doctest::Approx(5.0).epsilon(1e-6));
  // fail is on the other side of the substitution — reconstruct
  // returns 0 because `fcost_only = false` for this block.
  CHECK(fr_lp.fail_sol_at(s, t, block, col_sol)
        == doctest::Approx(0.0).epsilon(1e-6));
}

// ── Invariant 5 — stage-scope (qeh) substitution ─────────────────────
//
// `use_average = true` / `flow_mode = "stage_average"` moves the
// kink machinery from per-block to a single stage-level `qeh`
// column.  The same one-sided substitution applies: under fcost-only
// the qeh col absorbs the qeh_sn slack and the qkink row collapses
// into qeh.uppb + obj_constant.

TEST_CASE("FlowRight stage_average fcost-only — qeh col absorbs the kink")
{
  // stage-average mode, target=20, fcost=100, no uvalue.
  // attach_flow at stage scope substitutes qeh_sn + qkink_row:
  //   qeh.uppb shrinks to min(fmax, target) = 20
  //   qeh.cost = -fcost · stage_dur=1 = -100
  //   obj_constant += fcost · target = 2000
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_stage_fcost",
          .direction = -1,
          .fmax = 30.0,
          .target = 20.0,
          .flow_mode = "stage_average",
          .fcost = 100.0,
      },
  };
  // System without a junction reference — the qeh col stands alone
  // and the LP picks its primal at the upper bound.
  const System system {
      .name = "FlowRightStageAvgFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .flow_right_array = frs,
  };
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];

  // Structural assertions: qeh exists; qeh_sn / qkink elided.
  CHECK(fr_lp.has_qeh(s, t));
  CHECK_FALSE(fr_lp.has_qeh_slacks(s, t));
  CHECK_FALSE(fr_lp.has_qkink_row(s, t));

  auto& lp = system_lp.linear_interface();
  // qeh col cost should be -fcost · stage_duration=1 = -100.
  const auto obj_coeff = lp.get_obj_coeff();
  const auto qeh_col = fr_lp.qeh_col_at(s, t);
  CHECK(obj_coeff[qeh_col] == doctest::Approx(-100.0));
  // obj_constant = +fcost · target = 2000.
  CHECK(lp.get_obj_constant() == doctest::Approx(2000.0));
}

// ── Invariant 6 — target-0 usage-cost / reward encodings ─────────────
//
// PLP's Laja/Maule agreements price the rights FLOWS themselves:
// positive usage costs (CQVar(IQDR..IQGA), Invernada penalizadores) and
// rewards (ValorRiego).  gtopt encodes a per-unit flow price as a
// `target = 0` kink with a `uvalue` of the opposite sign: the
// uvalue-only substitution then folds the price into the flow column
// over its whole `[0, fmax]` range.  `resolve_bounds` must therefore
// KEEP a zero target when uvalue is active (it is only dropped when
// both slack costs are absent — the plain back-compat band).

TEST_CASE("FlowRight target-0 usage cost — negative uvalue prices the flow")
{
  // PLP: FO += 50 · qde (positive usage cost on the electric right).
  // gtopt encoding: target = 0, uvalue = -50 ⇒ uvalue-only
  // substitution with flow ∈ [0, fmax], cost = +50 · dur, no slacks,
  // obj_constant = 0.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_usage_cost",
          .direction = -1,
          .fmax = 100.0,
          .target = 0.0,
          .uvalue = -50.0,
      },
  };
  const System system {
      .name = "FlowRightUsageCostFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .flow_right_array = frs,
  };
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& block = t.blocks().front();

  // Substituted — no explicit slacks, no kink row.
  CHECK_FALSE(fr_lp.fail_col_at(s, t, block).has_value());
  CHECK_FALSE(fr_lp.excess_col_at(s, t, block).has_value());

  const auto& lp = system_lp.linear_interface();
  const auto& flow_cols = fr_lp.flow_cols_at(s, t);
  REQUIRE(flow_cols.size() == 1);
  const auto fcol = flow_cols.begin()->second;

  // The full band is preserved (NOT pinned to the zero target) and
  // the flow carries the positive per-unit price.
  CHECK(lp.get_col_low()[fcol] == doctest::Approx(0.0));
  CHECK(lp.get_col_upp()[fcol] == doctest::Approx(100.0));
  CHECK(lp.get_obj_coeff()[fcol] == doctest::Approx(50.0));
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
}

TEST_CASE("FlowRight target-0 reward — positive uvalue pays the flow")
{
  // PLP: FO -= 80 · qter (reward on delivered irrigation).  gtopt
  // encoding: target = 0, uvalue = +80 ⇒ flow ∈ [0, fmax] with cost
  // -80 · dur; the LP pushes the flow to fmax.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_usage_reward",
          .direction = -1,
          .fmax = 10.0,
          .target = 0.0,
          .uvalue = 80.0,
      },
  };
  const System system {
      .name = "FlowRightUsageRewardFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .flow_right_array = frs,
  };
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(lp.is_optimal());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto col_sol = lp.get_col_sol();
  const auto& flow_cols = fr_lp.flow_cols_at(s, t);
  REQUIRE(flow_cols.size() == 1);
  CHECK(col_sol[flow_cols.begin()->second] == doctest::Approx(10.0));
}

TEST_CASE("FlowRight target-0 with no costs — plain band back-compat")
{
  // The back-compat reset still applies when NEITHER fcost nor uvalue
  // is active: `target = 0` alone means "no soft requirement" and the
  // column is a plain hard band with no cost and no slacks.
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_plain_band",
          .direction = -1,
          .fmax = 100.0,
          .target = 0.0,
      },
  };
  const System system {
      .name = "FlowRightPlainBandFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .flow_right_array = frs,
  };
  const auto simulation = make_single_block_simulation();
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
  CHECK(lp.get_col_low()[fcol] == doctest::Approx(0.0));
  CHECK(lp.get_col_upp()[fcol] == doctest::Approx(100.0));
  CHECK(lp.get_obj_coeff()[fcol] == doctest::Approx(0.0));
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));
}

// ── Invariant 7 — bound-rule re-clamp preserves the substitution ─────
//
// A bound-rule tick re-clamps the flow bounds in `update_lp`.  The
// one-sided substitution folded a slack cost into the flow column
// whose algebra is only valid on the kink side of the CACHED target:
// under fcost-only the column carries cost `-sn` (every unit up to
// the target earns back the pre-paid `sn x target` obj constant), so
// re-opening `uppb` beyond the target would PAY the LP for phantom
// flow; the uvalue-only mirror would charge below the target.  The
// re-clamp must keep `uppb = min(fmax, target)` under fcost-only and
// `lowb = max(fmin, target)` under uvalue-only.
//
// One FlowRight per sub-case: editing LP bounds invalidates the
// solved state, so only the first element updated after a solve sees
// the moved average-volume axis — a shared fixture would leave the
// later elements untested.

namespace
{

// Backbone with a deterministic 25 m3/s hydro draw (50 MW demand,
// pf 2.0, free hydro vs 100 $/MWh thermal): after one solve the
// reservoir efin is 100 - 25*fcr*1h and the average volume moves off
// the build-time 100 hm3, so the 0.3 x V rule re-evaluates from 30
// to 0.3 x (100 + efin) / 2 and the re-clamp fires.
[[nodiscard]] System make_reclamp_system(const FlowRight& fr)
{
  return {
      .name = "FlowRightReclampFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .lmax = 50.0}},
      .generator_array =
          {
              {.uid = Uid {1},
               .name = "hydro",
               .bus = Uid {1},
               .gcost = 0.0,
               .capacity = 200.0},
              {.uid = Uid {2},
               .name = "thermal",
               .bus = Uid {1},
               .gcost = 100.0,
               .capacity = 200.0},
          },
      .junction_array =
          {
              {.uid = Uid {1}, .name = "j_up"},
              {.uid = Uid {2}, .name = "j_down", .drain = true},
          },
      .waterway_array = {{.uid = Uid {1},
                          .name = "ww",
                          .junction_a = Uid {1},
                          .junction_b = Uid {2},
                          .fmin = 0.0,
                          .fmax = 1000.0}},
      .reservoir_array = {{.uid = Uid {1},
                           .name = "rsv",
                           .junction = Uid {1},
                           .capacity = 200.0,
                           .emin = 0.0,
                           .emax = 200.0,
                           .eini = 100.0}},
      .turbine_array = {{.uid = Uid {1},
                         .name = "tur",
                         .waterway = Uid {1},
                         .generator = Uid {1},
                         .production_factor = 2.0}},
      .flow_right_array = {fr},
  };
}

[[nodiscard]] RightBoundRule make_reclamp_rule()
{
  return {
      .reservoir = Uid {1},
      .segments = {{.volume = 0.0, .slope = 0.3, .constant = 0.0}},
  };
}

constexpr double kReclampNewFmax =
    0.3 * ((100.0 + (100.0 - 25.0 * 0.0036)) / 2.0);

}  // namespace

TEST_CASE("FlowRight re-clamp keeps fcost-only uppb at the cached target")
{
  const FlowRight fr {.uid = Uid {1},
                      .name = "fr_fc",
                      .fmax = 50.0,
                      .target = 10.0,
                      .fcost = 100.0,
                      .bound_rule = make_reclamp_rule()};
  const auto system = make_reclamp_system(fr);
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto buid = t.blocks().front().uid();
  auto& li = system_lp.linear_interface();
  const auto fcol =
      system_lp.elements<FlowRightLP>().front().flow_cols_at(s, t).at(buid);

  // Build-time substitution clamp: uppb = min(min(50, 30), 10) = 10.
  CHECK(li.get_col_upp()[fcol] == doctest::Approx(10.0));

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(li.is_optimal());
  CHECK(system_lp.update_lp() > 0);

  // Rule moved 30 -> ~29.99 but the target clamp must survive.
  CHECK(li.get_col_upp()[fcol] == doctest::Approx(10.0));
  CHECK(li.get_col_low()[fcol] == doctest::Approx(0.0));
}

TEST_CASE("FlowRight re-clamp keeps uvalue-only lowb at the cached target")
{
  const FlowRight fr {.uid = Uid {1},
                      .name = "fr_uv",
                      .fmax = 50.0,
                      .target = 10.0,
                      .uvalue = 80.0,
                      .bound_rule = make_reclamp_rule()};
  const auto system = make_reclamp_system(fr);
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto buid = t.blocks().front().uid();
  auto& li = system_lp.linear_interface();
  const auto fcol =
      system_lp.elements<FlowRightLP>().front().flow_cols_at(s, t).at(buid);

  // Build: lowb = max(0, 10) = 10; uppb = min(50, rule 30) = 30.
  CHECK(li.get_col_low()[fcol] == doctest::Approx(10.0));
  CHECK(li.get_col_upp()[fcol] == doctest::Approx(30.0));

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(li.is_optimal());
  CHECK(system_lp.update_lp() > 0);

  // uppb follows the rule; lowb keeps the substitution clamp.
  CHECK(li.get_col_low()[fcol] == doctest::Approx(10.0));
  CHECK(li.get_col_upp()[fcol] == doctest::Approx(kReclampNewFmax));
}

TEST_CASE("FlowRight re-clamp keeps the qeh substitution clamp")
{
  const FlowRight fr {.uid = Uid {1},
                      .name = "fr_qeh",
                      .fmax = 50.0,
                      .target = 10.0,
                      .flow_mode = "stage_average",
                      .fcost = 100.0,
                      .bound_rule = make_reclamp_rule()};
  const auto system = make_reclamp_system(fr);
  const auto simulation = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  auto& li = system_lp.linear_interface();
  const auto qeh_col =
      system_lp.elements<FlowRightLP>().front().qeh_col_at(s, t);

  CHECK(li.get_col_upp()[qeh_col] == doctest::Approx(10.0));

  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(li.is_optimal());
  CHECK(system_lp.update_lp() > 0);

  CHECK(li.get_col_upp()[qeh_col] == doctest::Approx(10.0));
}
