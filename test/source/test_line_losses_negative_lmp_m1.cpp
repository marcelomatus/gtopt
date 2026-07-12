// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_negative_lmp_m1.cpp — channel-activation calibration
// under mechanism M1 (negative-cost injection), the deterministic
// control mechanism of the negative-LMP fixture ladder.
//
// Fixture: 2-bus, one lossy line, a negative-cost generator at bus 2:
//
//   bus1 (g1 c=+10 cap 500, d1 = 100) ── l12 (200 MW, R=0.5, V=100,
//   bus2 (g2 c=−100 cap 500, d2 = 50)     K=4, c = R/V² = 5e-5)
//
// Lossless solution: g2 = 150 serves d2 and exports 100 to bus1;
// g2 stays interior, so BOTH bus duals are exactly −100 (the
// negative-cost unit is marginal at both ends).  This gives clean,
// deterministic activation arithmetic for the channels:
//
//   (A) phantom circulation — per-direction losses earn |pi_recv| =
//       100 $/MWh at BOTH receivers → exposed for any
//       loss_cost_eps < 100; threshold ε* = |pi_recv|.
//   (B) top-down segment fill — the uniform-layout EQUALITY loss row
//       fixes loss = Σ λ_k·seg_k but not WHICH segments fill; with a
//       negative receiver the LP fills the steepest first, booking up
//       to (2K−1)× the true loss at low flow.
//   (C) midpoint free sink — the midpoint layout's loss row is `≥`
//       only, so any pi_recv < −ε pins ℓ at its column cap c·env²
//       INDEPENDENT of flow.
//   (C') tangent-layout shared path — same shape as (C): the legacy
//       shared loss column has only tangent `≥` rows below and the
//       c·env² column cap above.
//
// M1 coverage for `tangent_signed_flow` (channel D) already exists in
// test_line_losses_tangent_signed_flow.cpp:1002 ("chord UB never
// exceeds the loose constant bound") — not duplicated here.  The
// M2/M3 mechanisms are covered by test_line_losses_negative_lmp_kvl.cpp
// and test_line_losses_negative_lmp_reserve.cpp on a 4-bus ring.
//
// Defect-documenting tests assert the defect IS present (CI stays
// green); when one starts failing, the defect was fixed and the test
// should be inverted into a regression guard.

#include <algorithm>
#include <cmath>
#include <format>
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
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{
namespace negative_lmp_m1_test
{

constexpr double kV = 100.0;
constexpr double kR = 0.5;
constexpr double kTmax = 200.0;
constexpr double kCLoss = kR / (kV * kV);  ///< 5e-5
constexpr int kSegs = 4;
constexpr double kSegWidth = kTmax / kSegs;  ///< 50 MW
/// Per-direction loss-column cap: c·env² = 2 MW.
constexpr double kLossCap = kCLoss * kTmax * kTmax;

struct TwoBusNegFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit TwoBusNegFixture(std::string_view mode_name,
                            double g2_cost = -100.0,
                            std::string_view pwl_layout = "",
                            double loss_cost_eps = 0.0)
      : system {
            .name = "NegLmpM1TwoBus",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                },
            .demand_array =
                {
                    {.uid = Uid {1},
                     .name = "d1",
                     .bus = Uid {1},
                     .capacity = 100.0,},
                    {.uid = Uid {2},
                     .name = "d2",
                     .bus = Uid {2},
                     .capacity = 50.0,},
                },
            .generator_array =
                {
                    {.uid = Uid {1},
                     .name = "g1",
                     .bus = Uid {1},
                     .gcost = 10.0,
                     .capacity = 500.0,},
                    {.uid = Uid {2},
                     .name = "g2",
                     .bus = Uid {2},
                     .gcost = g2_cost,
                     .capacity = 500.0,},
                },
            .line_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "l12",
                        .bus_a = Uid {1},
                        .bus_b = Uid {2},
                        .voltage = kV,
                        .resistance = kR,
                        .line_losses_mode = OptName {std::string(mode_name)},
                        .loss_segments = kSegs,
                        .loss_pwl_layout = pwl_layout.empty()
                            ? OptName {}
                            : OptName {std::string(pwl_layout)},
                        .tmax_ba = kTmax,
                        .tmax_ab = kTmax,
                        .capacity = kTmax,
                    },
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1}, .first_block = 0, .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(loss_cost_eps))
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_matrix_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  PlanningOptionsLP make_options(double loss_cost_eps)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    if (loss_cost_eps > 0.0) {
      opts.model_options.loss_cost_eps = loss_cost_eps;
    }
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_matrix_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    return bo;
  }
};

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

