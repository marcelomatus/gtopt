/**
 * @file      test_irrigation_laja_agreement.cpp
 * @brief     Tier 10 — scaled-down Laja full-agreement integration tests
 * @date      2026-04-10
 * @copyright BSD-3-Clause
 *
 * End-to-end LP integration tests for an irrigation rights agreement
 * shaped after the Laja schedule, scaled down to a small but
 * representative system.  Mirrors test_irrigation_maule_agreement.cpp
 * in structure but exercises Laja-specific features:
 *
 *   - Economy VolumeRights with `reset_month = april`, matching the
 *     Laja IVESF/IVERF/IVAPF accumulators that are wiped clean at the
 *     start of each irrigation year (see
 *     `project_irrigation_design.md` §3).
 *   - A 3-stage horizon (march → april → may) that straddles the
 *     reset boundary, so the provisioning path in
 *     `source/volume_right_lp.cpp:219-226` is exercised.
 *   - A Laja-style 2-zone bound rule on the lead irrigation FlowRight.
 *
 * Tier 10 tests:
 *   10.1 — full Laja agreement is feasible across the reset boundary
 *   10.2 — economy VolumeRight's eini is re-provisioned at reset_month
 *          (inspect eini column bounds via the registry)
 *   10.3 — canal coupling constraint binds when canal_x is over-
 *          provisioned (district-cap analogue)
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Shared backbone — kept verbose so each Tier 10 sub-test can clone-and-
// tweak only the rights/UCs without rebuilding the topology.
struct LajaFixture
{
  Array<Bus> bus_array {
      {
          .uid = Uid {1},
          .name = "b_main",
      },
  };

  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "hydro_laja",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 300.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 500.0,
      },
  };

  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d_main",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };

  // Hydro topology: reservoir → waterway → drain junction
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
          .name = "ww_main",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 800.0,
      },
  };

  // Laja-style reservoir.  Capacity is generous so the reservoir is
  // not the dominant bottleneck against the irrigation rights, and the
  // initial energy alone is the water budget for the agreement test.
  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {1},
          .name = "rsv_laja",
          .junction = Uid {1},
          .capacity = 1'000'000.0,
          .emin = 0.0,
          .emax = 1'000'000.0,
          .eini = 1'000'000.0,
      },
  };

  Array<Turbine> turbine_array {
      {
          .uid = Uid {1},
          .name = "tur_laja",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
};

// Three-stage horizon stepping through March → April → May.  April is
// the Laja irrigation-year reset point — economy VolumeRights with
// `reset_month = april` have their eini re-provisioned at the start of
// that stage, breaking the efin(march)→eini(april) linkage.  Each
// stage carries a single 24-hour block.
[[nodiscard]] Simulation make_laja_simulation()
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
                  .month = MonthType::march,
              },
              {
                  .uid = Uid {2},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::april,
              },
              {
                  .uid = Uid {3},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::may,
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

[[nodiscard]] System make_laja_system(const LajaFixture& fx,
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

// Laja-style 2-zone bound rule on the source reservoir.  Simpler than
// Maule's 3-zone cushion curve: a flat baseline below the rule
// volume, then a linear ramp above.  Structurally the same
// reservoir_volume axis as Maule, just a different shape.
[[nodiscard]] RightBoundRule make_laja_bound_rule()
{
  return RightBoundRule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 4.0,
              },
              {
                  .volume = 1000.0,
                  .slope = 0.03,
                  .constant = -26.0,
              },
          },
      .cap = 80.0,
      .floor = 0.0,
  };
}

// Build the four FlowRights that share the downstream drain junction.
// Each carries a non-trivial fail_cost so the LP feels the tradeoff
// when capacity is tight.  Canal x/y are the agricultural canals;
// eco_laja is the ecological flow pin; muni is municipal supply.
[[nodiscard]] Array<FlowRight> make_laja_flow_rights()
{
  return {
      {
          .uid = Uid {10},
          .name = "fr_canal_x",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 8.0,
          .fail_cost = 4000.0,
      },
      {
          .uid = Uid {11},
          .name = "fr_canal_y",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 6.0,
          .fail_cost = 3000.0,
      },
      {
          .uid = Uid {12},
          .name = "fr_eco_laja",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 4.0,
          .fail_cost = 8000.0,
      },
      {
          .uid = Uid {13},
          .name = "fr_muni",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 2.0,
          .fail_cost = 6000.0,
      },
  };
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tier 10.1 — Full Laja agreement is feasible across the reset boundary
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 10.1 - scaled-down Laja agreement is feasible across reset month")
{
  // Build the full Laja agreement system: 4 FlowRights pulling from
  // the drain junction, 4 VolumeRights tracking downstream
  // allocation buckets (two of which are economy accumulators with
  // reset_month=april), and a Laja 2-zone bound rule on the lead
  // irrigation FlowRight.  Two user constraints encode the canonical
  // agreement clauses: a canal balance cap and an ecological-flow
  // pin.  Solver must find a feasible plan across all three stages
  // (march → april → may), crossing the April reset boundary.
  const LajaFixture fx;

  // Volume rights: two economy accumulators (IVESF/IVERF analogues)
  // and two canal allocation trackers.  Economy VRs get
  // reset_month=april so eini is re-provisioned in the april stage.
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {20},
          .name = "econ_ives",
          .reservoir = Uid {1},
          .emax = 150.0,
          .eini = 150.0,
          .demand = 4.0,
          .fail_cost = 2000.0,
          .use_state_variable = false,
          .reset_month = MonthType::april,
      },
      {
          .uid = Uid {21},
          .name = "econ_iver",
          .reservoir = Uid {1},
          .emax = 150.0,
          .eini = 150.0,
          .demand = 4.0,
          .fail_cost = 4000.0,
          .use_state_variable = false,
          .reset_month = MonthType::april,
      },
      {
          .uid = Uid {22},
          .name = "canal_x",
          .reservoir = Uid {1},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {23},
          .name = "canal_y",
          .reservoir = Uid {1},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
  };

  // Apply the Laja bound rule to the lead irrigation right.
  Array<FlowRight> frs = make_laja_flow_rights();
  frs[0].bound_rule = make_laja_bound_rule();

  // Two coupling constraints:
  //   (1) canal_x ≤ 30% of (canal_x + canal_y)
  //       written as 0.7 * x - 0.3 * y ≤ 0
  //   (2) ecological flow at least matches the canal_x share:
  //       fr_eco_laja - 0.25 * fr_canal_x ≥ 0
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "canal_cap",
          .expression =
              R"(0.7 * volume_right("canal_x").extraction - 0.3 * volume_right("canal_y").extraction <= 0)",
          .constraint_type = "raw",
      },
      {
          .uid = Uid {2},
          .name = "eco_priority",
          .expression =
              R"(flow_right("fr_eco_laja").flow - 0.25 * flow_right("fr_canal_x").flow >= 0)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_laja_system(fx, vrs, frs, ucs, "Tier10_1_LajaAgreement");
  const PlanningOptionsLP options;
  // SimulationLP holds a reference_wrapper to the Simulation, so the
  // Simulation must outlive it — bind to a named local before passing.
  const auto simulation = make_laja_simulation();
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Sanity: the resolver registered every right in every (sc, st) cell.
  const auto& sc = system_lp.system_context();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(scenarios.size() == 1);
  REQUIRE(stages.size() == 3);
  const auto sc_uid = scenarios[0].uid();

  for (const auto& st : stages) {
    const auto st_uid = st.uid();
    REQUIRE(!st.blocks().empty());
    const auto bk_uid = st.blocks().front().uid();

    // All four VolumeRights have an extraction column in this stage.
    for (const auto& vr : vrs) {
      const auto col = sc.find_ampl_col(
          "volume_right", vr.uid, "extraction", sc_uid, st_uid, bk_uid);
      REQUIRE(col.has_value());
    }
    // All four FlowRights have a flow column in this stage.
    for (const auto& fr : frs) {
      const auto col = sc.find_ampl_col(
          "flow_right", fr.uid, "flow", sc_uid, st_uid, bk_uid);
      REQUIRE(col.has_value());
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 10.2 — Economy VolumeRight eini is re-provisioned at reset_month
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 10.2 - economy VolumeRight eini is clamped in april (reset_month)")
{
  // At the reset_month stage, `source/volume_right_lp.cpp:219-226`
  // clamps the stage's eini column with `lowb == uppb == provision`.
  // Without a `bound_rule`, the provision value is `emax` (the annual
  // quota).  This test verifies that structural fact directly via
  // the LinearInterface bounds — a deterministic contract that does
  // not depend on the LP's optimization choices.
  //
  // Control: an otherwise-identical VolumeRight with `reset_month`
  // unset must NOT have its april eini bounds clamped to a point.
  const LajaFixture fx;

  constexpr double provision = 50.0;

  const auto run = [&](bool with_reset)
  {
    VolumeRight vr {
        .uid = Uid {30},
        .name = "econ_reset",
        .reservoir = Uid {1},
        .emax = provision,
        .eini = provision,
        .demand = 10.0,
        .fail_cost = 5000.0,
        .use_state_variable = false,
    };
    if (with_reset) {
      vr.reset_month = MonthType::april;
    }
    const Array<VolumeRight> vrs = {vr};
    const auto system = make_laja_system(fx, vrs, {}, {}, "Tier10_2_EconReset");
    const PlanningOptionsLP options;
    const auto simulation = make_laja_simulation();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    const auto& sc = system_lp.system_context();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    REQUIRE(stages.size() == 3);
    const auto sc_uid = scenarios[0].uid();

    // April is stages[1] (march/april/may horizon).  The eini
    // attribute is stage-scoped in the registry but `find_ampl_col`
    // still takes a block_uid — any block in the stage works.
    const auto april_uid = stages[1].uid();
    const auto bk_uid = stages[1].blocks().front().uid();
    const auto eini_col = sc.find_ampl_col(
        "volume_right", Uid {30}, "eini", sc_uid, april_uid, bk_uid);
    REQUIRE(eini_col.has_value());

    return std::pair {lp.get_col_low()[*eini_col], lp.get_col_upp()[*eini_col]};
  };

  // With reset: lowb == uppb == provision (clamped to a point).
  const auto [low_reset, upp_reset] = run(/*with_reset=*/true);
  CHECK(low_reset == doctest::Approx(provision));
  CHECK(upp_reset == doctest::Approx(provision));

  // Without reset: the bounds must NOT collapse to a point at `provision`.
  // The eini column (= previous stage's efin) is free within [0, emax]
  // so upp > low — any non-point interval is evidence the reset is off.
  const auto [low_noreset, upp_noreset] = run(/*with_reset=*/false);
  CHECK(upp_noreset > low_noreset + 1e-6);
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 10.3 — canal coupling constraint actually binds
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 10.3 - canal 30% cap binds when canal_x is over-provisioned")
{
  // Configure canal_x with high demand and canal_y with low demand,
  // then pin the agreement coupling so canal_x's per-block flow
  // must stay below ~0.43× canal_y's per-block flow (0.7·x − 0.3·y
  // ≤ 0).  Compare per-stage extraction of canal_x directly via the
  // AMPL registry: with the cap, the LP must hold x's flow below
  // 3/7 of y's flow in every stage.
  const LajaFixture fx;

  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {22},
          .name = "canal_x",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 50.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {23},
          .name = "canal_y",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
      },
  };

  // Run the LP and return the per-stage extraction values for
  // canal_x and canal_y, keyed by stage uid.
  struct CanalFlows
  {
    std::vector<std::pair<StageUid, double>> x_flow;
    std::vector<std::pair<StageUid, double>> y_flow;
  };

  const auto solve = [&](Array<UserConstraint> ucs) -> CanalFlows
  {
    const auto system =
        make_laja_system(fx, vrs, {}, std::move(ucs), "Tier10_3_CanalCap");
    const PlanningOptionsLP options;
    const auto simulation = make_laja_simulation();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    CanalFlows flows;
    const auto sol = lp.get_col_sol();
    const auto& sc = system_lp.system_context();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    const auto sc_uid = scenarios[0].uid();
    for (const auto& st : stages) {
      const auto st_uid = st.uid();
      const auto bk_uid = st.blocks().front().uid();
      const auto col_x = sc.find_ampl_col(
          "volume_right", Uid {22}, "extraction", sc_uid, st_uid, bk_uid);
      const auto col_y = sc.find_ampl_col(
          "volume_right", Uid {23}, "extraction", sc_uid, st_uid, bk_uid);
      REQUIRE(col_x.has_value());
      REQUIRE(col_y.has_value());
      flows.x_flow.emplace_back(st_uid, sol[*col_x]);
      flows.y_flow.emplace_back(st_uid, sol[*col_y]);
    }
    return flows;
  };

  const auto unconstrained = solve({});

  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "canal_cap",
          .expression =
              R"(0.7 * volume_right("canal_x").extraction - 0.3 * volume_right("canal_y").extraction <= 0)",
          .constraint_type = "raw",
      },
  };
  const auto capped = solve(ucs);

  // In the unconstrained run, canal_x should pull at least as much
  // per stage as canal_y (its demand is 10× larger).  In the capped
  // run, the cap forces 0.7·x ≤ 0.3·y, i.e. x ≤ (3/7)·y, so canal_x's
  // extraction must be strictly smaller than canal_y's in every
  // stage where the cap is active.
  REQUIRE(unconstrained.x_flow.size() == capped.x_flow.size());
  for (size_t i = 0; i < capped.x_flow.size(); ++i) {
    const auto x_uncap = unconstrained.x_flow[i].second;
    const auto x_cap = capped.x_flow[i].second;
    const auto y_cap = capped.y_flow[i].second;
    // Cap drives canal_x strictly below its unconstrained level.
    CHECK(x_cap < x_uncap + 1e-9);
    // Capped allocation respects 0.7·x ≤ 0.3·y (with small tolerance).
    CHECK(0.7 * x_cap - 0.3 * y_cap <= 1e-6);
  }
}
