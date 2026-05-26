// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the LineLP soft (Normal) flow limit + overload penalty
// feature.  Mirrors UC.jl's `Normal flow limit (MW)` + `Flow limit
// penalty ($/MW)` pair and PLEXOS's `Normal Rating` + overload-penalty
// convention.  See `gtopt::Line::tmax_normal_ab` /
// `tmax_normal_ba` / `overload_penalty` for the data contract.

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/uid.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Build a 2-bus system with a single line.  Bus 1 hosts a cheap
// generator (`gcost=10`), bus 2 hosts an expensive backup
// (`gcost=200`) plus the demand.  The line carries A→B flow; we
// drive the test by varying demand to force the LP into the
// soft / hard region.
struct TwoBusFixture
{
  Array<Bus> bus_array {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {2},
          .gcost = 200.0,
          .capacity = 500.0,
      },
  };
  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 80.0,
      },
  };
  Array<Line> line_array {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
      },
  };
  Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
};

PlanningOptions make_opts()
{
  PlanningOptions opts;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.scale_objective = 1.0;  // 1:1 so obj checks are easy
  opts.model_options.demand_fail_cost = 1e6;  // dominate any other cost
  return opts;
}

}  // namespace

TEST_CASE("LineLP soft cap - inert when overload_penalty is unset")
{
  // Pure hard-cap baseline.  Demand 80 MW < tmax 100 MW, line carries
  // 80 MW (well under the soft threshold we'll set).  Without
  // `overload_penalty`, the LP must emit no overload columns
  // regardless of `tmax_normal_ab`.
  TwoBusFixture fix;
  fix.line_array[0].tmax_normal_ab = 50.0;  // present but inert
  fix.line_array[0].tmax_normal_ba = 50.0;

  const System system {
      .name = "SoftCapInert",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 80 MW served by g1 at $10/MWh ⇒ obj = 800.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(800.0));

  // No overload columns emitted.
  const auto& line_lps = system_lp.elements<LineLP>();
  REQUIRE(line_lps.size() == 1);
  const auto& line_lp = line_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  CHECK(line_lp.overloadp_cols_at(scenario_lp, stage_lp).empty());
  CHECK(line_lp.overloadn_cols_at(scenario_lp, stage_lp).empty());
}