[[nodiscard]] auto count_cols_containing(const LinearInterface& li,
                                         std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] auto count_rows_containing(const LinearInterface& li,
                                         std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

/// Uniform-layout chord slope of segment k (1-based): λ_k = w·(2k−1)·c.
[[nodiscard]] constexpr double lambda_k(int k)
{
  return kSegWidth * static_cast<double>((2 * k) - 1) * kCLoss;
}

/// Minimal (bottom-up) PWL loss for a flow f on the uniform ladder —
/// the value an honest LP books when losses are penalized.
[[nodiscard]] double bottom_up_loss(double flow)
{
  double loss = 0.0;
  double remaining = flow;
  for (int k = 1; k <= kSegs && remaining > 0.0; ++k) {
    const double seg = std::min(remaining, kSegWidth);
    loss += lambda_k(k) * seg;
    remaining -= seg;
  }
  return loss;
}

// ── Control: the mechanism itself ─────────────────────────────────────

TEST_CASE(
    "negative-LMP M1 control: negative-cost injection flips both bus "
    "duals (mode none)")  // NOLINT
{
  // Anchor run with all-positive costs pins the raw dual sign
  // convention: g1 (c=+10) is marginal, so sign(pi1_anchor) is the
  // "+10 $/MWh" direction.
  TwoBusNegFixture anchor("none", /*g2_cost=*/+100.0);
  auto& li_a = anchor.lp();
  REQUIRE(li_a.resolve().has_value());
  const auto duals_a = li_a.get_row_dual_raw();
  const double pi1_anchor = duals_a[static_cast<std::size_t>(
      value_of(find_row(li_a, "bus_balance_1_")))];
  REQUIRE(std::abs(pi1_anchor) > 0.0);

  // Negative-cost run: g2 (c=−100) is interior-marginal at both ends
  // of the lossless line, so pi1 = pi2 = −100 exactly.
  TwoBusNegFixture fix("none");
  auto& li = fix.lp();
  REQUIRE(li.resolve().has_value());
  const auto duals = li.get_row_dual_raw();
  const double pi1 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_1_")))];
  const double pi2 =
      duals[static_cast<std::size_t>(value_of(find_row(li, "bus_balance_2_")))];
  CAPTURE(pi1_anchor);
  CAPTURE(pi1);
  CAPTURE(pi2);

  CHECK(pi1 * pi1_anchor < 0.0);
  CHECK(pi2 * pi1_anchor < 0.0);
  // |pi| = 100 = 10 × the anchor's marginal cost 10.
  CHECK(std::abs(pi2 / pi1_anchor) == doctest::Approx(10.0).epsilon(0.02));
  CHECK(std::abs(pi1 / pi1_anchor) == doctest::Approx(10.0).epsilon(0.02));
}

// ── Channels A + B on the uniform (default) layout ────────────────────

TEST_CASE(
    "negative-LMP M1 piecewise uniform: phantom circulation + top-down "
    "segment fill (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channels (A) and (B), mode `piecewise`
  // (= bidirectional wrap), uniform layout, ε = 0.
  //
  // (A): both receivers price at −100, so per-direction losses are
  //      revenue at BOTH ends: fp = fn = X circulation runs to the
  //      caps.  Net flow is pinned at ~100 MW (2→1 import), so
  //      fn → 200 (cap) and fp → ~98.
  // (B): the equality loss row leaves the SEGMENT CHOICE free; with a
  //      negative receiver the LP fills the steepest segments first.
  //      At fp ≈ 98 = 2w the top-down fill {seg4, seg3} books 3× the
  //      bottom-up chord — the (2K−1)/K-style inflation at partial
  //      fill.  Fill-order inversion (top segment full while segment 1
  //      is empty) is the structural fingerprint.
  // Activation: pi_recv < −ε at the direction's receiver.
  TwoBusNegFixture fix("piecewise");
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double fp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_1_0_")))];
  const double fn =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flown_1_0_")))];
  const double lossp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_1_")))];
  const double lossn =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossn_1_")))];
  CAPTURE(fp);
  CAPTURE(fn);
  CAPTURE(lossp);
  CAPTURE(lossn);

  // (A) phantom circulation: both directions dispatched.
  CHECK(std::min(fp, fn) > 50.0);

  // (B) fill-order inversion on the A→B ladder: the steepest segment
  // is full while the flattest is empty.
  const auto seg_val = [&](int k)
  {
    const auto col = find_col(li, std::format("line_flowp_seg_1_0_1_1_{}", k));
    return sol[static_cast<std::size_t>(value_of(col))];
  };
  const double seg1 = seg_val(1);
  const double seg4 = seg_val(kSegs);
  CAPTURE(seg1);
  CAPTURE(seg4);
  CHECK(seg4 > kSegWidth - 1.0);
  CHECK(seg1 < 1.0);

  // (B) booked loss ≥ 2× the honest bottom-up chord at the same flow.
  const double honest = bottom_up_loss(fp);
  CAPTURE(honest);
  CHECK(lossp > 2.0 * honest);

  // Arbitrage volume: booked (lossp + lossn) vs physical c·net².
  const double net = std::abs(fp - fn);
  const double physical = kCLoss * net * net;
  CAPTURE(physical);
  CHECK(lossp + lossn > 5.0 * physical);
}

// ── Channel A ε-threshold calibration ─────────────────────────────────

TEST_CASE(
    "negative-LMP M1 loss_cost_eps calibration: threshold is "
    "|pi_recv|, not the degeneracy-scale 1e-6")  // NOLINT
{
  // Channel (A) activation is pi_a + pi_b < −2ε per circulated
  // MW-pair, i.e. per-direction the sink earns |pi_recv| = 100 $/MWh
  // against a tax of ε on each loss column.  The production-
  // recommended ε = 1e-6 (line.hpp `loss_cost_eps`) cures LP
  // DEGENERACY (positive-price networks, cf.
  // test_line_losses_strict_direction.cpp) but is 8 orders of
  // magnitude below the true-arbitrage threshold.

  SUBCASE("eps = 1e-6 (degeneracy cure): still EXPOSED under M1")
  {
    // DEFECT-DOCUMENTING SUBCASE.
    TwoBusNegFixture fix("piecewise", -100.0, "", /*loss_cost_eps=*/1e-6);
    auto& li = fix.lp();
    REQUIRE(li.resolve().has_value());
    const auto sol = li.get_col_sol_raw();
    const double fp = sol[static_cast<std::size_t>(
        value_of(find_col(li, "line_flowp_1_0_")))];
    const double fn = sol[static_cast<std::size_t>(
        value_of(find_col(li, "line_flown_1_0_")))];
    CAPTURE(fp);
    CAPTURE(fn);
    CHECK(std::min(fp, fn) > 50.0);
  }

  SUBCASE("eps = 150 > |pi_recv| = 100: circulation closed (GUARD)")
  {
    // REGRESSION GUARD — ε sized above the worst credible negative
    // price kills the channel (at the cost of taxing real losses).
    TwoBusNegFixture fix("piecewise", -100.0, "", /*loss_cost_eps=*/150.0);
    auto& li = fix.lp();
    REQUIRE(li.resolve().has_value());
    const auto sol = li.get_col_sol_raw();
    const double fp = sol[static_cast<std::size_t>(
        value_of(find_col(li, "line_flowp_1_0_")))];
    const double fn = sol[static_cast<std::size_t>(
        value_of(find_col(li, "line_flown_1_0_")))];
    const double lossn =
        sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossn_1_")))];
    CAPTURE(fp);
    CAPTURE(fn);
    CAPTURE(lossn);

    // Complementarity restored: the import direction only.
    CHECK(fp < 0.01);
    CHECK(fn > 99.0);
    // Loss back on the honest bottom-up chord of the real flow.
    CHECK(lossn <= bottom_up_loss(fn) + 0.01);
  }
}

// ── Channel C: midpoint free sink ─────────────────────────────────────

TEST_CASE(
    "negative-LMP M1 midpoint layout: loss column pinned at its cap "
    "independent of flow (DEFECT)")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (C), mode `piecewise` +
  // `loss_pwl_layout = midpoint`, ε = 0.
  //
  // The midpoint de-bias turns the loss row into an INEQUALITY
  // `loss ≥ Σ λ_k·seg_k − offset` (line_losses.cpp add_direction,
  // debias branch), so nothing ties ℓ from above except its column
  // cap c·env².  Any pi_recv < −ε pins ℓ = cap regardless of the
  // dispatched flow — the documented ~6× system-wide `loss_sol`
  // inflation on CEN PCP v0407 (line_enums.hpp:289-300).
  // Activation: pi_recv < −ε.
  TwoBusNegFixture fix("piecewise", -100.0, "midpoint");
  auto& li = fix.lp();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double fp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_1_0_")))];
  const double lossp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_1_")))];
  const double lossn =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossn_1_")))];
  CAPTURE(fp);
  CAPTURE(lossp);
  CAPTURE(lossn);

  // Both per-direction loss columns sit at the cap (both receivers
  // negative): a 4 MW booked loss on a line whose physical worst case
  // is 2 MW at FULL rating.
  CHECK(lossp > 0.95 * kLossCap);
  CHECK(lossn > 0.95 * kLossCap);
}

