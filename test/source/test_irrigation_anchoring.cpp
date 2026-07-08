// SPDX-License-Identifier: BSD-3-Clause
//
// Tier 9 — physical anchoring of the rights partition.
//
// PLP ties the Laja/Maule rights partition directly to the central's
// physical generation column (`-qg37 + l_qdr + l_qde + ... = 0`,
// genpdlajam.f:70-76).  gtopt reproduces this with three PAMPL-style
// UserConstraints emitted by gtopt_expand:
//
//   partition : qgt = qdr + qde
//   anchor    : qgt = waterway('ww').flow      (gen arc ONLY — spills
//               are deliberately excluded so spilled water can never
//               be charged to, or rewarded through, a rights bucket)
//   ledger    : volume_right(v*).extraction = flow_right(q*).flow
//
// The ledger VolumeRights carry NO `reservoir` coupling: the physical
// water leaves through the anchored turbine arc; the buckets are pure
// accounting (PLP-faithful).
//
// The three sub-tests walk the causal chain:
//   1. the anchor binds (qgt == physical turbined flow),
//   2. the cheapest category absorbs the flow (usage-cost steering),
//   3. a nearly-empty ledger caps its category and forces the
//      remainder onto the costed category — the agreements actually
//      constrain the dispatch.

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <gtopt/waterway_lp.hpp>

using namespace gtopt;

namespace
{

// One bus; free hydro (reservoir -> j_up -> ww -> j_down drain,
// turbine pf = 2.0) vs a 100 $/MWh thermal back-fill; 50 MW demand
// over a single 24 h block.  Serving the demand hydraulically needs
// 25 m3/s of turbined flow.
[[nodiscard]] System make_anchored_system(Array<VolumeRight> vrs,
                                          Array<FlowRight> frs,
                                          Array<UserConstraint> ucs,
                                          std::string name)
{
  return {
      .name = std::move(name),
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
                          .fmax = 200.0}},
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
      .flow_right_array = std::move(frs),
      .volume_right_array = std::move(vrs),
      .user_constraint_array = std::move(ucs),
  };
}

[[nodiscard]] Simulation make_anchored_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = 24}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
}

// Laja-shaped rights: qgt (supply side of the partition), qdr (free
// usage), qde (charged usage: target-0 + negative uvalue = 50
// $/(m3/s.h) — the PLP CQVar(IQDE) encoding).
[[nodiscard]] Array<FlowRight> make_rights()
{
  return {
      {.uid = Uid {1},
       .name = "qgt",
       .direction = 1,
       .fmax = 200.0,
       .target = 0.0},
      {.uid = Uid {2},
       .name = "qdr",
       .direction = -1,
       .fmax = 200.0,
       .target = 0.0},
      {.uid = Uid {3},
       .name = "qde",
       .direction = -1,
       .fmax = 200.0,
       .target = 0.0,
       .uvalue = -50.0},
  };
}

// Ledger buckets — NO `reservoir` coupling (pure accounting).
// fcr = 0.0036 hm3/(m3/s.h): one 24 h block depletes 0.0864 hm3 per
// m3/s of extraction.  `use_state_variable = true` matches the
// emitted templates; `false` would add StorageLP's efin==eini
// closure row, pinning extraction (and via the ledger linkage the
// whole anchored partition) to zero.
[[nodiscard]] Array<VolumeRight> make_ledgers(Real eini_dr)
{
  return {
      {.uid = Uid {11},
       .name = "vdr",
       .emax = 100.0,
       .eini = eini_dr,
       .use_state_variable = true},
      {.uid = Uid {12},
       .name = "vde",
       .emax = 100.0,
       .eini = 50.0,
       .use_state_variable = true},
  };
}

[[nodiscard]] Array<UserConstraint> make_anchor_constraints()
{
  return {
      {.uid = Uid {21},
       .name = "particion",
       .expression =
           R"(flow_right("qgt").flow = flow_right("qdr").flow + flow_right("qde").flow)",
       .constraint_type = "raw"},
      {.uid = Uid {22},
       .name = "anclaje",
       .expression = R"(flow_right("qgt").flow = waterway("ww").flow)",
       .constraint_type = "raw"},
      {.uid = Uid {23},
       .name = "ledger_dr",
       .expression =
           R"(volume_right("vdr").extraction = flow_right("qdr").flow)",
       .constraint_type = "raw"},
      {.uid = Uid {24},
       .name = "ledger_de",
       .expression =
           R"(volume_right("vde").extraction = flow_right("qde").flow)",
       .constraint_type = "raw"},
  };
}

struct Solved
{
  double qgt;
  double qdr;
  double qde;
  double ww_flow;
};

[[nodiscard]] Solved solve_anchored(SystemLP& system_lp)
{
  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(lp.is_optimal());
  const auto col_sol = lp.get_col_sol();

  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto buid = t.blocks().front().uid();

  const auto& frs = system_lp.elements<FlowRightLP>();
  const auto flow_at = [&](const FlowRightLP& fr)
  { return col_sol[fr.flow_cols_at(s, t).at(buid)]; };

  const auto& ww = system_lp.elements<WaterwayLP>().front();
  return Solved {
      .qgt = flow_at(frs[0]),
      .qdr = flow_at(frs[1]),
      .qde = flow_at(frs[2]),
      .ww_flow = col_sol[ww.flow_cols_at(s, t).at(buid)],
  };
}

}  // namespace

