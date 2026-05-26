// SPDX-License-Identifier: BSD-3-Clause
//
// Convergence of the two PWL line-loss constructions.
//
// The loss curve is the convex quadratic ℓ(f) = (R/V²)·f².  gtopt offers
// two static PWL approximations of it:
//
//   * uniform / equal_error (``loss_segment_geometry``): equal-width
//     SECANT chords — OVER-estimates ℓ (chords lie above a convex curve),
//     exact at the segment breakpoints.
//   * tangent (``loss_tangent_geometry``): outer-approximation TANGENT
//     lines — UNDER-estimates ℓ (tangents lie below a convex curve),
//     exact at the touch points.
//
// They are two approximations of the SAME curve, so they must (a) sandwich
// the true f² and (b) converge to each other as the segment count K grows.
// The worst chord/tangent error of each is (envelope/2K)², i.e. O(1/K²),
// so the gap between them must shrink quadratically: doubling K cuts it ~4×.
//
// This test pins that behaviour purely on the geometric (pre-R/V²) values,
// independent of any line's resistance or voltage.

#include <algorithm>

#include <doctest/doctest.h>
#include <gtopt/line_losses.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
using namespace gtopt::line_losses;  // NOLINT(google-global-names-in-headers)

namespace
{

// Geometric (pre-R/V²) loss the UNIFORM secant PWL assigns to a flow ``f``
// on ``[0, envelope]``: the convex LP fills the lowest-slope segments first
// (slopes increase with k), so we greedily fill segments 1..K.
[[nodiscard]] double uniform_loss(double f, double envelope, int nseg)
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

// Geometric (pre-R/V²) loss the TANGENT outer-approximation assigns to a
// flow ``f``: loss = max_k (2·t_k·f − t_k²) (the upper envelope of the
// tangent lower-bounds), clamped at 0.
[[nodiscard]] double tangent_loss(double f, double envelope, int nseg)
{
  double best = 0.0;
  for (int k = 1; k <= nseg; ++k) {
    const auto g =
        loss_tangent_geometry(envelope, nseg, k, LinePwlLayout::tangent);
    best = std::max(best, (g.slope_coef * f) + g.intercept_coef);
  }
  return best;
}

constexpr double kEnvelope = 1.0;
constexpr int kSamples = 2000;

// Largest (uniform − tangent) gap sampled across the envelope.
[[nodiscard]] double max_gap(int nseg)
{
  double gap = 0.0;
  for (int i = 0; i <= kSamples; ++i) {
    const double f = kEnvelope * static_cast<double>(i) / kSamples;
    gap = std::max(
        gap,
        uniform_loss(f, kEnvelope, nseg) - tangent_loss(f, kEnvelope, nseg));
  }
  return gap;
}

}  // namespace

TEST_CASE(
    "line-loss PWL: uniform and tangent sandwich the true curve")  // NOLINT
{
  // Uniform over-estimates and tangent under-estimates f² everywhere.
  for (const int nseg : {4, 8, 16}) {
    for (int i = 0; i <= kSamples; ++i) {
      const double f = kEnvelope * static_cast<double>(i) / kSamples;
      const double truth = f * f;
      const double uni = uniform_loss(f, kEnvelope, nseg);
      const double tan = tangent_loss(f, kEnvelope, nseg);
      CHECK(uni >= truth - 1e-12);  // secant is an UPPER bound
      CHECK(tan <= truth + 1e-12);  // tangent is a LOWER bound
      CHECK(uni >= tan - 1e-12);  // and uniform never below tangent
    }
  }
}

TEST_CASE(
    "line-loss PWL: both constructions are exact at their nodes")  // NOLINT
{
  constexpr int nseg = 8;
  const double w = kEnvelope / nseg;
  // Uniform is exact at segment breakpoints f = k·w.
  for (int k = 0; k <= nseg; ++k) {
    const double f = k * w;
    CHECK(uniform_loss(f, kEnvelope, nseg)
          == doctest::Approx(f * f).epsilon(1e-9));
  }
  // Tangent is exact at its touch points t_k = envelope·(2k−1)/(2K).
  for (int k = 1; k <= nseg; ++k) {
    const auto g =
        loss_tangent_geometry(kEnvelope, nseg, k, LinePwlLayout::tangent);
    const double f = g.touch_point;
    CHECK(tangent_loss(f, kEnvelope, nseg)
          == doctest::Approx(f * f).epsilon(1e-9));
  }
}

TEST_CASE("line-loss PWL: uniform and tangent converge as O(1/K^2)")  // NOLINT
{
  // The gap must shrink monotonically with more segments...
  const double g4 = max_gap(4);
  const double g10 = max_gap(10);
  const double g20 = max_gap(20);
  const double g40 = max_gap(40);
  CHECK(g10 < g4);
  CHECK(g20 < g10);
  CHECK(g40 < g20);

  SUBCASE("k=10 close, k=20 much closer (quadratic convergence)")
  {
    // Doubling K cuts the gap ~4× (gap ∝ 1/K²).  Allow a band for the
    // finite-sample max; the exact ratio is 4.
    CHECK((g10 / g20) == doctest::Approx(4.0).epsilon(0.05));
    CHECK((g20 / g40) == doctest::Approx(4.0).epsilon(0.05));
    // gap·K² is the same constant (= envelope²/4) for every K.
    CHECK((g10 * 100.0) == doctest::Approx(g20 * 400.0).epsilon(0.02));
    CHECK((g20 * 400.0) == doctest::Approx(g40 * 1600.0).epsilon(0.02));
  }

  SUBCASE("absolute gap is small and as predicted (envelope^2 / 4K^2)")
  {
    // Worst secant + worst tangent error each = (envelope/2K)², so the
    // sampled gap tracks envelope²/(4K²).
    for (const int nseg : {4, 10, 20, 40}) {
      const double predicted = (kEnvelope * kEnvelope) / (4.0 * nseg * nseg);
      CHECK(max_gap(nseg) == doctest::Approx(predicted).epsilon(0.02));
    }
  }
}
