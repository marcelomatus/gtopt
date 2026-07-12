// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_negative_lmp_kvl.cpp — mode × channel exposure under
// mechanism M2: KVL angular coupling ("amarre angular").
//
// Fixture: a 4-bus ring (1—2—3—4—1, equal reactances, use_kirchhoff =
// true) with ALL-POSITIVE offer costs and one binding thermal limit:
//
//        g1 (c=10, 1000 MW)          d2 = 200 MW
//   bus1 ─────── l12 (1000) ─────── bus2
//    │                                │
//   l14 (1000)                      l32 (1000)
//    │                                │
//   bus4 ─────── l43 (30 MW) ─────── bus3
//                                      g3 (c=100, 1000 MW)
//
// Lossless DC solution (equal X): g1 = 160, g3 = 40; flows
// f12 = 130, f32 = 70, f14 = 30, f43 = 30 (BINDING).  The congestion
// shadow price couples the four bus duals through the loop PTDFs:
//
//   pi1 = c1 = 10        (g1 marginal)
//   pi3 = c3 = 100       (g3 marginal)
//   pi2 = (c1 + c3)/2      = 55
//   pi4 = (3*c1 - c3)/2    = -35   ← NEGATIVE with all-positive costs
//
// (Marginal +1 MW load at bus 4 relieves the binding 4→3 limit by 1/2,
// letting cheap g1 substitute expensive g3 at 1.5 : −0.5 — the classic
// counterflow / angular-coupling negative LMP.)
//
// Line l14 is the unique arbitrage corridor: pi1 + pi4 = −25 < 0, so
// dumping fictitious loss split/receiver-allocated across (bus1, bus4)
// is PROFITABLE for every loss model whose loss is not tied to |f|
// from above.  All other line pair-sums are positive.
//
// Channels exercised (see .claude/agents/lp-numerics-expert.md,
// "Line-loss arbitrage defects"):
//   (A) phantom circulation  — linear, piecewise(=bidirectional),
//       piecewise_direct: KVL does NOT block fp = fn = X (the ±x_tau
//       stamps cancel exactly).  Activation: pi_a + pi_b < −2ε.
//   (D) v-inflation          — tangent_signed_flow: v ≥ ±f only, so v
//       inflates to fmax and drags ℓ up the chord row.  Activation:
//       pi_a + pi_b < −2ε_v/(c·env) − 2ε_ℓ.
//
// Defect-documenting tests assert the defect IS present (CI stays
// green); when one starts failing, the defect was fixed and the test
// should be inverted into a regression guard.

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{
namespace negative_lmp_kvl_test
{

constexpr double kV = 100.0;
constexpr double kR = 0.5;  ///< c = R/V² = 5e-5 per line
constexpr double kX = 10.0;
constexpr double kTmax = 1000.0;
constexpr double kTmaxLimited = 30.0;  ///< l43 binding limit
constexpr double kCLoss = kR / (kV * kV);  ///< 5e-5

/// Loss-model knobs for one fixture build.
struct LossKnobs
{
  std::string_view mode = "none";
  int loss_segments = 4;
  double loss_cost_eps = 0.0;  ///< global model_options.loss_cost_eps
  int loss_secant_segments = 1;
  bool loss_use_sos2 = false;
};

struct RingFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit RingFixture(const LossKnobs& knobs)
      : system {
            .name = "NegLmpKvlRing",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                    {.uid = Uid {3}, .name = "b3",},
                    {.uid = Uid {4}, .name = "b4",},
                },
            .demand_array =
                {
                    {.uid = Uid {1},
                     .name = "d2",
                     .bus = Uid {2},
                     .capacity = 200.0,},
                },
            .generator_array =
                {
                    {.uid = Uid {1},
                     .name = "g1",
                     .bus = Uid {1},
                     .gcost = 10.0,
                     .capacity = 1000.0,},
                    {.uid = Uid {3},
                     .name = "g3",
                     .bus = Uid {3},
                     .gcost = 100.0,
                     .capacity = 1000.0,},
                },
            .line_array =
                {
                    make_line(Uid {1}, "l12", Uid {1}, Uid {2}, kTmax, knobs),
                    make_line(Uid {2}, "l32", Uid {3}, Uid {2}, kTmax, knobs),
                    make_line(
                        Uid {3}, "l43", Uid {4}, Uid {3}, kTmaxLimited, knobs),
                    make_line(Uid {4}, "l14", Uid {1}, Uid {4}, kTmax, knobs),
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1}, .first_block = 0, .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(knobs))
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_matrix_opts(knobs))
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(Uid uid,
                        std::string_view name,
                        Uid bus_a,
                        Uid bus_b,
                        double tmax,
                        const LossKnobs& knobs)
  {
    Line ln = {
        .uid = uid,
        .name = std::string(name),
        .bus_a = bus_a,
        .bus_b = bus_b,
        .voltage = kV,
        .resistance = kR,
        .reactance = kX,
        .line_losses_mode = OptName {std::string(knobs.mode)},
        .loss_segments = knobs.loss_segments,
        .tmax_ba = tmax,
        .tmax_ab = tmax,
        .capacity = tmax,
    };
    if (knobs.loss_secant_segments > 1) {
      ln.loss_secant_segments = knobs.loss_secant_segments;
      ln.loss_use_sos2 = knobs.loss_use_sos2;
    }
    return ln;
  }

  PlanningOptionsLP make_options(const LossKnobs& knobs)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = true;
    opts.model_options.kirchhoff_mode = std::string("cycle_basis");
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    if (knobs.loss_cost_eps > 0.0) {
      opts.model_options.loss_cost_eps = knobs.loss_cost_eps;
    }
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_matrix_opts(const LossKnobs& knobs)
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    if (knobs.loss_use_sos2) {
      auto& reg = SolverRegistry::instance();
      reg.load_all_plugins();
      if (reg.has_solver("cplex")) {
        bo.solver_name = "cplex";
      }
    }
    return bo;
  }
};

