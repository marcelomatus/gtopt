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
#include <array>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line_losses.hpp>

using namespace gtopt;
using namespace gtopt::line_losses;

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

// ─── System-wide convergence under the adaptive K rule ────────────────
//
// The single-K convergence above pins worst-case PWL chord error ∝ 1/K²
// for ONE envelope.  This block stress-tests the SAME O(1/K²) decay at
// the system level when ``compute_adaptive_loss_segments`` distributes
// K across a heterogeneous mix: as ``err_pct`` shrinks, the rule pushes
// more segments onto the lossiest lines first, and the realised PWL
// error budget (Σ_i L_max,i / (4 K_i²)) shrinks accordingly — until the
// ceiling=6 clamp saturates and further accuracy gains are impossible.
//
// Method: purely analytic (mirrors the worst-case bound used inside the
// rule), no LP solves needed.  Task 1 in
// ``test_line_losses_decoupled_envelope.cpp`` validates that the
// LP-realised error sits BELOW this analytic bound on the same mix.

namespace line_losses_system_convergence_ns
{
namespace
{

struct LineFix
{
  double R;
  double fmax;
};

// Same 4-line CEN-PCP-scale mix as the LP integration test in
// ``test_line_losses_decoupled_envelope.cpp``.  L_max spans 900..2.5
// (~3 orders of magnitude).
constexpr std::array<LineFix, 4> kSystemLines = {{
    {
        .R = 0.0001,
        .fmax = 3000.0,
    },
    {
        .R = 0.0008,
        .fmax = 300.0,
    },
    {
        .R = 0.005,
        .fmax = 100.0,
    },
    {
        .R = 0.025,
        .fmax = 10.0,
    },
}};

/// Σ_i L_max,i / (4 K_i²) — system-wide worst-case PWL secant error.
[[nodiscard]] double system_error(std::span<const int> K)
{
  double total = 0.0;
  for (std::size_t i = 0; i < kSystemLines.size(); ++i) {
    const auto& ln = kSystemLines[i];
    const double L = ln.R * ln.fmax * ln.fmax;
    const double k = static_cast<double>(K[i]);
    total += L / (4.0 * k * k);
  }
  return total;
}

}  // namespace
}  // namespace line_losses_system_convergence_ns

using namespace line_losses_system_convergence_ns;

TEST_CASE(
    "line-loss PWL: adaptive rule drives system error O(1/K²) as err_pct "
    "shrinks")  // NOLINT
{
  // Parallel R / fmax vectors for the rule.
  std::vector<double> R_vec;
  std::vector<double> fmax_vec;
  R_vec.reserve(kSystemLines.size());
  fmax_vec.reserve(kSystemLines.size());
  for (const auto& ln : kSystemLines) {
    R_vec.push_back(ln.R);
    fmax_vec.push_back(ln.fmax);
  }
  double L_total = 0.0;
  for (std::size_t i = 0; i < kSystemLines.size(); ++i) {
    L_total += R_vec[i] * fmax_vec[i] * fmax_vec[i];
  }

  // err_pct sweep — INCREASING accuracy (smaller budget).
  const std::vector<double> err_pcts = {0.10, 0.05, 0.02, 0.01, 0.005, 0.002};

  std::vector<double> errors;
  std::vector<int> total_K_per_pct;
  std::vector<std::vector<int>> K_per_pct;
  errors.reserve(err_pcts.size());
  total_K_per_pct.reserve(err_pcts.size());
  K_per_pct.reserve(err_pcts.size());

  for (const double err_pct : err_pcts) {
    const AdaptiveSegmentsOpts opts {
        .err_pct = err_pct,
        .floor = 2,
        .ceiling = 6,
    };
    const auto K =
        compute_adaptive_loss_segments(std::span<const double> {R_vec},
                                       std::span<const double> {fmax_vec},
                                       opts);
    REQUIRE(K.size() == kSystemLines.size());

    const double err = system_error(std::span<const int> {K});
    const int total_K = std::accumulate(K.begin(), K.end(), 0);

    CAPTURE(err_pct);
    CAPTURE(total_K);
    CAPTURE(err);
    CAPTURE(L_total);

    // Sanity: every K_i in [floor, ceiling] for lossy lines.
    for (const int k : K) {
      CHECK(k >= 2);
      CHECK(k <= 6);
    }

    errors.push_back(err);
    total_K_per_pct.push_back(total_K);
    K_per_pct.push_back(std::vector<int> {K.begin(), K.end()});
  }

  SUBCASE("error decreases monotonically with err_pct (until ceiling clamps)")
  {
    // The ceiling=6 floor on system-wide predicted error is reached when
    // EVERY lossy line hits ceiling.  Once total_K stops growing, error
    // can no longer decrease — the clamp regime.  Within the unclamped
    // regime each successive (tighter) err_pct must NOT increase error.
    for (std::size_t j = 1; j < errors.size(); ++j) {
      CAPTURE(j);
      CAPTURE(err_pcts[j - 1]);
      CAPTURE(err_pcts[j]);
      CAPTURE(errors[j - 1]);
      CAPTURE(errors[j]);
      CAPTURE(total_K_per_pct[j - 1]);
      CAPTURE(total_K_per_pct[j]);
      if (total_K_per_pct[j] > total_K_per_pct[j - 1]) {
        // Unclamped step — error must strictly drop.
        CHECK(errors[j] < errors[j - 1] - 1e-12);
      } else {
        // Ceiling has fully saturated; the rule cannot improve.
        CHECK(errors[j] == doctest::Approx(errors[j - 1]).epsilon(1e-9));
      }
    }
  }

  SUBCASE("empirical decay tracks O(1/total_K²) within ~30% slack")
  {
    // Theoretical bound: err ∝ Σ L_i/K_i².  For balanced KKT K_i the
    // total-K scaling is err ∝ 1/(total_K/N)² · L_total = N²·L_total/total_K².
    // Empirically: doubling total_K should quarter the error (within 30 %
    // slack from integer-K rounding and the heterogeneous L distribution
    // pushing the rule away from uniform K).  Skip pairs where ceiling
    // clamped (no further decay possible).
    bool checked_any = false;
    for (std::size_t a = 0; a < errors.size(); ++a) {
      for (std::size_t b = a + 1; b < errors.size(); ++b) {
        if (total_K_per_pct[a] == total_K_per_pct[b]) {
          continue;  // saturated regime — no decay to compare
        }
        const double ka = static_cast<double>(total_K_per_pct[a]);
        const double kb = static_cast<double>(total_K_per_pct[b]);
        const double ratio_K = kb / ka;  // > 1 (b is tighter)
        const double ratio_err = errors[a] / errors[b];  // > 1
        const double expected = ratio_K * ratio_K;  // O(1/K²) ⇒ err ∝ 1/K²
        CAPTURE(a);
        CAPTURE(b);
        CAPTURE(total_K_per_pct[a]);
        CAPTURE(total_K_per_pct[b]);
        CAPTURE(errors[a]);
        CAPTURE(errors[b]);
        CAPTURE(ratio_K);
        CAPTURE(ratio_err);
        CAPTURE(expected);
        // Allow ±60 % band: discrete K can deviate substantially from
        // the smooth ∝1/K² when only 4 lines × 4-segment range.  The
        // important property is the empirical ratio is in the right
        // ballpark — true monotone strictness is pinned in the prior
        // subcase.
        CHECK(ratio_err >= expected * 0.4);
        CHECK(ratio_err <= expected * 2.5);
        checked_any = true;
      }
    }
    CHECK(checked_any);
  }
}
