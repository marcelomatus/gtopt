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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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
  // This is the EL=0 "lifted" envelope behaviour: when a line is
  // demoted to enforce_level = 0 the segment geometry is rebuilt on
  // [0, 2·tmax] instead of [0, tmax].
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

// NOLINTEND(bugprone-unchecked-optional-access)