TEST_CASE("Tier 9.1 - anchored partition equals physical turbined flow")
{
  const auto system = make_anchored_system(make_ledgers(50.0),
                                           make_rights(),
                                           make_anchor_constraints(),
                                           "Tier9_1_anchor_binds");
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto sim = make_anchored_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);

  const auto sol = solve_anchored(system_lp);

  // Demand 50 MW / pf 2.0 => 25 m3/s turbined; the anchor forces the
  // partition total onto the physical arc.
  CHECK(sol.ww_flow == doctest::Approx(25.0).epsilon(1e-6));
  CHECK(sol.qgt == doctest::Approx(sol.ww_flow).epsilon(1e-9));
  CHECK(sol.qdr + sol.qde == doctest::Approx(sol.qgt).epsilon(1e-9));
}

TEST_CASE("Tier 9.2 - usage cost steers the flow to the free category")
{
  // Ample ledgers: everything is charged to qdr (free) and none to
  // qde (50 $/(m3/s.h)) — the PLP CQVar steering behaviour.
  const auto system = make_anchored_system(make_ledgers(50.0),
                                           make_rights(),
                                           make_anchor_constraints(),
                                           "Tier9_2_steering");
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto sim = make_anchored_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);

  const auto sol = solve_anchored(system_lp);
  CHECK(sol.qdr == doctest::Approx(25.0).epsilon(1e-6));
  CHECK(sol.qde == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("Tier 9.4 - december debit: anticipado counter reduces provision")
{
  // PLP INICIOTEMP debit (genpdlajam.f:234-239): the december riego
  // provision is `rule(V) - IVGAF`.  Fixture: a december stage where
  //   * `vga` is the anticipado UP-counter carrying 5 hm3 of early
  //     spending (its eini; reset_month=september does NOT fire here),
  //   * `vdr` re-provisions via a flat bound_rule worth 100 hm3 and
  //     debits `vga` through `reset_debit_right`.
  // Expected: vdr's eini settles at 100 - 5 = 95 via the debit row
  //   `eini + vga_eini = 100`.
  const Array<VolumeRight> vrs = {
      {.uid = Uid {11},
       .name = "vga",
       .emax = 100.0,
       .eini = 5.0,
       .use_state_variable = true,
       .reset_month = MonthType::september,
       .reset_value = 0.0},
      {.uid = Uid {12},
       .name = "vdr",
       .emax = 200.0,
       .eini = 50.0,
       .use_state_variable = true,
       .reset_month = MonthType::december,
       .reset_debit_right = Uid {11},
       .bound_rule =
           RightBoundRule {
               .reservoir = Uid {1},
               .segments = {{.volume = 0.0, .slope = 0.0, .constant = 100.0}},
           }},
  };
  const auto system = make_anchored_system(vrs, {}, {}, "Tier9_4_debit");
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const Simulation sim = {
      .block_array = {{.uid = Uid {1}, .duration = 24}},
      .stage_array = {{.uid = Uid {1},
                       .first_block = 0,
                       .count_block = 1,
                       .month = MonthType::december}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(lp.is_optimal());
  const auto col_sol = lp.get_col_sol();

  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& vrs_lp = system_lp.elements<VolumeRightLP>();
  const auto vga_eini = col_sol[vrs_lp[0].eini_col_at(s, t)];
  const auto vdr_eini = col_sol[vrs_lp[1].eini_col_at(s, t)];
  CHECK(vga_eini == doctest::Approx(5.0).epsilon(1e-9));
  CHECK(vdr_eini == doctest::Approx(95.0).epsilon(1e-9));
}

TEST_CASE("Tier 9.5 - reset_value pins the up-counter to zero at reset")
{
  // The anticipado counter restarts each september (PLP INICIOANTIC,
  // genpdlajam.f:656-660): reset_value=0 overrides the config eini.
  const Array<VolumeRight> vrs = {
      {.uid = Uid {11},
       .name = "vga",
       .emax = 100.0,
       .eini = 5.0,
       .use_state_variable = true,
       .reset_month = MonthType::september,
       .reset_value = 0.0},
  };
  const auto system = make_anchored_system(vrs, {}, {}, "Tier9_5_reset_value");
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const Simulation sim = {
      .block_array = {{.uid = Uid {1}, .duration = 24}},
      .stage_array = {{.uid = Uid {1},
                       .first_block = 0,
                       .count_block = 1,
                       .month = MonthType::september}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(lp.is_optimal());
  const auto col_sol = lp.get_col_sol();
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  CHECK(col_sol[vr_lp.eini_col_at(s, t)] == doctest::Approx(0.0));
}

TEST_CASE("Tier 9.3 - depleted ledger caps its category")
{
  // vdr starts nearly empty: eini = 1.0 hm3 caps qdr at
  // 1.0 / (0.0036 * 24) = 11.5741 m3/s; the remaining
  // 13.4259 m3/s must be charged to the costed qde — the ledger
  // genuinely constrains the dispatch (hydro is still cheaper than
  // the 100 $/MWh thermal: 50 / pf = 25 $/MWh equivalent).
  const auto system = make_anchored_system(make_ledgers(1.0),
                                           make_rights(),
                                           make_anchor_constraints(),
                                           "Tier9_3_ledger_cap");
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  const auto sim = make_anchored_simulation();
  SimulationLP simulation_lp(sim, options);
  SystemLP system_lp(system, simulation_lp);

  const auto sol = solve_anchored(system_lp);
  const double qdr_cap = 1.0 / (0.0036 * 24.0);
  CHECK(sol.qdr == doctest::Approx(qdr_cap).epsilon(1e-4));
  CHECK(sol.qde == doctest::Approx(25.0 - qdr_cap).epsilon(1e-4));
  CHECK(sol.qgt == doctest::Approx(25.0).epsilon(1e-6));
}
