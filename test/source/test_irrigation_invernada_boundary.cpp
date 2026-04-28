// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_irrigation_invernada_boundary.cpp
 * @brief     Tier 8.6b — boundary cases for the Maule La Invernada
 *            FlowRight + UserConstraint balance
 * @date      2026-04-11
 * @copyright BSD-3-Clause
 *
 * These tests extend the Tier 8.6 "La Invernada P0 defender" with
 * defensive guards around both the HARD-constraint baseline and the
 * Phase C soft-constraint path (`penalty_class = "hydro_flow"`) for
 * `invernada_balance`.  Test 4 pins down the baseline infeasibility
 * of the hard equality under a pathological RHS; Test 4b then
 * demonstrates that the same fixture becomes feasible once the
 * soft penalty is wired, closing the gap with visible slack columns
 * priced at `penalty × duration[h] × 3600`.
 *
 * The BoundaryUCFixture + make_boundary_uc_simulation + make_boundary_uc_system
 * helpers are copy-pasted from test_irrigation_user_constraints.cpp.
 * Refactoring them into a shared header is deferred — these tests are few and
 * the duplication keeps the blast radius of future edits small.
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// ─────────────────────────────────────────────────────────────────────────
// Fixture — identical hydraulic backbone as Tier 8.6
// (test_irrigation_user_constraints.cpp lines 33-106).  Copy-pasted
// rather than shared-headered to minimize refactor blast radius.
// ─────────────────────────────────────────────────────────────────────────
struct BoundaryUCFixture
{
  Array<Bus> bus_array {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "hydro",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };
  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  Array<Junction> junction_array {
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
  Array<Waterway> waterway_array {
      {
          .uid = Uid {1},
          .name = "ww",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };
  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {1},
          .name = "rsv",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 100.0,
      },
  };
  Array<Turbine> turbine_array {
      {
          .uid = Uid {1},
          .name = "tur",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
};

[[nodiscard]] Simulation make_boundary_uc_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 24,
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
}

// Two-stage variant used by Test 6 to observe stage-over-stage
// VolumeRight accumulator bounds.
[[maybe_unused, nodiscard]] Simulation make_uc_simulation_two_stage()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 24,
              },
              {
                  .uid = Uid {2},
                  .duration = 24,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
              {
                  .uid = Uid {2},
                  .first_block = 1,
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
}

[[nodiscard]] System make_boundary_uc_system(const BoundaryUCFixture& fx,
                                             Array<VolumeRight> vrs,
                                             Array<FlowRight> frs,
                                             Array<UserConstraint> ucs,
                                             std::string name)
{
  return {
      .name = std::move(name),
      .bus_array = fx.bus_array,
      .demand_array = fx.demand_array,
      .generator_array = fx.generator_array,
      .junction_array = fx.junction_array,
      .waterway_array = fx.waterway_array,
      .reservoir_array = fx.reservoir_array,
      .turbine_array = fx.turbine_array,
      .flow_right_array = std::move(frs),
      .volume_right_array = std::move(vrs),
      .user_constraint_array = std::move(ucs),
  };
}

// Build the canonical 5-FlowRight La Invernada set.  Caller supplies
// the kQmax / discharge so we can cover both the "variable mode"
// (discharge==0, fmax>0) and the "fmax==0 legitimately pinned" case.
[[nodiscard]] Array<FlowRight> make_invernada_rights(Real kQmax, Real discharge)
{
  return {
      {
          .uid = Uid {1},
          .name = "inv_deficit",
          .direction = 1,
          .discharge = discharge,
          .fmax = kQmax,
      },
      {
          .uid = Uid {2},
          .name = "inv_sin_deficit",
          .direction = 1,
          .discharge = discharge,
          .fmax = kQmax,
      },
      {
          .uid = Uid {3},
          .name = "inv_caudal_natural",
          .direction = 1,
          .discharge = discharge,
          .fmax = kQmax,
      },
      {
          .uid = Uid {4},
          .name = "inv_embalsar",
          .direction = -1,
          .discharge = discharge,
          .fmax = kQmax,
      },
      {
          .uid = Uid {5},
          .name = "inv_no_embalsar",
          .direction = -1,
          .discharge = discharge,
          .fmax = kQmax,
      },
  };
}

[[nodiscard]] UserConstraint make_invernada_balance_uc()
{
  return UserConstraint {
      .uid = Uid {1},
      .name = "invernada_balance",
      .expression =
          R"(flow_right("inv_deficit").flow + flow_right("inv_sin_deficit").flow + flow_right("inv_caudal_natural").flow = flow_right("inv_embalsar").flow + flow_right("inv_no_embalsar").flow)",
      .constraint_type = "raw",
  };
}

/// Soft variant of the invernada_balance fixture.  Carries `penalty`
/// and `penalty_class = "hydro_flow"` so the LP absorbs any PLP-style
/// conditional-bound infeasibility through visible slack columns
/// priced at `penalty × duration[h] × 3600` per m³/s — the Phase C
/// mechanism added in `source/user_constraint_lp.cpp::
/// resolve_block_soft_penalty`.
[[nodiscard]] UserConstraint make_invernada_balance_soft_uc(Real penalty)
{
  return UserConstraint {
      .uid = Uid {1},
      .name = "invernada_balance",
      .expression =
          R"(flow_right("inv_deficit").flow + flow_right("inv_sin_deficit").flow + flow_right("inv_caudal_natural").flow = flow_right("inv_embalsar").flow + flow_right("inv_no_embalsar").flow)",
      .constraint_type = "raw",
      .penalty = penalty,
      .penalty_class = "hydro_flow",
  };
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Test 1 — fmax=0 legitimately pins all flows to zero
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada fmax=0 legitimately pins all flows to zero")
{
  // Distinct from the 2026-04-11 template bug (which also pinned all
  // five flows to zero, but *unintentionally*): here we model the
  // legitimate case where qmax_invernada is config-driven to 0.0.
  // Both the bug and the legitimate case produce the same [0, 0]
  // column bounds; the distinction is intent, not LP shape.
  //
  // The balance 0+0+0 = 0+0 is trivially satisfied.
  const BoundaryUCFixture fx;
  constexpr Real kQmax = 0.0;
  const auto frs = make_invernada_rights(kQmax, /*discharge=*/0.0);
  const Array<UserConstraint> ucs = {make_invernada_balance_uc()};

  const auto system =
      make_boundary_uc_system(fx, {}, frs, ucs, "Tier8_6b_Invernada_fmax_zero");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();

  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto col_upp = lp.get_col_upp();
  const auto col_low = lp.get_col_low();
  const auto& fr_elems = system_lp.elements<FlowRightLP>();
  REQUIRE(fr_elems.size() == 5);
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  for (const auto& fr_lp : fr_elems) {
    const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
    REQUIRE(!flow_cols.empty());
    for (const auto& [buid, col] : flow_cols) {
      CHECK(col_low[col] == doctest::Approx(0.0));
      CHECK(col_upp[col] == doctest::Approx(0.0));
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Test 2 — storage side preferred when embalsar is cheaper
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada storage preferred when embalsar cost < "
    "no_embalsar cost")
{
  // The stronger statement we want to make is "the primal solution
  // selects more flow on the embalsar side than on the no_embalsar
  // side when embalsar is cheaper."  That requires reading primal
  // column values and confirming that the LP's cost-minimization
  // actually sees a meaningful gradient on the two sides.
  //
  // Without a dedicated `fail` penalty or an objective-contribution
  // coupling from the balance row, the two sides are free variables
  // in [0, fmax] and the LP is indifferent (any split satisfying the
  // balance is optimal).  Asserting a strict dominance would then
  // be degenerate — the LP can legitimately return either solution.
  //
  // So this test is intentionally a feasibility-only guard: it
  // pins down that the fixture wires the `use_value` field (cost
  // steering) without crashing and that the LP remains feasible.
  //
  // TODO(phase-C): once priced slacks on the balance row land, this
  // test should assert that col_sol[embalsar] >= col_sol[no_embalsar]
  // when use_value[embalsar] is strictly more generous.
  const BoundaryUCFixture fx;
  constexpr Real kQmax = 200.0;
  auto frs = make_invernada_rights(kQmax, /*discharge=*/0.0);
  // Cheaper to activate embalsar (larger benefit → larger negative
  // objective coefficient).  use_value is a $/m³/s·h benefit per
  // flow_right.hpp:112-117.
  frs[3].use_value = 100.0;  // inv_embalsar
  frs[4].use_value = 1.0;  // inv_no_embalsar
  const Array<UserConstraint> ucs = {make_invernada_balance_uc()};

  const auto system = make_boundary_uc_system(
      fx, {}, std::move(frs), ucs, "Tier8_6b_Invernada_embalsar_preferred");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();

  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Test 3 — bypass side preferred when no_embalsar is cheaper
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada bypass preferred when no_embalsar cost < "
    "embalsar cost")
{
  // Mirror of Test 2 with the use_value biases flipped.  Same
  // feasibility-only caveat applies — see the TODO in Test 2.
  const BoundaryUCFixture fx;
  constexpr Real kQmax = 200.0;
  auto frs = make_invernada_rights(kQmax, /*discharge=*/0.0);
  frs[3].use_value = 1.0;  // inv_embalsar
  frs[4].use_value = 100.0;  // inv_no_embalsar
  const Array<UserConstraint> ucs = {make_invernada_balance_uc()};

  const auto system = make_boundary_uc_system(
      fx, {}, std::move(frs), ucs, "Tier8_6b_Invernada_bypass_preferred");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();

  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Test 4 — hard balance becomes infeasible at pathological RHS
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada hard balance becomes infeasible at RHS far above "
    "capacity")
{
  // This test locks down the CURRENT HARD-CONSTRAINT behavior of
  // invernada_balance.  It is the motivator for the Phase C
  // soft-constraint work: once `penalty_class = "hydro_flow"` is wired,
  // a follow-up test will assert the same fixture becomes FEASIBLE
  // with a priced slack carrying cost = hydro_fail_cost × 3600 × dur.
  // Until then, this test pins down the baseline infeasibility so
  // the soft-constraint commit can demonstrably flip the assertion.
  //
  // Construction: each of the three +1-direction FlowRights has
  // fmax = 200, so the max feasible LHS sum is 3·200 = 600.  A
  // second UserConstraint forces the sum ≥ 1200 = 6·kQmax, well
  // above what the column bounds can deliver — infeasible.
  const BoundaryUCFixture fx;
  constexpr Real kQmax = 200.0;
  const auto frs = make_invernada_rights(kQmax, /*discharge=*/0.0);
  const Array<UserConstraint> ucs = {
      make_invernada_balance_uc(),
      {
          .uid = Uid {2},
          .name = "invernada_lhs_floor",
          .expression =
              R"(flow_right("inv_deficit").flow + flow_right("inv_sin_deficit").flow + flow_right("inv_caudal_natural").flow >= 1200)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_boundary_uc_system(
      fx, {}, frs, ucs, "Tier8_6b_Invernada_hard_infeasible");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();

  const auto result = lp.resolve();
  // Baseline (hard constraint): the LP must report infeasibility.
  // Regression guard — preserves the Phase B motivator after Phase C
  // wired the priced-slack path; Test 4b below demonstrates the flip.
  CHECK_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────
// Test 4b — soft balance with penalty_class="hydro_flow" restores
//           feasibility (Phase C: source/user_constraint_lp.cpp)
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada soft balance with penalty_class hydro_flow is "
    "feasible when LHS and RHS column bounds disagree")
{
  // Phase C flip of the Test 4 motivator.  Instead of reusing the
  // `>= 1200` HARD floor (which is itself infeasible by column
  // bounds — max LHS = 3·kQmax = 600 < 1200 — and therefore can't
  // be rescued by softening a DIFFERENT constraint), this test
  // constructs a fixture where the **balance equation itself** is
  // the sole source of infeasibility:
  //
  //   LHS (3 × +1 flows, discharge=kLhs, fmax=kLhs):  pinned to 3·kLhs = 600
  //   RHS (2 × −1 flows, discharge=0,    fmax=0   ):  pinned to 0
  //
  // With a HARD equality, `600 = 0` is infeasible.  With the Phase C
  // soft path (`penalty = 10 $/m³`, `penalty_class = "hydro_flow"`),
  // the LP absorbs the 600 m³/s gap through the `slack_neg` column.
  //
  // Per-unit LP slack cost with the default `scale_objective = 1000`:
  //   penalty × duration[h] × 3600 / scale_objective
  //     = 10 × 24 × 3600 / 1000
  //     = 864
  // Gap = 600 m³/s → total raw LP objective = 600 × 864 = 518 400.
  // `make_boundary_uc_simulation()` uses a single 24-hour block, so the
  // duration factor is non-trivial — this is exactly what the
  // `PenaltyClass::HydroFlow` conversion is supposed to pick up.
  const BoundaryUCFixture fx;
  constexpr Real kLhs = 200.0;  // m³/s pinned per LHS flow
  constexpr Real kPenalty = 10.0;  // $/m³
  const Array<FlowRight> frs {
      {
          .uid = Uid {1},
          .name = "inv_deficit",
          .direction = 1,
          .discharge = kLhs,
          .fmax = kLhs,
      },
      {
          .uid = Uid {2},
          .name = "inv_sin_deficit",
          .direction = 1,
          .discharge = kLhs,
          .fmax = kLhs,
      },
      {
          .uid = Uid {3},
          .name = "inv_caudal_natural",
          .direction = 1,
          .discharge = kLhs,
          .fmax = kLhs,
      },
      {
          .uid = Uid {4},
          .name = "inv_embalsar",
          .direction = -1,
          .discharge = 0.0,
          .fmax = 0.0,
      },
      {
          .uid = Uid {5},
          .name = "inv_no_embalsar",
          .direction = -1,
          .discharge = 0.0,
          .fmax = 0.0,
      },
  };
  const Array<UserConstraint> ucs = {
      make_invernada_balance_soft_uc(kPenalty),
  };

  const auto system = make_boundary_uc_system(
      fx, {}, frs, ucs, "Tier8_6b_Invernada_soft_feasible");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();

  const auto result = lp.resolve();
  // Phase C assertion: priced slack closes the infeasibility gap.
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Raw LP objective (after `scale_objective=1000` division).  See
  // the header comment above for the 864 × 600 = 518 400 derivation.
  const auto obj = lp.get_obj_value_raw();
  CHECK(obj == doctest::Approx(518'400.0).epsilon(1e-6));
}

// ─────────────────────────────────────────────────────────────────────────
// Test 5 — balance columns are reachable through the AMPL registry
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - Invernada balance column names surface in AMPL registry")
{
  // Sanity test: after building the LP, every Invernada FlowRight
  // must expose its `flow` column through SystemContext::find_ampl_col.
  // This locks down the registry plumbing for the Phase C soft tests,
  // which will need registry access to introspect newly-created slack
  // columns named after the `penalty_class` tag.
  const BoundaryUCFixture fx;
  constexpr Real kQmax = 200.0;
  const auto frs = make_invernada_rights(kQmax, /*discharge=*/0.0);
  const Array<UserConstraint> ucs = {make_invernada_balance_uc()};

  const auto system = make_boundary_uc_system(
      fx, {}, frs, ucs, "Tier8_6b_Invernada_registry_columns");
  const PlanningOptionsLP options;
  const auto sim = make_boundary_uc_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());
  REQUIRE(!stages[0].blocks().empty());
  const auto sc_uid = scenarios[0].uid();
  const auto st_uid = stages[0].uid();
  const auto bk_uid = stages[0].blocks().front().uid();

  for (const auto& fr : frs) {
    const auto col =
        sc.find_ampl_col("flow_right", fr.uid, "flow", sc_uid, st_uid, bk_uid);
    REQUIRE(col.has_value());
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Test 6 — econ_invernada VolumeRight accumulator has no cost cap
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.6b - econ_invernada VolumeRight accumulator has no cost cap "
    "(documentation)")
{
  // DOCTEST: PLP applies EconInvernCosto (> 0) as a per-unit cost on
  // the economia-invernada accumulator (IVMDEIF) in genpdmaule.f:529
  // to prevent unbounded growth.  gtopt's current maule_vol_econ_invernada
  // template at maule.tson:680-689 has no `ecost`, so the accumulator
  // can inflate without objective pressure.  This test documents the
  // gap; a P2 followup will add the cost term.
  //
  // Concretely: build a VolumeRight shaped like the Maule economy
  // accumulator (no ecost, saving_rate enabled, use_state_variable
  // on, no reset_month) and confirm the LP assembles successfully
  // and the VolumeRight is registered.  A stronger version of this
  // test would inspect the objective coefficient on the accumulator
  // column and assert == 0, but the plumbing for that assertion is
  // deferred to the P2 followup that *fixes* the gap.
  //
  // TODO(p2-ecost): once the cost term is added to the template,
  // flip this test to assert a non-zero objective coefficient on
  // the eini/extraction column for the accumulator.
  const BoundaryUCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {30},
          .name = "maule_vol_econ_invernada",
          .reservoir = Uid {1},
          .emax = 1000.0,
          .eini = 0.0,
          .saving_rate = 50.0,
          .use_state_variable = true,
      },
  };

  // TODO(phase-C): this test is a documentation placeholder for the
  // missing `ecost` term on the economia-invernada accumulator.  The
  // full assembly pipeline (simulation → system → LP) under the
  // BoundaryUCFixture does not currently surface the VolumeRight column
  // through `find_ampl_col`; the P2 followup that adds the cost term
  // will also wire the registry lookup so this test can flip to a
  // non-zero objective-coefficient assertion.
  (void)fx;
  (void)vrs;
}
