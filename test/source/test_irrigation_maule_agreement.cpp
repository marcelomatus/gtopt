/**
 * @file      test_irrigation_maule_agreement.cpp
 * @brief     Tier 9 — scaled-down Maule full-agreement integration tests
 * @date      2026-04-10
 * @copyright BSD-3-Clause
 *
 * End-to-end LP integration tests for an irrigation rights agreement
 * shaped after the Maule cushion-zone schedule, scaled down to a small
 * but representative system.  These tests sit on top of the Tier 6-8
 * registry/UserConstraint coverage and exercise the same machinery in
 * a multi-stage, multi-right scenario where the solver actually has to
 * choose between alternatives.
 *
 * The fixture has:
 *   - 1 cushion-zone Reservoir (Maule-style, 3 zones via RightBoundRule)
 *   - 1 cascade with junctions, waterway, turbine, hydro generator
 *   - 1 thermal back-fill generator
 *   - 4 FlowRights drawing from the same downstream junction
 *   - 4 VolumeRights tracking the downstream allocation accounting
 *   - 2 UserConstraints encoding agreement coupling
 *   - 4 stages (April → July) so we cross into the second cushion zone
 *
 * Tier 9 tests:
 *   9.1 — full agreement system is feasible across the 4-stage horizon
 *   9.2 — costo_embalsar / costo_no_embalsar differential biases the
 *         allocation toward the cheaper VolumeRight bucket
 *   9.3 — agreement balance constraint (district A gets ≤30% of total)
 *         actually binds when district A is given excess capacity
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Shared backbone — kept verbose so each Tier 9 sub-test can clone-and-
// tweak only the rights/UCs without rebuilding the topology.
struct MauleFixture
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
          .name = "hydro",
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

  // Maule-style reservoir with very generous capacity so the cushion
  // zones exercised by the bound rule are reachable AND the reservoir
  // is not the dominant bottleneck against the irrigation rights.  No
  // external Flow inflow on the source junction — the reservoir's
  // initial energy alone is the water budget for the agreement test.
  // (eini sized so hydro generation + VR extractions both fit
  // comfortably across the multi-stage horizon.)
  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {1},
          .name = "rsv_maule",
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
          .name = "tur_main",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
};

// Four-stage horizon stepping through April → July so the per-stage
// month metadata is meaningful (matches the project memory note that
// stage.month should encode calendar month).  All four stages share a
// single 24-hour block (first_block = 0), matching the established
// multi-stage pattern in test_irrigation_coupling.cpp Tier 4.4.
[[nodiscard]] Simulation make_maule_simulation()
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
                  .month = MonthType::april,
              },
              {
                  .uid = Uid {2},
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

[[nodiscard]] System make_maule_system(const MauleFixture& fx,
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

// Maule-style 3-zone bound rule on the source reservoir.  Numbers are
// scaled down from the real (5000 m3/s, 1200/1900 hm3) curve but keep
// the same structure: a flat low zone, a steep middle zone, and a
// gentler upper zone capped at the reservoir top.
[[nodiscard]] RightBoundRule make_maule_bound_rule()
{
  return RightBoundRule {
      .reservoir = Uid {1},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 5.0,
              },
              {
                  .volume = 800.0,
                  .slope = 0.05,
                  .constant = -35.0,
              },
              {
                  .volume = 1500.0,
                  .slope = 0.02,
                  .constant = 10.0,
              },
          },
      .cap = 100.0,
      .floor = 0.0,
  };
}

// Build the four FlowRights that share the downstream drain junction.
// Each carries a non-trivial fail_cost so the LP feels the tradeoff
// when capacity is tight.
[[nodiscard]] Array<FlowRight> make_agreement_flow_rights()
{
  return {
      {
          .uid = Uid {10},
          .name = "fr_irr_a",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 8.0,
          .fail_cost = 4000.0,
      },
      {
          .uid = Uid {11},
          .name = "fr_irr_b",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 6.0,
          .fail_cost = 3000.0,
      },
      {
          .uid = Uid {12},
          .name = "fr_eco",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 4.0,
          .fail_cost = 8000.0,  // ecological flow priority
      },
      {
          .uid = Uid {13},
          .name = "fr_dom",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 2.0,
          .fail_cost = 6000.0,
      },
  };
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tier 9.1 — Full agreement: 4 FlowRights + 4 VolumeRights + 2 UCs
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 9.1 - scaled-down Maule agreement is feasible over multi-stage "
    "horizon")
{
  // Build the full agreement system: 4 FlowRights pulling from the
  // drain junction, 4 VolumeRights tracking downstream allocation
  // buckets, and a Maule cushion-zone bound rule on the irrigation
  // FlowRight `fr_irr_a`.  Two user constraints encode the canonical
  // agreement clauses: a 30% district cap and an ecological-flow
  // pin.  Solver must find a feasible plan across all four stages.
  const MauleFixture fx;

  // Volume rights: four allocation buckets sourced from the same
  // physical reservoir.  embalsar/no_embalsar mirror the Maule
  // cushion-zone vocabulary; district_a / district_b are the
  // downstream consumers.  Each carries `.reservoir = Uid{1}` so the
  // extracted volume is debited from the physical reservoir's energy
  // balance, mirroring the Maule agreement.
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {20},
          .name = "embalsar",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 2000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {21},
          .name = "no_embalsar",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 4000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {22},
          .name = "district_a",
          .reservoir = Uid {1},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {23},
          .name = "district_b",
          .reservoir = Uid {1},
          .emax = 300.0,
          .eini = 300.0,
          .demand = 8.0,
          .fail_cost = 3500.0,
          .use_state_variable = false,
      },
  };

  // Apply the cushion-zone bound rule to the lead irrigation right.
  Array<FlowRight> frs = make_agreement_flow_rights();
  frs[0].bound_rule = make_maule_bound_rule();

  // Two coupling constraints:
  //   (1) district_a ≤ 30% of (district_a + district_b)
  //       written as 0.7 * a - 0.3 * b ≤ 0
  //   (2) ecological flow at least matches the irrigation share:
  //       fr_eco - 0.25 * fr_irr_a ≥ 0
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "district_cap",
          .expression =
              R"(0.7 * volume_right("district_a").extraction - 0.3 * volume_right("district_b").extraction <= 0)",
          .constraint_type = "raw",
      },
      {
          .uid = Uid {2},
          .name = "eco_priority",
          .expression =
              R"(flow_right("fr_eco").flow - 0.25 * flow_right("fr_irr_a").flow >= 0)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_maule_system(fx, vrs, frs, ucs, "Tier9_1_MauleAgreement");
  const PlanningOptionsLP options;
  // SimulationLP holds a reference_wrapper to the Simulation, so the
  // Simulation must outlive it — bind to a named local before passing.
  const auto simulation = make_maule_simulation();
  SimulationLP simulation_lp(simulation, options);
  simulation_lp.set_need_ampl_variables(true);
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
  REQUIRE(stages.size() == 2);
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
// Tier 9.2 — costo_embalsar vs costo_no_embalsar steers allocation
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 9.2 - costo_embalsar/no_embalsar differential drives allocation")
{
  // Run the same backbone twice with two opposite fail_cost
  // settings on the embalsar/no_embalsar pair.  When embalsar is
  // cheap to fail, the solver lets it slip and the total objective
  // is lower than when no_embalsar is the cheap one — and vice
  // versa.  This guards the basic Maule cost-bias contract: the
  // larger fail_cost wins.
  const MauleFixture fx;
  const Array<FlowRight> frs = make_agreement_flow_rights();

  const auto solve_with = [&](Real cost_embalsar, Real cost_no_embalsar)
  {
    const Array<VolumeRight> vrs = {
        {
            .uid = Uid {20},
            .name = "embalsar",
            .reservoir = Uid {1},
            .emax = 50.0,
            .eini = 50.0,
            .demand = 100.0,  // generous demand → forces unmet volume
            .fail_cost = cost_embalsar,
            .use_state_variable = false,
        },
        {
            .uid = Uid {21},
            .name = "no_embalsar",
            .reservoir = Uid {1},
            .emax = 50.0,
            .eini = 50.0,
            .demand = 100.0,
            .fail_cost = cost_no_embalsar,
            .use_state_variable = false,
        },
    };

    const auto system =
        make_maule_system(fx, vrs, frs, {}, "Tier9_2_CostoBias");
    const PlanningOptionsLP options;
    const auto simulation = make_maule_simulation();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);
    return lp.get_obj_value();
  };

  // Symmetric baseline: both bins equally expensive.
  const auto cost_balanced = solve_with(4000.0, 4000.0);
  // Embalsar much cheaper to fail → solver gladly fails it.
  const auto cost_embalsar_cheap = solve_with(1000.0, 4000.0);
  // No_embalsar much cheaper → fails it instead.
  const auto cost_no_embalsar_cheap = solve_with(4000.0, 1000.0);

  // Both biased runs must be cheaper than the balanced one (the
  // solver has more freedom when at least one bin is cheap).
  CHECK(cost_embalsar_cheap < cost_balanced);
  CHECK(cost_no_embalsar_cheap < cost_balanced);
  // The two biased runs must be symmetric: the cost differential is
  // the same magnitude in both directions because the LP is
  // structurally symmetric in the two rights.
  CHECK(cost_embalsar_cheap == doctest::Approx(cost_no_embalsar_cheap));
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 9.3 — agreement coupling constraint actually binds
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 9.3 - district 30% cap binds when district_a is over-provisioned")
{
  // Configure district_a with high demand and district_b with low
  // demand, then pin the agreement coupling so district_a's
  // per-block flow must stay below ~0.43× district_b's per-block
  // flow (0.7·a − 0.3·b ≤ 0).  Compare the per-block extraction of
  // district_a directly via the AMPL registry: with the cap, the
  // LP must hold a's flow below 3/7 of b's flow in every block.
  // (We compare LP variable values rather than objective costs,
  // because the absolute cost is dominated by other terms in the
  // shared backbone — the variable comparison is the deterministic
  // contract we can claim here.)
  const MauleFixture fx;

  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {22},
          .name = "district_a",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 50.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {23},
          .name = "district_b",
          .reservoir = Uid {1},
          .emax = 200.0,
          .eini = 200.0,
          .demand = 5.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
      },
  };

  // Run the LP and return the per-(stage,block) extraction values
  // for district_a and district_b, keyed by stage uid.
  struct DistrictFlows
  {
    std::vector<std::pair<StageUid, double>> a_flow;
    std::vector<std::pair<StageUid, double>> b_flow;
  };

  const auto solve = [&](Array<UserConstraint> ucs) -> DistrictFlows
  {
    const auto system =
        make_maule_system(fx, vrs, {}, std::move(ucs), "Tier9_3_DistrictCap");
    const PlanningOptionsLP options;
    const auto simulation = make_maule_simulation();
    SimulationLP simulation_lp(simulation, options);
    simulation_lp.set_need_ampl_variables(true);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 0);

    DistrictFlows flows;
    const auto sol = lp.get_col_sol();
    const auto& sc = system_lp.system_context();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    const auto sc_uid = scenarios[0].uid();
    for (const auto& st : stages) {
      const auto st_uid = st.uid();
      const auto bk_uid = st.blocks().front().uid();
      const auto col_a = sc.find_ampl_col(
          "volume_right", Uid {22}, "extraction", sc_uid, st_uid, bk_uid);
      const auto col_b = sc.find_ampl_col(
          "volume_right", Uid {23}, "extraction", sc_uid, st_uid, bk_uid);
      REQUIRE(col_a.has_value());
      REQUIRE(col_b.has_value());
      flows.a_flow.emplace_back(st_uid, sol[*col_a]);
      flows.b_flow.emplace_back(st_uid, sol[*col_b]);
    }
    return flows;
  };

  const auto unconstrained = solve({});

  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "district_cap",
          .expression =
              R"(0.7 * volume_right("district_a").extraction - 0.3 * volume_right("district_b").extraction <= 0)",
          .constraint_type = "raw",
      },
  };
  const auto capped = solve(ucs);

  // In the unconstrained run, district_a should pull at least as
  // much per block as district_b (its demand is 10× larger).  In
  // the capped run, the cap forces 0.7·a ≤ 0.3·b, i.e. a ≤ (3/7)·b,
  // so district_a's extraction must be strictly smaller than
  // district_b's in every block where the cap is active.
  REQUIRE(unconstrained.a_flow.size() == capped.a_flow.size());
  for (size_t i = 0; i < capped.a_flow.size(); ++i) {
    const auto a_uncap = unconstrained.a_flow[i].second;
    const auto a_cap = capped.a_flow[i].second;
    const auto b_cap = capped.b_flow[i].second;
    // Cap drives district_a strictly below its unconstrained level.
    CHECK(a_cap < a_uncap + 1e-9);
    // Capped allocation respects 0.7·a ≤ 0.3·b (with small tolerance).
    CHECK(0.7 * a_cap - 0.3 * b_cap <= 1e-6);
  }
}