// ── Channel C-analog: legacy shared path (tangent layout) ─────────────

TEST_CASE(
    "negative-LMP M1 piecewise+tangent layout: shared loss column is a "
    "cap sink (DEFECT) and keeps the legacy 3-col LP shape")  // NOLINT
{
  // DEFECT-DOCUMENTING TEST — channel (C-analog), the only remaining
  // user of the legacy shared-loss path `add_piecewise_shared`
  // (line_losses.cpp:2163: layout `tangent` bypasses the
  // bidirectional wrap).
  //
  // The shared ℓ has K tangent rows BELOW it (`loss ≥ 2c·t_k(fp+fn) −
  // c·t_k²`) and only the column cap c·env² above: with the receiver
  // at −100 the LP pins ℓ = cap.  Same activation as (C).
  TwoBusNegFixture fix("piecewise", -100.0, "tangent");
  auto& li = fix.lp();

  // Structural pin (uncovered elsewhere): 3 line columns (fp, fn,
  // shared lossp — NO per-segment columns, NO lossn) and K tangent
  // rows (NO flow_link / loss-equality rows).
  CHECK(count_cols_containing(li, "line_flowp_seg_") == 0);
  CHECK(count_cols_containing(li, "line_flown_seg_") == 0);
  CHECK(count_cols_containing(li, "line_flowp_") == 1);
  CHECK(count_cols_containing(li, "line_flown_") == 1);
  CHECK(count_cols_containing(li, "line_lossp_") == 1);
  CHECK(count_cols_containing(li, "line_lossn_") == 0);
  CHECK(count_rows_containing(li, "line_loss_link_") == kSegs);
  CHECK(count_rows_containing(li, "line_flow_link_") == 0);

  auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto sol = li.get_col_sol_raw();
  const double fp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flowp_1_0_")))];
  const double fn =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_flown_1_0_")))];
  const double lossp =
      sol[static_cast<std::size_t>(value_of(find_col(li, "line_lossp_1_")))];
  CAPTURE(fp);
  CAPTURE(fn);
  CAPTURE(lossp);

  CHECK(lossp > 0.95 * kLossCap);
  // The sink decouples from physics: booked ℓ (= the 2 MW cap) vs the
  // honest c·net² ≈ 0.5 MW at the ~100 MW net flow — a 4× inflation
  // pinned at the column bound regardless of dispatch.
  const double net = std::abs(fp - fn);
  CHECK(lossp > 3.0 * kCLoss * net * net);
}

}  // namespace negative_lmp_m1_test
}  // namespace
