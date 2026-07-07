// SPDX-License-Identifier: BSD-3-Clause
/// @file test_line_losses_tangent.cpp
/// @brief Pin the math of the tangent (outer-approximation) PWL
///        line-loss formulation (``loss_tangent_geometry`` +
///        ``add_tangents`` LP behavior).
///
/// The tangent mode approximates the convex quadratic loss curve
/// ``ℓ(f) = (R/V²)·f²`` by K **tangent lines** that touch the curve
/// at K uniformly-spaced points ``t_k = envelope · (2k − 1) / (2K)``.
/// Each tangent is the LP inequality
///
///     loss ≥ R/V² · (2·t_k · |f| − t_k²)
///
/// The LP minimises ``loss``, so it picks ``loss = max_k tangent_k(|f|)``
/// — which equals ``ℓ(|f|)`` exactly at one of the touch points and
/// lies BELOW the curve elsewhere (outer approximation; LP
/// underestimates loss).  Companion to the secant chord formulation
/// in ``test_line_losses_seg_geom.cpp`` (uniform mode); both
/// approximations use the same envelope ``[0, envelope]`` and the
/// same K but on opposite sides of the curve.
///
/// Tests pin:
///   1. Touch-point spacing matches ``envelope · (2k − 1) / (2K)``.
///   2. Tangent line touches the curve at ``t_k`` (LP equality holds).
///   3. Outer approximation: every tangent stays BELOW the curve
///      across ``[0, envelope]``.
///   4. Max underestimate at partition boundaries
///      ``b_k = envelope · k / K`` matches the theoretical
///      ``(envelope / (2K))²`` (independent of k).
///   5. Envelope of K tangents (``max_k tangent_k(f)``) is a valid
///      lower-bound piecewise-affine envelope of ``ℓ(f)``.
///   6. The geometry returns all zeros (no LP rows) when called with
///      a non-tangent layout or invalid ``k``.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>
#include <gtopt/line_enums.hpp>
#include <gtopt/line_losses.hpp>

using namespace gtopt;

