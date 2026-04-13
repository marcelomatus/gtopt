/**
 * @file      test_irrigation_user_constraints.cpp
 * @brief     Tier 7-8 — UserConstraint resolution against irrigation rights
 * @date      2026-04-10
 * @copyright BSD-3-Clause
 *
 * Verifies that user-supplied constraint expressions referencing
 * `volume_right("...")` and `flow_right("...")` resolve through the
 * AMPL element-attribute registry introduced by f02856d7 and bind
 * correctly into the LP.  These tests sit between the unit-level
 * registry checks (Tier 6) and the full-agreement integration tests
 * (Tier 9-11).
 *
 * Each Tier 7 sub-test exercises one attribute kind on a single
 * right.  Tier 8 sub-tests cover multi-term coupling expressions
 * representative of real Maule/Laja agreement constraints.
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Minimal hydraulic backbone shared by every test below: 1 bus,
// 1 hydro generator, 1 thermal back-fill, 1 demand, reservoir feeding
// a turbine via a single waterway.
struct UCFixture
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

[[nodiscard]] Simulation make_uc_simulation()
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

[[nodiscard]] System make_uc_system(const UCFixture& fx,
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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tier 7 — UserConstraint references against a single irrigation right
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 7.1 - volume_right.extraction reference resolves and binds")
{
  // The user constraint forces the VolumeRight's extraction column to
  // be at most 1 m³/s.  Solving must succeed (registry resolved the
  // reference) and the LP must remain feasible.  We exercise the
  // canonical attribute name `extraction`.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vrt1",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .demand = 5.0,
          .fail_cost = 1000.0,
          .use_state_variable = false,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "vrt_cap",
          .expression = R"(volume_right("vrt1").extraction <= 1)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_uc_system(fx, vrs, {}, ucs, "Tier7_1_VolumeRight_extraction");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 7.2 - flow_right.flow reference resolves and binds")
{
  // The user constraint caps the FlowRight's flow column at 5 m³/s.
  // Solving must succeed and the LP must remain feasible.
  const UCFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr1",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 30.0,
          .fail_cost = 5000.0,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "fr_cap",
          .expression = R"(flow_right("fr1").flow <= 5)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_uc_system(fx, {}, frs, ucs, "Tier7_2_FlowRight_flow");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 7.3 - volume_right.eini state-backed reference resolves")
{
  // The state-backed `eini` column is registered separately by
  // StorageBase::add_to_lp.  This test pins it to a specific value
  // via a user constraint and verifies the resolver picks it up.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vrt_state",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 80.0,
          .demand = 5.0,
          .fail_cost = 1000.0,
          .use_state_variable = false,
      },
  };
  // eini is the global initial-condition column on the first stage,
  // so the constraint must agree with the configured eini value.
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "vrt_eini_pin",
          .expression = R"(volume_right("vrt_state").eini = 80)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_uc_system(fx, vrs, {}, ucs, "Tier7_3_VolumeRight_eini");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 7.4 - UserConstraint referencing missing element fails in strict "
    "mode")
{
  // A user constraint that names a non-existent VolumeRight must be
  // rejected when the constraint mode is strict.  This pins down the
  // "user constraints must error on unresolved/inactive element refs"
  // contract from the project memory.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vrt_real",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .demand = 5.0,
          .fail_cost = 1000.0,
          .use_state_variable = false,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "ghost_ref",
          .expression = R"(volume_right("does_not_exist").extraction <= 1)",
          .constraint_type = "raw",
      },
  };

  // Strict mode is the default for PlanningOptionsLP, so the resolver
  // will throw when the ghost element ref cannot be bound — no extra
  // option tweaking needed.
  const auto system =
      make_uc_system(fx, vrs, {}, ucs, "Tier7_4_StrictMissingRef");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  // Constructing/solving the SystemLP must throw because the strict
  // resolver cannot bind the missing element reference.
  CHECK_THROWS((
      [&]
      {
        SystemLP system_lp(system, simulation_lp);
        auto&& lp = system_lp.linear_interface();
        (void)lp.resolve();
      }()));
}

TEST_CASE(  // NOLINT
    "Tier 7.5 - flow_right.fail deficit attribute resolves when registered")
{
  // FlowRight with discharge>0 and fail_cost>0 registers the `fail`
  // deficit variable.  A user constraint capping the deficit at 0
  // forces the solver to fully serve the right (or report
  // infeasibility — feasibility is not the contract here, the
  // contract is that the resolver finds the column).
  const UCFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_fail",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 1.0,
          .fail_cost = 5000.0,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "no_deficit",
          .expression = R"(flow_right("fr_fail").fail <= 1000)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_uc_system(fx, {}, frs, ucs, "Tier7_5_FlowRight_fail");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 8 — UserConstraint coupling multiple irrigation rights
//
// These mirror representative real-world coupling expressions from the
// Maule and Laja agreements (5-term balances, percentage splits,
// district partitions).  The contract under test here is purely
// resolution + binding — solver feasibility is not the concern, so
// each setup uses generous capacities.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 8.1 - La Invernada 5-term balance resolves")
{
  // deficit + sin_deficit + natural = embalsar + no_embalsar
  // Built from 5 VolumeRights tied to the same physical reservoir,
  // each carrying a small extraction budget.  The constraint links
  // their extraction columns into a single balance row.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "deficit",
          .reservoir = Uid {1},
          .emax = 50.0,
          .eini = 50.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {2},
          .name = "sin_deficit",
          .reservoir = Uid {1},
          .emax = 50.0,
          .eini = 50.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {3},
          .name = "natural",
          .reservoir = Uid {1},
          .emax = 50.0,
          .eini = 50.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {4},
          .name = "embalsar",
          .reservoir = Uid {1},
          .emax = 50.0,
          .eini = 50.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {5},
          .name = "no_embalsar",
          .reservoir = Uid {1},
          .emax = 50.0,
          .eini = 50.0,
          .use_state_variable = false,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "invernada_balance",
          .expression =
              R"(volume_right("deficit").extraction + volume_right("sin_deficit").extraction + volume_right("natural").extraction = volume_right("embalsar").extraction + volume_right("no_embalsar").extraction)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_uc_system(fx, vrs, {}, ucs, "Tier8_1_LaInvernada");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 8.2 - Maule 20/80 percentage split resolves")
{
  // ordinary_elec - 0.2 * ordinary_elec - 0.2 * ordinary_riego <= 0
  // i.e. ordinary_elec ≤ 0.2 × (ordinary_elec + ordinary_riego)
  // Expressed in raw form to keep the parser happy.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "ordinary_elec",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {2},
          .name = "ordinary_riego",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "maule_pct",
          .expression =
              R"(0.8 * volume_right("ordinary_elec").extraction - 0.2 * volume_right("ordinary_riego").extraction <= 0)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_uc_system(fx, vrs, {}, ucs, "Tier8_2_Maule_20_80");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 8.3 - District proportional split resolves")
{
  // Three districts each get a fraction of total irrigation.  We
  // model the constraint as district_A ≤ 0.30 × total_irr, where
  // total_irr is itself the sum of the three district extractions.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "district_A",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {2},
          .name = "district_B",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {3},
          .name = "district_C",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
  };
  // 0.7 * A - 0.3 * B - 0.3 * C <= 0  ⇔  A <= 0.3 * (A + B + C)
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "district_split",
          .expression =
              R"(0.7 * volume_right("district_A").extraction - 0.3 * volume_right("district_B").extraction - 0.3 * volume_right("district_C").extraction <= 0)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_uc_system(fx, vrs, {}, ucs, "Tier8_3_DistrictSplit");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 8.4 - Mixed VolumeRight + FlowRight expression resolves")
{
  // A coupling expression mixes a VolumeRight extraction column with
  // a FlowRight flow column at a non-unit coefficient.  Demonstrates
  // that the resolver can mediate between the two element types in
  // the same row.
  const UCFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vrt_mix",
          .reservoir = Uid {1},
          .emax = 100.0,
          .eini = 100.0,
          .use_state_variable = false,
      },
  };
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_mix",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 5.0,
          .fail_cost = 1000.0,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "mixed_coupling",
          .expression =
              R"(2.0 * volume_right("vrt_mix").extraction - flow_right("fr_mix").flow >= 0)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_uc_system(fx, vrs, frs, ucs, "Tier8_4_Mixed");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 8.5 - Laja 4-way flow partition resolves")
{
  // IQGT = IQDR + IQDE + IQDM + IQGA — the irrigation total is the
  // sum of four downstream channels.  All five FlowRights live on the
  // same drain junction.
  const UCFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "iqgt",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 100.0,
          .fail_cost = 1000.0,
      },
      {
          .uid = Uid {2},
          .name = "iqdr",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 25.0,
          .fail_cost = 1000.0,
      },
      {
          .uid = Uid {3},
          .name = "iqde",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 25.0,
          .fail_cost = 1000.0,
      },
      {
          .uid = Uid {4},
          .name = "iqdm",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 25.0,
          .fail_cost = 1000.0,
      },
      {
          .uid = Uid {5},
          .name = "iqga",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 25.0,
          .fail_cost = 1000.0,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "laja_partition",
          .expression =
              R"(flow_right("iqgt").flow - flow_right("iqdr").flow - flow_right("iqde").flow - flow_right("iqdm").flow - flow_right("iqga").flow = 0)",
          .constraint_type = "raw",
      },
  };

  const auto system = make_uc_system(fx, {}, frs, ucs, "Tier8_5_LajaPartition");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "Tier 8.6 - La Invernada FlowRight balance binds non-trivially (P0 "
    "defender)")
{
  // Regression defender for the template bug fixed on 2026-04-11 in
  // scripts/gtopt_expand/templates/maule.tson:418-470.  The template
  // previously emitted the five La Invernada FlowRights with
  // `discharge: 0` and no `fmax` field, which per the asymmetric default
  // at flow_right_lp.cpp:53-56 pinned every flow column to [0, 0] and
  // reduced the `invernada_balance` UserConstraint to `0 + 0 + 0 = 0 + 0`.
  //
  // The fix renders `qmax_invernada` into the `fmax` field so
  // `discharge == 0 && fmax > 0` activates variable mode `[0, fmax]`.
  // This test mirrors the fixed template shape exactly (5 FlowRights,
  // +1/+1/+1/-1/-1 directions, `discharge = 0`, `fmax > 0`) and asserts
  // that every flow column's upper bound is strictly positive — i.e.
  // the constraint has room to bind.  Under the P0 bug every column's
  // upper bound is 0, the assertion fails, and the defender fires.
  //
  // Note: the real template also sets `use_average = true` to expose a
  // `qeh` stage-average variable; that's orthogonal to the bounds check
  // and is omitted here to keep the fixture small.  The real template
  // also leaves `junction` unset (the Invernada FlowRights are not
  // hydraulically coupled, they exist only for the balance constraint),
  // which we also mirror.
  const UCFixture fx;
  constexpr Real kQmax = 200.0;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "inv_deficit",
          .direction = 1,
          .discharge = 0.0,
          .fmax = kQmax,
      },
      {
          .uid = Uid {2},
          .name = "inv_sin_deficit",
          .direction = 1,
          .discharge = 0.0,
          .fmax = kQmax,
      },
      {
          .uid = Uid {3},
          .name = "inv_caudal_natural",
          .direction = 1,
          .discharge = 0.0,
          .fmax = kQmax,
      },
      {
          .uid = Uid {4},
          .name = "inv_embalsar",
          .direction = -1,
          .discharge = 0.0,
          .fmax = kQmax,
      },
      {
          .uid = Uid {5},
          .name = "inv_no_embalsar",
          .direction = -1,
          .discharge = 0.0,
          .fmax = kQmax,
      },
  };
  const Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "invernada_balance",
          .expression =
              R"(flow_right("inv_deficit").flow + flow_right("inv_sin_deficit").flow + flow_right("inv_caudal_natural").flow = flow_right("inv_embalsar").flow + flow_right("inv_no_embalsar").flow)",
          .constraint_type = "raw",
      },
  };

  const auto system =
      make_uc_system(fx, {}, frs, ucs, "Tier8_6_Invernada_P0_Defender");
  const PlanningOptionsLP options;
  SimulationLP simulation_lp(make_uc_simulation(), options);
  SystemLP system_lp(system, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The P0 defender: every flow column's upper bound must equal kQmax,
  // not 0.  Under the bug (`fmax` missing from the template) the default
  // path at flow_right_lp.cpp:53-56 drives `block_fmax == 0 &&
  // block_discharge == 0`, which yields `uppb = block_discharge = 0` and
  // the column is pinned at [0, 0].  With the fix, `block_fmax > 0 &&
  // block_discharge == 0` yields `uppb = block_fmax = kQmax`.
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
      CHECK(col_upp[col] == doctest::Approx(kQmax));
    }
  }
}