/// SOS2 needs a MIP backend with native SOS2 (CPLEX in this env);
/// CBC/OSI throw at LP-build time.  Same guard as
/// test_line_losses_sos2.cpp::sos2_available.
[[nodiscard]] bool sos2_available()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  return reg.has_solver("cplex");
}

[[nodiscard]] auto find_col(const LinearInterface& li, std::string_view substr)
    -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

[[nodiscard]] auto find_row(const LinearInterface& li, std::string_view substr)
    -> RowIndex
{
  RowIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

/// Sum of primal values over every column whose name contains `substr`.
[[nodiscard]] auto sum_cols(const LinearInterface& li,
                            std::span<const double> sol,
                            std::string_view substr) -> double
{
  double total = 0.0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      total += sol[static_cast<std::size_t>(value_of(idx))];
    }
  }
  return total;
}

// ── Control: the mechanism itself ────────────────────────────────────

TEST_CASE(
    "negative-LMP M2 control: binding ring limit flips the bus-4 dual "
    "negative with all-positive costs (mode none)")  // NOLINT
{
  RingFixture fix(LossKnobs {.mode = "none"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const auto duals = li.get_row_dual_raw();
  REQUIRE(duals.size() == static_cast<std::size_t>(li.get_numrows()));

  // Primal sanity: the lossless DC dispatch is g1 = 160, g3 = 40 and
  // the l43 flow sits AT its 30 MW limit.  Without the binding limit
  // the whole dual analysis below is vacuous.
  const auto g1_col = find_col(li, "generator_generation_1_");
  const auto g3_col = find_col(li, "generator_generation_3_");
  const auto f43_col = find_col(li, "line_flowp_3_");
  CHECK(sol[static_cast<std::size_t>(value_of(g1_col))]
        == doctest::Approx(160.0).epsilon(0.01));
  CHECK(sol[static_cast<std::size_t>(value_of(g3_col))]
        == doctest::Approx(40.0).epsilon(0.01));
  CHECK(std::abs(sol[static_cast<std::size_t>(value_of(f43_col))])
        == doctest::Approx(30.0).epsilon(0.01));

  // Dual pattern.  The raw dual sign convention is backend-dependent;
  // normalize against bus 1, whose LMP is pinned at +c1 = +10 by the
  // interior marginal g1.  Everything below is sign-free (ratios).
  const auto bal1 = find_row(li, "bus_balance_1_");
  const auto bal2 = find_row(li, "bus_balance_2_");
  const auto bal3 = find_row(li, "bus_balance_3_");
  const auto bal4 = find_row(li, "bus_balance_4_");
  const double pi1 = duals[static_cast<std::size_t>(value_of(bal1))];
  const double pi2 = duals[static_cast<std::size_t>(value_of(bal2))];
  const double pi3 = duals[static_cast<std::size_t>(value_of(bal3))];
  const double pi4 = duals[static_cast<std::size_t>(value_of(bal4))];
  CAPTURE(pi1);
  CAPTURE(pi2);
  CAPTURE(pi3);
  CAPTURE(pi4);
  REQUIRE(std::abs(pi1) > 0.0);

  // pi2/pi1 = (c1+c3)/(2 c1) = 5.5;  pi3/pi1 = 10;
  // pi4/pi1 = (3 c1 − c3)/(2 c1) = −3.5.
  CHECK(pi2 / pi1 == doctest::Approx(5.5).epsilon(0.02));
  CHECK(pi3 / pi1 == doctest::Approx(10.0).epsilon(0.02));
  CHECK(pi4 / pi1 == doctest::Approx(-3.5).epsilon(0.02));

  // The M2 headline: bus 4 has the OPPOSITE dual sign of bus 1 —
  // a negative LMP produced purely by KVL angular coupling.
  CHECK(pi4 * pi1 < 0.0);
}

// ── Channel A defects: phantom circulation on the l14 corridor ───────

TEST_CASE(
    "negative-LMP M2 linear mode: phantom circulation on the "
    "negative-dual corridor (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (A), mode `linear`.
  //
  // fp/fn are independent columns whose per-MW loss λ = R·fmax/V² is
  // dumped at each direction's receiver.  Circulating fp = fn = X on
  // l14 consumes λX at bus1 (pays pi1 = 10) and λX at bus4 (earns
  // |pi4| = 35): net +25·λ per circulated MW-pair, so the LP drives
  // both directions to their caps.  KVL does NOT block it: the
  // aggregates stamp +x_tau·fp − x_tau·fn, which cancels exactly.
  // Activation: pi_a + pi_b < −2·tcost/λ (tcost = 0 here).
  // Fix path: single signed flow column (tangent_signed_flow + SOS2)
  // or a direction binary; ε on losses only shifts the threshold.
  RingFixture fix(LossKnobs {.mode = "linear"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double fp14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_4_")))];
  const double fn14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flown_4_")))];
  CAPTURE(fp14);
  CAPTURE(fn14);

  // Physical net flow on l14 is ~30 MW; both directions carrying
  // hundreds of MW simultaneously is the defect.
  CHECK(std::min(fp14, fn14) > 100.0);
  // Reported loss ∝ (fp + fn); physical loss ∝ |fp − fn|.  The gap is
  // the arbitrage volume: (fp+fn) ≈ 1970 vs |net| ≈ 30 → ratio ≈ 65×.
  CHECK(fp14 + fn14 > 5.0 * std::abs(fp14 - fn14));
}

TEST_CASE(
    "negative-LMP M2 piecewise/bidirectional: circulation profitable "
    "when the dual PAIR-SUM is negative (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (A), mode `piecewise` (which
  // wraps `bidirectional` for uniform layout).
  //
  // The per-direction loss columns pay at each direction's OWN
  // receiver, which defuses circulation only when pi_a + pi_b ≥ 0
  // (the case pinned by test_line_losses_no_phantom_flow.cpp, whose
  // 3-bus loop has no negative dual).  Here pi1 + pi4 = −25 < 0, so
  // fp = fn = X earns 35·loss_n(X) at bus4 and pays 10·loss_p(X) at
  // bus1: strictly profitable, circulation runs to the caps and both
  // equality loss ladders fill completely (loss = c·env² each).
  // Activation: pi_a + pi_b < −2ε (ε = loss_cost_eps = 0 here).
  RingFixture fix(LossKnobs {.mode = "piecewise"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double fp14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_4_")))];
  const double fn14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flown_4_")))];
  const double lossp14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  const double lossn14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossn_4_")))];
  CAPTURE(fp14);
  CAPTURE(fn14);
  CAPTURE(lossp14);
  CAPTURE(lossn14);

  // Phantom circulation: both directions dispatched simultaneously.
  CHECK(std::min(fp14, fn14) > 50.0);

  // Loss consistency broken vs the NET flow: physical loss is
  // c·(fp−fn)² ≈ 0.05 MW; the booked loss is ~100 MW (both ladders
  // full at c·env² = 50 each).  Report the arbitrage volume.
  const double net = std::abs(fp14 - fn14);
  const double physical = kCLoss * net * net;
  const double booked = lossp14 + lossn14;
  CAPTURE(physical);
  CAPTURE(booked);
  CHECK(booked > 10.0);
  CHECK(booked > 100.0 * physical);

  // Sanity: only the negative-pair-sum corridor hosts the defect —
  // l32 (pi3 + pi2 = 155 > 0) stays single-direction.
  const double fp32 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_2_")))];
  const double fn32 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flown_2_")))];
  CHECK(std::min(fp32, fn32) < 0.01);
}

TEST_CASE(
    "negative-LMP M2 piecewise_direct: worst-mode phantom circulation "
    "(DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (A), mode `piecewise_direct`.
  //
  // No link row, no fp/fn aggregator: each direction's K segments
  // stamp the bus balances directly with λ_k baked in, so nothing at
  // all resists simultaneous fill of both ladders.  Self-documented as
  // the WORST mode for phantom flow (line_enums.hpp:141-149); the
  // resolver keeps it opt-in for exactly this reason
  // (line_losses.cpp:80-94).  Same activation as channel (A) above.
  RingFixture fix(LossKnobs {.mode = "piecewise_direct"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  // piecewise_direct has no aggregator columns — per-direction flow is
  // the sum of the K segment columns.
  const double sum_p = sum_cols(li, sol, "line_flowp_seg_4_");
  const double sum_n = sum_cols(li, sol, "line_flown_seg_4_");
  CAPTURE(sum_p);
  CAPTURE(sum_n);

  CHECK(std::min(sum_p, sum_n) > 50.0);
  CHECK(sum_p + sum_n > 5.0 * std::abs(sum_p - sum_n));
}

// ── Channel D: tangent_signed_flow v-inflation ────────────────────────

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow eps=0: v-inflation loss sink "
    "on the negative corridor (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (D), mode `tangent_signed_flow`.
  //
  // Phantom circulation is structurally impossible (single signed f),
  // but the |f|-aux column is bounded only from BELOW by v ≥ ±f; the
  // chord row anchors ℓ to v, not to |f|.  With split allocation each
  // bus absorbs ℓ/2, so the sink is profitable once
  //   pi_a + pi_b < −2ε_v/(c·env) − 2ε_ℓ.
  // Here pi1 + pi4 = −25 and ε = 0 → v inflates to fmax and ℓ rides
  // the chord to c·env² = 50 MW on a line carrying ~30 MW.  The code
  // comment claiming "arbitrage immunity still holds" for the chord
  // row (line_losses.cpp:1672-1675) is FALSE under negative LMPs —
  // the chord bounds ℓ by v, and nothing bounds v from above but its
  // own column cap.
  // Fix path: loss_use_sos2 + loss_secant_segments ≥ 2 (exact
  // bracket, see the SOS2 guard below) or ε sized against the worst
  // credible negative pair-sum (see the eps=1 guard below).
  RingFixture fix(LossKnobs {.mode = "tangent_signed_flow"});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double v14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flow_abs_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(v14);
  CAPTURE(loss14);

  // v decouples from |f| (the defect enabler)…
  CHECK(v14 > std::abs(f14) + 100.0);
  // …and ℓ decouples from physics: booked ℓ vs c·f².
  const double physical = kCLoss * f14 * f14;
  CAPTURE(physical);
  CHECK(loss14 > 5.0);
  CHECK(loss14 > 100.0 * physical);
}

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow eps=0.1 (plexos2gtopt default): "
    "STILL exposed at a -25 $/MWh pair-sum (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (D) threshold.
  //
  // ε is loss-priced (the v columns carry only the internal 1e-6
  // degeneracy pin), so the sink is profitable iff
  // |pi_a + pi_b|/2 > ε.  The plexos2gtopt production default
  // ε = 0.1 protects only up to a pair-sum of −0.2 $/MWh — a
  // −25 $/MWh congestion corridor blows straight through it: v
  // inflates to the envelope (the pin is no deterrent when ℓ-dumping
  // pays 12.5 $/MWh) and ℓ rides the chord to its c·env² cap.
  RingFixture fix(
      LossKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 0.1});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double v14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flow_abs_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(v14);
  CAPTURE(loss14);

  CHECK(v14 > std::abs(f14) + 100.0);
  CHECK(loss14 > 5.0);
}

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow eps=1.0 (loss-priced): below the "
    "pair-sum threshold, still exposed (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (D), loss-priced ε semantics.
  //
  // Since the 2026-07 re-point, ε lands on the LOSS column only (the
  // v columns carry the internal 1e-6 pin).  The single closure
  // condition is ε > |pi_a + pi_b|/2 = 12.5 here; ε = 1.0 is far
  // below it, so ℓ-dumping nets 11.5 $/MWh and the LP inflates v to
  // the envelope and ℓ to the c·env² cap — same exposure as ε = 0.1.
  //
  // (Pre-re-point, ε = 1.0 > ε* ≈ 0.6 used to close the v-sink via
  // the flow toll while the chord residual ℓ = c·env·|f| ≈ 32×
  // physics survived — the toll bought only partial protection at
  // ~ε $/MWh per lossy line of LMP distortion.)
  RingFixture fix(
      LossKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 1.0});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double v14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flow_abs_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(v14);
  CAPTURE(loss14);

  // The corridor still carries real flow…
  CHECK(std::abs(f14) > 10.0);
  // DEFECT: v inflates past |f| (the 1e-6 pin is no deterrent)…
  CHECK(v14 > std::abs(f14) + 100.0);
  // …and ℓ sits at the cap, far above physics.
  CHECK(loss14 > 5.0);
}

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow eps=20 > |pair-sum|/2: chord "
    "residual also closed (GUARD)")  // NOLINT
{
  // REGRESSION GUARD — full ε closure of channel (D), loss-priced.
  //
  // ε = 20 > |pi1 + pi4|/2 = 12.5: inflating ℓ anywhere above the
  // tangent lower envelope has strictly positive net cost, so the LP
  // minimises ℓ down to max_k tangent_k(f) ≤ c·f², and the internal
  // pin drops v to |f|.  This pins the single sizing rule
  // ε > ½·|worst pair-sum| — affordable since the re-point, because
  // the incidence is loss-scaled (ε·2λ per marginal MWh), not a
  // flow toll.
  RingFixture fix(
      LossKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 20.0});
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double v14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flow_abs_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(v14);
  CAPTURE(loss14);

  CHECK(std::abs(f14) > 10.0);
  CHECK(v14 <= std::abs(f14) + 0.01);
  // ℓ collapses to the tangent lower envelope: within c·f² from above.
  CHECK(loss14 <= (kCLoss * f14 * f14) + 1e-4);
}

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow: the eps increment prices "
    "losses, not flow (re-point GUARD)")  // NOLINT
{
  // REGRESSION GUARD for the 2026-07 ε re-point itself.
  //
  // Both ε = 15 and ε = 20 exceed the 12.5 $/MWh closure threshold,
  // so both dispatches are clean and identical; the objective delta
  // is therefore Δε · Σℓ_phys (a few MW of quadratic loss ⇒ tens of
  // $).  Under the pre-re-point flow-toll semantics the same delta
  // was Δε · (Σ|f| + Σℓ) ≈ 5 · 360 ≈ 1800 $ on this fixture — a
  // regression to v-pricing trips the bound immediately.
  RingFixture fix15(
      LossKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 15.0});
  auto& li15 = fix15.lp();
  REQUIRE(li15.resolve().value_or(-1) == 0);
  const double obj15 = li15.get_obj_value();

  RingFixture fix20(
      LossKnobs {.mode = "tangent_signed_flow", .loss_cost_eps = 20.0});
  auto& li20 = fix20.lp();
  REQUIRE(li20.resolve().value_or(-1) == 0);
  const double obj20 = li20.get_obj_value();

  CAPTURE(obj15);
  CAPTURE(obj20);
  // Some loss exists, so the increment is strictly positive…
  CHECK(obj20 > obj15);
  // …but it prices only the physical losses (≈ Δε·Σc·f² ≈ 15 $),
  // nowhere near the old Δε·Σ|f| ≈ 1800 $ flow toll.
  CHECK(obj20 - obj15 < 100.0);
}

