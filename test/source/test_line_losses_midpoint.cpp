// SPDX-License-Identifier: BSD-3-Clause
//
// De-biased ("midpoint") PWL line-loss layout.
//
// The convex loss curve is ℓ(f) = (R/V²)·f².  The legacy ``uniform``
// layout uses equal-width SECANT chords, which lie ABOVE the curve and
// systematically OVERSTATE loss (error peaks at the segment midpoints by
// (w/2)²·R/V², zero at the breakpoints — a strictly positive, all-one-
// sign bias).
//
// The ``midpoint`` layout keeps the SAME chord slopes ``w·(2k−1)`` but
// shifts the whole PWL DOWN by the FLAT constant ``(w/2)²·R/V²`` so each
// chord becomes the TANGENT to ℓ at its segment midpoint
// ``m_k = (2k−1)·w/2``.  Adjacent midpoint tangents intersect EXACTLY at
// the breakpoints ``b_k = k·w``, so the offset does NOT accumulate — the
// reconstruction is a single continuous PWL with
//
//     midpoint(f) = secant(f) − (w/2)²·R/V²            (for all f > 0)
//
// clamped at 0 near f = 0.  This makes it an UNBIASED estimator (exact at
// the midpoints, ≤ that constant UNDER at the breakpoints) vs the secant's
// all-positive overstatement.  The geometric (pre-R/V²) values are
// verified here, independent of any line's resistance / voltage.

#include <algorithm>
#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/line_losses.hpp>

using namespace gtopt;
using namespace gtopt::line_losses;

namespace test_line_losses_midpoint  // NOLINT
{

// Geometric secant PWL loss for ``f`` (convex fill, lowest slope first).
[[nodiscard]] double secant_loss(double f, double envelope, int nseg)
{
  double loss = 0.0;
  double remaining = f;
  for (int k = 1; k <= nseg && remaining > 1e-15; ++k) {
    const auto g =
        loss_segment_geometry(envelope, nseg, k, LinePwlLayout::uniform);
    const double fill = std::min(g.width, remaining);
    loss += g.slope * fill;
    remaining -= fill;
  }
  return loss;
}

// Geometric de-biased ("midpoint") PWL loss: the secant minus the flat
// (w/2)² offset, clamped at 0 (mirrors the LP ``loss ≥ secant − offset``
// with ``loss ≥ 0``).
[[nodiscard]] double midpoint_loss(double f, double envelope, int nseg)
{
  const double w = envelope / nseg;
  const double offset = (w / 2.0) * (w / 2.0);
  if (f <= 1e-15) {
    return 0.0;
  }
  return std::max(0.0, secant_loss(f, envelope, nseg) - offset);
}

constexpr double kEnvelope = 1.0;
constexpr int kSamples = 4000;

TEST_CASE("line-loss midpoint: exact at the segment midpoints")  // NOLINT
{
  for (const int nseg : {4, 6, 8, 16}) {
    const double w = kEnvelope / nseg;
    for (int k = 1; k <= nseg; ++k) {
      const double m_k = (2 * k - 1) * w / 2.0;  // segment midpoint
      const double truth = m_k * m_k;
      CHECK(midpoint_loss(m_k, kEnvelope, nseg)
            == doctest::Approx(truth).epsilon(1e-9));
    }
  }
}

TEST_CASE(
    "line-loss midpoint: de-bias is a flat constant (no accumulation)")  // NOLINT
{
  // secant(f) − midpoint(f) must equal the SAME (w/2)² for every f > 0,
  // regardless of how many segments the flow spans.  This is the key
  // property the implementation relies on: the offset is a single RHS
  // constant on the loss row, not a per-segment accumulation.
  for (const int nseg : {4, 6, 8}) {
    const double w = kEnvelope / nseg;
    const double offset = (w / 2.0) * (w / 2.0);
    for (int i = 1; i <= kSamples; ++i) {
      const double f = kEnvelope * static_cast<double>(i) / kSamples;
      const double sec = secant_loss(f, kEnvelope, nseg);
      const double mid = midpoint_loss(f, kEnvelope, nseg);
      // Only assert flatness where the clamp at 0 is inactive (the curve
      // near f=0 is below the offset).
      if (sec >= offset) {
        CHECK((sec - mid) == doctest::Approx(offset).epsilon(1e-9));
      }
    }
  }
}

TEST_CASE("line-loss midpoint: unbiased vs the secant overstatement")  // NOLINT
{
  // Secant error is ALL POSITIVE (over-estimate); midpoint error is
  // symmetric / near-zero mean.  Compare mean signed error and mean
  // absolute error across the envelope.
  for (const int nseg : {4, 6, 8}) {
    double sum_sec = 0.0;
    double sum_mid = 0.0;
    double abs_sec = 0.0;
    double abs_mid = 0.0;
    double min_sec_err = 0.0;
    int n = 0;
    for (int i = 0; i <= kSamples; ++i) {
      const double f = kEnvelope * static_cast<double>(i) / kSamples;
      const double truth = f * f;
      const double e_sec = secant_loss(f, kEnvelope, nseg) - truth;
      const double e_mid = midpoint_loss(f, kEnvelope, nseg) - truth;
      sum_sec += e_sec;
      sum_mid += e_mid;
      abs_sec += std::abs(e_sec);
      abs_mid += std::abs(e_mid);
      min_sec_err = std::min(min_sec_err, e_sec);
      ++n;
    }
    const double mean_sec = sum_sec / n;
    const double mean_mid = sum_mid / n;

    // Secant never undershoots the true curve (upper bound).
    CHECK(min_sec_err >= -1e-9);
    // Secant has a strictly POSITIVE mean error (systematic overstatement).
    CHECK(mean_sec > 0.0);
    // Midpoint mean error is much closer to zero (unbiased).
    CHECK(std::abs(mean_mid) < std::abs(mean_sec));
    // And its mean ABSOLUTE error is smaller too — strictly better
    // accuracy at identical K and LP structure.
    CHECK((abs_mid / n) < (abs_sec / n));
  }
}

TEST_CASE("line-loss midpoint: shares the secant's chord slopes")  // NOLINT
{
  // The LP de-bias changes ONLY the loss-row RHS constant, not the
  // per-segment slopes — confirm the geometry helper still returns the
  // uniform chord slopes (the implementation reuses ``seg_geom`` and only
  // shifts the row's RHS).
  for (const int nseg : {4, 8}) {
    for (int k = 1; k <= nseg; ++k) {
      const auto gu =
          loss_segment_geometry(kEnvelope, nseg, k, LinePwlLayout::uniform);
      const auto gm =
          loss_segment_geometry(kEnvelope, nseg, k, LinePwlLayout::midpoint);
      CHECK(gm.width == doctest::Approx(gu.width));
      CHECK(gm.slope == doctest::Approx(gu.slope));
    }
  }
}

}  // namespace test_line_losses_midpoint
