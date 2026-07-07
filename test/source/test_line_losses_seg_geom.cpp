// SPDX-License-Identifier: BSD-3-Clause
/// @file test_line_losses_seg_geom.cpp
/// @brief Pin the per-segment geometry of the static PWL line-loss
///        approximation (``loss_segment_geometry``).
///
/// Regression guard for the seg_width/envelope bug that briefly
/// shipped: ``seg_geom`` expected the FULL envelope, but the caller
/// was passing ``seg_width = envelope/K`` by mistake, so every slope
/// came out a factor of ``1/K`` too small.  The LP underestimated
/// loss accordingly; the bug was silent for K = 4–6 (artificially
/// cheap obj), but at K = 10 the loss coefficients fell below the
/// ``kLossCoeffTolerance = 1e-6`` cutoff and the loss row degenerated
/// → LP-infeasibility.  These tests pin the math so the same shape
/// of bug can't recur silently.

#include <doctest/doctest.h>
#include <gtopt/line_enums.hpp>
#include <gtopt/line_losses.hpp>

using namespace gtopt;

namespace
{

constexpr double kEps = 1e-12;

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE("loss_segment_geometry: uniform partition with K=1 = whole envelope")
{
  // K=1 ⇒ one segment covers [0, B].  Chord slope on [0, B] of f² is
  // (B² − 0²) / (B − 0) = B.  So with envelope B = 100:
  //   width_1 = 100, slope_1 = 100.
  const auto g =
      line_losses::loss_segment_geometry(100.0, 1, 1, LinePwlLayout::uniform);
  CHECK(g.width == doctest::Approx(100.0).epsilon(kEps));
  CHECK(g.slope == doctest::Approx(100.0).epsilon(kEps));
}

TEST_CASE("loss_segment_geometry: uniform partition slope formula = w·(2k−1)")
{
  // For uniform breakpoints on a convex quadratic, the secant chord
  // slope on segment k is (b_k² − b_{k−1}²)/(b_k − b_{k−1})
  //                     = b_k + b_{k−1}
  //                     = w·k + w·(k−1)
  //                     = w·(2k−1)
  // with w = B/K.  This is what add_segments stamps as the per-MW
  // loss coefficient (× R/V²).
  constexpr double B = 200.0;
  for (const int K : {2, 3, 5, 10}) {
    const double w_expected = B / K;
    for (int k = 1; k <= K; ++k) {
      const auto g =
          line_losses::loss_segment_geometry(B, K, k, LinePwlLayout::uniform);
      CAPTURE(K);
      CAPTURE(k);
      const double slope_expected = w_expected * ((2 * k) - 1);
      CHECK(g.width == doctest::Approx(w_expected).epsilon(kEps));
      CHECK(g.slope == doctest::Approx(slope_expected).epsilon(kEps));
    }
  }
}

TEST_CASE("loss_segment_geometry: PWL re-sums to true quadratic at endpoints")
{
  // Chord on segment [a, b] of f² is exact at both endpoints — by
  // construction, secant(b_k) = b_k².  Summing the per-segment loss
  // contribution Σ_k slope_k · width_k from segment 1 up to segment K
  // gives Σ_k (b_k + b_{k-1})·(b_k − b_{k-1}) = Σ_k (b_k² − b_{k-1}²)
  // = b_K² = B².  Test this telescoping for a couple of K values.
  constexpr double B = 50.0;
  for (const int K : {2, 4, 7}) {
    double total = 0.0;
    for (int k = 1; k <= K; ++k) {
      const auto g =
          line_losses::loss_segment_geometry(B, K, k, LinePwlLayout::uniform);
      total += g.slope * g.width;
    }
    CAPTURE(K);
    CHECK(total == doctest::Approx(B * B).epsilon(kEps));
  }
}

TEST_CASE(  // NOLINT
    "loss_segment_geometry: regression — slopes are NOT 1/K too small")
{
  // The seg_width/envelope bug made every slope come out as
  // slope = (B/K) · (2k−1) / K       (off by 1/K)
  // instead of
  // slope = (B/K) · (2k−1)            (correct)
  //
  // Pin a specific value the buggy code would have produced and
  // assert we don't return it.  For B=100, K=10, k=1:
  //   correct slope = (100/10) · 1 = 10
  //   buggy slope   = (10/10) · 1  = 1
  // A 10× delta is impossible to miss even with floating-point noise.
  const auto g =
      line_losses::loss_segment_geometry(100.0, 10, 1, LinePwlLayout::uniform);
  CHECK(g.width == doctest::Approx(10.0).epsilon(kEps));
  CHECK(g.slope == doctest::Approx(10.0).epsilon(kEps));
  // Buggy value would be 1.0 — assert we're at least 5× above that
  // (catches any future regression that re-introduces a 1/K scaling).
  CHECK(g.slope > 5.0);
}

TEST_CASE("loss_segment_geometry: equal_error aliases to uniform")
{
  // For a convex quadratic, the chord-error minimax IS the uniform
  // partition (chord error ∝ width², minimized by equal widths).
  // The ``equal_error`` mode is a documented alias for ``uniform``
  // until a meaningful weighted-by-flow-distribution variant lands;
  // pin that semantics here so a future implementer who silently
  // changes the formula must update this test too.
  for (const int K : {2, 4, 6}) {
    for (int k = 1; k <= K; ++k) {
      const auto u = line_losses::loss_segment_geometry(
          80.0, K, k, LinePwlLayout::uniform);
      const auto e = line_losses::loss_segment_geometry(
          80.0, K, k, LinePwlLayout::equal_error);
      CAPTURE(K);
      CAPTURE(k);
      CHECK(e.width == doctest::Approx(u.width).epsilon(kEps));
      CHECK(e.slope == doctest::Approx(u.slope).epsilon(kEps));
    }
  }
}

TEST_CASE("loss_segment_geometry: scales linearly with envelope")
{
  // Doubling the envelope doubles widths and doubles slopes (linear
  // in B), so all per-MW loss coefficients (slope × R/V²) double too.
  // (Historically this was the EL=0 "lifted" envelope path; that lift
  // was removed and ``enforce_level`` is now a no-op, but the linear-
  // in-B scaling property of the geometry itself still holds and is
  // what this test pins.)
  for (const int K : {3, 5}) {
    for (int k = 1; k <= K; ++k) {
      const auto a = line_losses::loss_segment_geometry(
          50.0, K, k, LinePwlLayout::uniform);
      const auto b = line_losses::loss_segment_geometry(
          100.0, K, k, LinePwlLayout::uniform);
      CAPTURE(K);
      CAPTURE(k);
      CHECK(b.width == doctest::Approx(2.0 * a.width).epsilon(kEps));
      CHECK(b.slope == doctest::Approx(2.0 * a.slope).epsilon(kEps));
    }
  }
}

TEST_CASE(  // NOLINT
    "make_config — per-line loss_row_scale lifts max seg coef to ~1.0")
{
  // Per-line ``loss_row_scale`` override in
  // ``line_losses::make_config`` picks
  //   s_line = K · V² / (fmax · R · (2K-1))
  // so the largest segment-K coefficient
  //   loss_K · s_line = (fmax/K)·(2K-1)·R/V² · s_line = 1.0
  // lands at exactly 1.0 in the LP matrix.  Pins the κ-improving
  // recipe that took CEN PCP weekly from κ ≈ 1.2e9 (legacy global
  // scaling) down to ≈ 1.6e8 (per-line auto-scale, all loss rows
  // normalised).  Caller-supplied positive ``loss_row_scale`` is
  // overridden in PWL modes when the per-line recipe applies.
  Line line;
  constexpr double R = 0.01;
  constexpr double V = 100.0;
  constexpr int K = 6;
  constexpr double FMAX = 200.0;

  auto cfg = line_losses::make_config(LineLossesMode::piecewise,
                                      line,
                                      LossAllocationMode::receiver,
                                      /*lossfactor=*/0,
                                      R,
                                      V,
                                      K,
                                      FMAX,
                                      /*loss_row_scale=*/1e4);

  CHECK(cfg.mode == LineLossesMode::piecewise);
  CHECK(cfg.nseg == K);
  const double max_slope = (FMAX / K) * ((2 * K) - 1) * R / (V * V);
  const double expected = 1.0 / max_slope;
  CHECK(cfg.loss_row_scale == doctest::Approx(expected).epsilon(kEps));
  // Sanity: with these inputs the max post-scale coef is exactly 1.0.
  CHECK((cfg.loss_row_scale * max_slope) == doctest::Approx(1.0).epsilon(kEps));
}

TEST_CASE(  // NOLINT
    "make_config — per-line loss_row_scale is EL-symmetric")
{
  // Setting ``Line.enforce_level`` to 0, 1, or 2 must NOT change
  // ``loss_row_scale`` — the loss-PWL geometry is built on the same
  // envelope (``fmax``) regardless of EL, so flipping a line from
  // EL=1 → EL=0 (or back) must NOT alter the loss approximation.
  // Earlier code lifted the envelope to ``2 × fmax`` when EL=0,
  // breaking this invariant; the fix removed the lift entirely
  // (lift_multiplier hard-coded out of all three PWL paths in
  // ``line_losses.cpp::add_piecewise / add_direction /
  // add_piecewise_direct``).
  Line line_el0 {
      .enforce_level = 0,
  };
  Line line_el1 {
      .enforce_level = 1,
  };
  Line line_el2 {
      .enforce_level = 2,
  };

  constexpr auto args = [](Line& ln)
  {
    return [&ln]
    {
      return line_losses::make_config(LineLossesMode::piecewise,
                                      ln,
                                      LossAllocationMode::receiver,
                                      /*lossfactor=*/0,
                                      /*resistance=*/0.01,
                                      /*voltage=*/100,
                                      /*loss_segments=*/4,
                                      /*fmax=*/200);
    };
  };

  const auto cfg0 = args(line_el0)();
  const auto cfg1 = args(line_el1)();
  const auto cfg2 = args(line_el2)();

  CHECK(cfg0.loss_row_scale
        == doctest::Approx(cfg1.loss_row_scale).epsilon(kEps));
  CHECK(cfg1.loss_row_scale
        == doctest::Approx(cfg2.loss_row_scale).epsilon(kEps));
  // And the recipe holds:
  // max_slope = (200/4) · 7 · 0.01 / 10000 = 3.5e-4 → s_line ≈ 2857
  const double expected = 4 * 10000.0 / (200.0 * 0.01 * 7);
  CHECK(cfg1.loss_row_scale == doctest::Approx(expected).epsilon(kEps));
}

TEST_CASE(  // NOLINT
    "make_config — per-line scale needs all four inputs (R,V,fmax,K≥2)")
{
  // The per-line auto-scale branch in ``make_config`` only fires
  // when ``resistance > 0 && V² > 0 && fmax > 0 && nseg ≥ 2``.
  // Otherwise it falls back to the caller-supplied
  // ``loss_row_scale`` (legacy global scaling path).  Pins the
  // fallback so the linear and degenerate-PWL modes don't
  // accidentally pick up the auto-scale.
  Line line;
  // Linear mode → no per-line scaling (loss_row_scale stays at 7.0)
  auto cfg_linear = line_losses::make_config(LineLossesMode::linear,
                                             line,
                                             LossAllocationMode::receiver,
                                             /*lossfactor=*/0.05,
                                             /*resistance=*/0,
                                             /*voltage=*/0,
                                             /*loss_segments=*/1,
                                             /*fmax=*/200,
                                             /*loss_row_scale=*/7.0);
  CHECK(cfg_linear.loss_row_scale == doctest::Approx(7.0).epsilon(kEps));

  // PWL but fmax=0 → fall back to caller's scale
  auto cfg_nofmax = line_losses::make_config(LineLossesMode::piecewise,
                                             line,
                                             LossAllocationMode::receiver,
                                             /*lossfactor=*/0,
                                             /*resistance=*/0.01,
                                             /*voltage=*/100,
                                             /*loss_segments=*/4,
                                             /*fmax=*/0,
                                             /*loss_row_scale=*/7.0);
  CHECK(cfg_nofmax.loss_row_scale == doctest::Approx(7.0).epsilon(kEps));
}

// NOLINTEND(bugprone-unchecked-optional-access)