namespace
{

// kEps_tangent avoids the Unity-build collision with the same name
// in the sibling ``test_line_losses_seg_geom.cpp`` (anonymous
// namespaces merge under unity builds).  Per CLAUDE.md memory note
// `unity-anon-namespace`.
constexpr double kEps_tangent = 1e-12;

/// Apply tangent_k(f) = R/V² · (2·t_k·f − t_k²) using the public
/// geometry helper.  ``k_loss = R/V²``.
[[nodiscard]] double tangent_at(
    double f, double k_loss, double envelope, int K, int k)
{
  const auto g = line_losses::loss_tangent_geometry(
      envelope, K, k, LinePwlLayout::tangent);
  return k_loss * ((g.slope_coef * f) + g.intercept_coef);
}

/// True loss curve.
[[nodiscard]] double true_loss(double f, double k_loss)
{
  return k_loss * f * f;
}

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE("loss_tangent_geometry: touch-point spacing = envelope·(2k−1)/(2K)")
{
  // Pin the formula on a known envelope.  For K=4, envelope=80:
  //   t_1 = 80 · 1/8 = 10
  //   t_2 = 80 · 3/8 = 30
  //   t_3 = 80 · 5/8 = 50
  //   t_4 = 80 · 7/8 = 70
  // (midpoints of equal-width partitions [0,20], [20,40], …, [60,80]).
  constexpr double B = 80.0;
  constexpr std::array<double, 4> expected = {10.0, 30.0, 50.0, 70.0};
  for (size_t i = 0; i < expected.size(); ++i) {
    const int k = static_cast<int>(i) + 1;
    const auto g =
        line_losses::loss_tangent_geometry(B, 4, k, LinePwlLayout::tangent);
    CAPTURE(k);
    const double exp_t = expected.at(i);
    CHECK(g.touch_point == doctest::Approx(exp_t).epsilon(kEps_tangent));
    // Slope_coef = 2·t_k; intercept_coef = -t_k².
    CHECK(g.slope_coef == doctest::Approx(2.0 * exp_t).epsilon(kEps_tangent));
    CHECK(g.intercept_coef
          == doctest::Approx(-exp_t * exp_t).epsilon(kEps_tangent));
  }
}

TEST_CASE("loss_tangent_geometry: tangent touches the curve at t_k")
{
  // The tangent line at point t_k must equal the true curve value at
  // f = t_k (otherwise it's not a tangent).  This is the FUNDAMENTAL
  // mathematical property of the outer-approximation method.
  constexpr double B = 100.0;
  constexpr double k_loss = 0.001;  // arbitrary R/V²
  for (const int K : {1, 2, 4, 6, 10}) {
    for (int k = 1; k <= K; ++k) {
      const auto g =
          line_losses::loss_tangent_geometry(B, K, k, LinePwlLayout::tangent);
      const double tan_at_touch = tangent_at(g.touch_point, k_loss, B, K, k);
      const double curve_at_touch = true_loss(g.touch_point, k_loss);
      CAPTURE(K);
      CAPTURE(k);
      CAPTURE(g.touch_point);
      CHECK(tan_at_touch == doctest::Approx(curve_at_touch).epsilon(1e-10));
    }
  }
}

TEST_CASE("loss_tangent_geometry: tangent is BELOW the curve everywhere")
{
  // Outer approximation: ℓ(f) − tangent_k(f) ≥ 0 for all f ∈ [0, B].
  // (Equality at f = t_k; strictly positive elsewhere on a convex
  // quadratic.)  Sample 21 points across [0, B] for each (K, k) and
  // assert the curve never dips below the tangent.
  constexpr double B = 200.0;
  constexpr double k_loss = 0.005;
  constexpr int N_SAMPLES = 21;
  for (const int K : {2, 4, 8}) {
    for (int k = 1; k <= K; ++k) {
      for (int s = 0; s < N_SAMPLES; ++s) {
        const double f =
            B * static_cast<double>(s) / static_cast<double>(N_SAMPLES - 1);
        const double gap =
            true_loss(f, k_loss) - tangent_at(f, k_loss, B, K, k);
        CAPTURE(K);
        CAPTURE(k);
        CAPTURE(f);
        // Allow a tiny epsilon for f very close to t_k where the gap
        // is genuinely 0 modulo floating-point noise.
        CHECK(gap >= -1e-9);
      }
    }
  }
}

TEST_CASE(  // NOLINT
    "loss_tangent_geometry: max underestimate at partition boundary = (B/2K)²")
{
  // At the boundary b_k = B · k / K between partitions k and k+1,
  // both tangent_k and tangent_{k+1} are equidistant from t_k and
  // t_{k+1} (distance = B/(2K)).  Either tangent's value at b_k is
  //
  //     tan_k(b_k) = R/V² · (2·t_k·b_k − t_k²)
  //                = R/V² · (2·t_k·b_k − t_k²)
  //
  // and the curve is ℓ(b_k) = R/V² · b_k².  The gap is
  //
  //     gap = R/V² · (b_k − t_k)² = R/V² · (B/(2K))²
  //
  // i.e. INDEPENDENT of k — every partition boundary has the same
  // underestimate magnitude.  This is the "minimax" property of
  // uniform tangent placement on a convex quadratic.
  constexpr double B = 120.0;
  constexpr double k_loss = 0.01;
  for (const int K : {2, 3, 4, 6, 10}) {
    const double expected_gap = k_loss * std::pow(B / (2.0 * K), 2.0);
    for (int k = 1; k < K; ++k) {
      const double b_k = B * static_cast<double>(k) / static_cast<double>(K);
      const double gap_left =
          true_loss(b_k, k_loss) - tangent_at(b_k, k_loss, B, K, k);
      const double gap_right =
          true_loss(b_k, k_loss) - tangent_at(b_k, k_loss, B, K, k + 1);
      CAPTURE(K);
      CAPTURE(k);
      CAPTURE(b_k);
      // Both tangents have the same gap at the shared boundary.
      CHECK(gap_left == doctest::Approx(expected_gap).epsilon(1e-10));
      CHECK(gap_right == doctest::Approx(expected_gap).epsilon(1e-10));
    }
  }
}

TEST_CASE(
    "loss_tangent_geometry: max-of-K envelope is a piecewise affine lower "
    "bound")
{
  // The LP picks loss = max_k tangent_k(f) at each f.  This envelope
  // is the best K-piece piecewise-affine UNDER-approximator of the
  // curve.  Verify on a dense grid that
  //   (a) max_k tangent_k(f) ≤ ℓ(f) for all f ∈ [0, B]
  //   (b) the envelope ERROR ℓ(f) − max_k(…) ≤ (B/(2K))²·R/V² for
  //       all f (the theoretical worst-case gap at any partition
  //       boundary).
  constexpr double B = 50.0;
  constexpr double k_loss = 0.02;
  constexpr int N_SAMPLES = 101;
  for (const int K : {2, 4, 8}) {
    const double worst_case = k_loss * std::pow(B / (2.0 * K), 2.0);
    for (int s = 0; s < N_SAMPLES; ++s) {
      const double f =
          B * static_cast<double>(s) / static_cast<double>(N_SAMPLES - 1);
      double env = -std::numeric_limits<double>::infinity();
      for (int k = 1; k <= K; ++k) {
        env = std::max(env, tangent_at(f, k_loss, B, K, k));
      }
      const double curve = true_loss(f, k_loss);
      CAPTURE(K);
      CAPTURE(f);
      // (a) envelope below curve.
      CHECK(env <= curve + 1e-10);
      // (b) error within theoretical worst case (+ small epsilon).
      CHECK((curve - env) <= worst_case + 1e-10);
    }
  }
}

TEST_CASE("loss_tangent_geometry: returns zeros for invalid layout / k")
{
  // Non-tangent layout → zeros (the geometry is meaningless and we
  // don't want callers to silently get bogus tangent rows).
  for (const auto layout : {LinePwlLayout::uniform, LinePwlLayout::equal_error})
  {
    const auto g = line_losses::loss_tangent_geometry(100.0, 4, 1, layout);
    CHECK(g.touch_point == doctest::Approx(0.0).epsilon(kEps_tangent));
    CHECK(g.slope_coef == doctest::Approx(0.0).epsilon(kEps_tangent));
    CHECK(g.intercept_coef == doctest::Approx(0.0).epsilon(kEps_tangent));
  }
  // k out of range → zeros.
  const auto g_under =
      line_losses::loss_tangent_geometry(100.0, 4, 0, LinePwlLayout::tangent);
  const auto g_over =
      line_losses::loss_tangent_geometry(100.0, 4, 5, LinePwlLayout::tangent);
  CHECK(g_under.touch_point == doctest::Approx(0.0).epsilon(kEps_tangent));
  CHECK(g_over.touch_point == doctest::Approx(0.0).epsilon(kEps_tangent));
}

TEST_CASE(
    "loss_tangent_geometry: scales linearly in envelope, quadratically in "
    "intercept")
{
  // Doubling envelope doubles touch_point and slope_coef, but
  // QUADRUPLES the magnitude of intercept_coef (since intercept =
  // -t_k² and t_k scales linearly).  This is the "EL=0 lifted"
  // envelope behaviour.
  for (const int K : {2, 4, 8}) {
    for (int k = 1; k <= K; ++k) {
      const auto a = line_losses::loss_tangent_geometry(
          40.0, K, k, LinePwlLayout::tangent);
      const auto b = line_losses::loss_tangent_geometry(
          80.0, K, k, LinePwlLayout::tangent);
      CAPTURE(K);
      CAPTURE(k);
      CHECK(b.touch_point
            == doctest::Approx(2.0 * a.touch_point).epsilon(kEps_tangent));
      CHECK(b.slope_coef
            == doctest::Approx(2.0 * a.slope_coef).epsilon(kEps_tangent));
      CHECK(b.intercept_coef
            == doctest::Approx(4.0 * a.intercept_coef).epsilon(kEps_tangent));
    }
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)