TEST_CASE(
    "negative-LMP M2 tangent_signed_flow + SOS2 lambda-form: exact "
    "bracket closes the sink (GUARD, needs CPLEX)")  // NOLINT
{
  // REGRESSION GUARD — the exact fix for channel (D).
  //
  // loss_use_sos2 + loss_secant_segments = 4 replaces the v/abs/chord
  // apparatus with the lambda-form ladder (line_losses.cpp:1821-1938):
  // convexity + flow-link + chord rows over 2L+1 breakpoint weights,
  // SOS2 adjacency.  The chord then equals the SECANT on the segment
  // containing f — an upper bound tied to |f| itself, with worst-case
  // slack c·w²/4 (w = env/L).  No ε needed, valid for ANY price sign.
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 guard — no SOS2-capable backend loaded");
    return;
  }

  constexpr int L = 4;
  RingFixture fix(LossKnobs {.mode = "tangent_signed_flow",
                             .loss_secant_segments = L,
                             .loss_use_sos2 = true});
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 4);  // one per (line, block)

  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto sol = li.get_col_sol_raw();
  const double f14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flows_4_")))];
  const double loss14 =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_4_")))];
  CAPTURE(f14);
  CAPTURE(loss14);

  // True bracket: tangent envelope ≤ ℓ ≤ secant with ≤ c·w²/4 slack.
  const double w = kTmax / static_cast<double>(L);
  const double bracket_slack = kCLoss * w * w / 4.0;
  CHECK(std::abs(f14) > 10.0);
  CHECK(loss14 <= (kCLoss * f14 * f14) + bracket_slack + 1e-4);
  // Concretely: the 50 MW sink of the eps=0 defect test is closed.
  CHECK(loss14 < 1.0);
}

}  // namespace negative_lmp_kvl_test
}  // namespace