TEST_CASE("LineLP soft cap - penalty applied above soft threshold")
{
  // Hard cap 100, soft cap 50, penalty $1000/MWh.  Drive 80 MW
  // across the line.  We disable bus_b's local generator (capacity
  // 0) so the only way to serve the 80 MW demand is via g1 across
  // the line; the LP must pay the penalty for the 30 MW above the
  // soft cap.  Expected:
  //   flowp = 80,  overloadp = max(0, 80 − 50) = 30
  //   obj_line_cost = 30 * 1000 = 30_000
  //   gen cost      = 80 * 10   = 800
  //   total obj     = 30_800
  TwoBusFixture fix;
  fix.generator_array[1].capacity = 0.0;  // disable backup at bus_b
  fix.line_array[0].tmax_normal_ab = 50.0;
  fix.line_array[0].tmax_normal_ba = 50.0;
  fix.line_array[0].overload_penalty = 1000.0;

  const System system {
      .name = "SoftCapBinding",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  CHECK(lp.get_obj_value_raw() == doctest::Approx(30'800.0));

  // overloadp_cols must be present and equal 30.0 at block 1.
  const auto& line_lps = system_lp.elements<LineLP>();
  REQUIRE(line_lps.size() == 1);
  const auto& line_lp = line_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& opc = line_lp.overloadp_cols_at(scenario_lp, stage_lp);
  REQUIRE_FALSE(opc.empty());
  const auto it = opc.find(make_uid<Block>(1));
  REQUIRE(it != opc.end());
  CHECK(lp.get_col_sol()[it->second] == doctest::Approx(30.0));

  // Negative direction sees no flow (and so no overload usage).
  const auto& onc = line_lp.overloadn_cols_at(scenario_lp, stage_lp);
  if (!onc.empty()) {
    const auto nit = onc.find(make_uid<Block>(1));
    REQUIRE(nit != onc.end());
    CHECK(lp.get_col_sol()[nit->second] == doctest::Approx(0.0));
  }
}

TEST_CASE("LineLP soft cap - hard cap still binds above tmax_normal")
{
  // Demand 120 > hard cap 100.  Even with the soft cap active, the
  // LP cannot exceed the hard cap.  Expected:
  //   flowp        = 100 (hard cap binds)
  //   overloadp    = 100 − 50 = 50 (the full slack range)
  //   curtailment  = 120 − 100 = 20 MW @ fail_cost 1e6/MWh
  //   gen cost     = 100 * 10 = 1_000
  //   overload     = 50 * 1000 = 50_000
  //   fail cost    = 20 * 1e6 = 20_000_000
  //   total obj    = 20_051_000
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  // Force the demand so the LP cannot opt to curtail cheaply — UC.jl
  // semantics: penalty is only an alternative when curtailment is
  // also costly.  Here `demand_fail_cost = 1e6` ensures the LP
  // prefers to pay the overload penalty over curtailment.
  fix.line_array[0].tmax_normal_ab = 50.0;
  fix.line_array[0].tmax_normal_ba = 50.0;
  fix.line_array[0].overload_penalty = 1000.0;
  // Lower g2 to 200 so the LP must use g1 + the line + accept
  // curtailment for the residual.  g2 is at bus_b so it doesn't
  // need the line; demand is 120 but g2 only has 0 capacity here
  // to force the line saturation case.
  fix.generator_array[1].capacity = 0.0;

  const System system {
      .name = "HardCapAboveSoft",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  CHECK(lp.get_obj_value_raw() == doctest::Approx(20'051'000.0));

  const auto& line_lps = system_lp.elements<LineLP>();
  REQUIRE(line_lps.size() == 1);
  const auto& line_lp = line_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& opc = line_lp.overloadp_cols_at(scenario_lp, stage_lp);
  REQUIRE_FALSE(opc.empty());
  const auto it = opc.find(make_uid<Block>(1));
  REQUIRE(it != opc.end());
  // overloadp saturates at (tmax − tmax_normal) = 50.
  CHECK(lp.get_col_sol()[it->second] == doctest::Approx(50.0));
}

// ─── Per-(stage, block) tcost / overload_penalty ─────────────────────────
//
// ``Line.tcost`` and ``Line.overload_penalty`` were promoted from
// ``OptTRealFieldSched`` (per-stage) to ``OptTBRealFieldSched``
// (per-(stage, block)) on 2026-05-18.  Scalars still broadcast across
// every (stage, block); 2-D arrays now bind per block.  These tests pin
// the new contract on a single-stage / single-block fixture so the LP
// build path is exercised without per-block variation (covered at the
// FieldSched level by ``test_schedule.cpp``'s per-block doctests).

TEST_CASE("LineLP — tcost scalar broadcasts to every block")
{
  // Scalar tcost = 0.5 — every block sees the same cost.  The LP
  // accepts the scalar form unchanged after the T→TB promotion.
  TwoBusFixture fix;
  fix.line_array[0].tcost = 0.5;
  // No overload penalty — pure hard cap path.

  const System system {
      .name = "LineTcostScalar",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 80 MW × ($10 generation + $0.5 tcost) = $840.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(840.0));
}

TEST_CASE("LineLP — overload_penalty scalar broadcasts to every block")
{
  // Pin the (existing) scalar broadcast for overload_penalty after the
  // T→TB promotion.  Soft cap binds in block 1, penalty 1000 × 30 MW.
  TwoBusFixture fix;
  fix.generator_array[1].capacity = 0.0;  // disable bus_b backup
  fix.line_array[0].tmax_normal_ab = 50.0;
  fix.line_array[0].tmax_normal_ba = 50.0;
  fix.line_array[0].overload_penalty = 1000.0;  // scalar — broadcasts

  const System system {
      .name = "LineOverloadPenaltyScalar",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Identical to "penalty applied above soft threshold" obj = 30,800.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(30'800.0));
}

// =============================================================
// Line.enforce_level (PLEXOS "Enforce Limits" mirror) — three modes:
//   0 = never enforce (line capacity not binding; tmax_ab kept only
//       for loss-segment discretization)
//   1 = voltage-conditional (in our LP treated as hard cap, since
//       we have no AC voltage-feasibility iteration)
//   2 = always enforce (hard cap — the historical default)
// =============================================================

TEST_CASE("LineLP enforce_level=2 (default) — hard cap binds")
{
  // Demand 120 MW > tmax 100 MW.  With enforce_level=2 (default the
  // schema implicit) the LP must hit the cap, leaving 20 MW
  // unserved → 20_000_000 demand-fail at $1e6/MWh dominates.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;  // no backup at bus_b
  // enforce_level not set → default 2.

  const System system {
      .name = "EnforceLevel2HardCap",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 100 MW served via line (gen cost = 100 × 10 = 1000)
  // 20 MW unserved at fail_cost 1e6 = 20_000_000
  // obj = 1000 + 20_000_000 = 20_001_000
  CHECK(lp.get_obj_value_raw() == doctest::Approx(20'001'000.0));
}

TEST_CASE("LineLP enforce_level=0 — cap not binding, full demand served")
{
  // Same scenario as above but with enforce_level=0.  The hard cap
  // is relaxed; the LP must be able to push the full 120 MW across
  // the line, serving all demand and avoiding the 20-MW
  // demand-fail.  tmax_ab=100 is kept on the JSON for loss-segment
  // discretization but doesn't bind on the flow column.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;
  fix.line_array[0].enforce_level = 0;

  const System system {
      .name = "EnforceLevel0Unbounded",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 120 MW served via g1 (cost = 120 × 10 = 1200), zero unserved.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(1200.0));
}

TEST_CASE("LineLP enforce_level=1 — same hard-cap behaviour as level=2")
{
  // In our LP (no AC voltage iteration), level 1 is treated the same
  // as level 2.  This guards against the regression where an
  // off-by-one in the threshold check (e.g. ``> 1`` instead of
  // ``>= 1``) would have made level 1 behave like level 0.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;
  fix.line_array[0].enforce_level = 1;

  const System system {
      .name = "EnforceLevel1HardCap",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Identical to enforce_level=2: 100 MW carried, 20 MW unserved.
  CHECK(lp.get_obj_value_raw() == doctest::Approx(20'001'000.0));
}

TEST_CASE(
    "LineLP enforce_level=0 with piecewise losses — segments also relaxed")
{
  // Regression test for the Capricornio-style lift on lossy lines.
  // In piecewise loss mode each segment column has ``.uppb =
  // seg_width = block_tmax_ab / nseg`` and the linkrow says
  // ``fp + fn − Σ seg_k = 0``.  Without lifting the segment caps
  // too, the aggregator still binds to the total rating via the
  // segment sum.  The wired ``enforce_capacity`` flag in
  // ``line_losses::add_block`` propagates the relaxation to BOTH
  // the directional flow columns AND the per-segment columns while
  // keeping ``seg_width`` (and hence the loss-row coefficients)
  // finite to avoid blowing up solver numerics.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;
  fix.line_array[0].enforce_level = 0;
  // Enable piecewise losses with 3 segments (CEN PCP default).
  fix.line_array[0].resistance = 0.01;
  fix.line_array[0].voltage = 100.0;
  fix.line_array[0].line_losses_mode = std::string {"piecewise"};
  fix.line_array[0].loss_segments = 3;

  const System system {
      .name = "EnforceLevel0PiecewiseLosses",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With losses, g1 must send (120 + loss) MW to serve 120 MW at
  // bus_b.  With R=0.01 / V=100 the loss is negligible (≤ 0.01 MW)
  // so total gen ≈ 120 × 10 = 1200, plus a tiny loss premium.
  // Critically: NO demand-fail cost (otherwise obj would be in the
  // millions).
  CHECK(lp.get_obj_value_raw() < 5000.0);
}

TEST_CASE("LineLP enforce_level=0 with linear losses")
{
  // Same setup but linear loss model (lossfactor explicitly set
  // without resistance + voltage triggering piecewise downgrade).
  // The directional flow columns must be released so the LP can
  // dispatch the full 120 MW + linear loss.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;
  fix.line_array[0].enforce_level = 0;
  fix.line_array[0].lossfactor = 0.001;  // 0.1 % linear loss
  fix.line_array[0].line_losses_mode = std::string {"linear"};

  const System system {
      .name = "EnforceLevel0LinearLosses",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // gen cost ≈ 120 × (1 + lossfactor) × 10 ≈ 1201; no fail cost.
  CHECK(lp.get_obj_value_raw() < 5000.0);
}

TEST_CASE("LineLP enforce_level=2 with piecewise losses — hard cap still binds")
{
  // Regression guard: when enforce_level=2 (default), the piecewise
  // segments MUST still cap the flow at the rating.  This catches
  // a bug where the ``enforce_capacity`` plumbing accidentally
  // releases the segment bounds for level=2 too.
  TwoBusFixture fix;
  fix.demand_array[0].capacity = 120.0;
  fix.generator_array[1].capacity = 0.0;
  fix.line_array[0].enforce_level = 2;  // explicit hard cap
  fix.line_array[0].resistance = 0.01;
  fix.line_array[0].voltage = 100.0;
  fix.line_array[0].line_losses_mode = std::string {"piecewise"};
  fix.line_array[0].loss_segments = 3;

  const System system {
      .name = "EnforceLevel2PiecewiseHardCap",
      .bus_array = fix.bus_array,
      .demand_array = fix.demand_array,
      .generator_array = fix.generator_array,
      .line_array = fix.line_array,
  };

  const PlanningOptionsLP options(make_opts());
  SimulationLP simulation_lp(fix.simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // 100 MW carried (capped), 20 MW unserved → 20 × 1e6 = 2e7
  // plus 100 × 10 ≈ 1000 gen cost.
  CHECK(lp.get_obj_value_raw() > 1.9e7);
  CHECK(lp.get_obj_value_raw() < 2.1e7);
}
