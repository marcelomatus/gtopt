// SPDX-License-Identifier: BSD-3-Clause
/// @file
/// @brief Unit tests for the converter-agnostic adaptive-K rule
///        (``gtopt::line_losses::compute_adaptive_loss_segments``).
///
/// Companion to the Python ``scripts/plexos2gtopt/tests/test_parsers.py``
/// adaptive-K suite; pins the C++ implementation against the same
/// invariants so any future converter (plp2gtopt, hand-built JSON,
/// etc.) can rely on identical K allocations.
///
/// The rule is purely algebraic — no LP, no solver — so these are
/// fast unit tests.  LP-side correctness for any given per-line K is
/// pinned separately by ``test_line_losses_decoupled_envelope.cpp``.

#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/line_losses.hpp>

using namespace gtopt;

namespace line_losses_adaptive_test_ns
{
namespace
{

/// Convenience: wrap two ``std::vector<double>`` calls into one
/// ``compute_adaptive_loss_segments`` invocation.
[[nodiscard]] auto run(const std::vector<double>& R,
                       const std::vector<double>& fmax,
                       line_losses::AdaptiveSegmentsOpts opts = {})
{
  return line_losses::compute_adaptive_loss_segments(
      std::span<const double> {R}, std::span<const double> {fmax}, opts);
}

/// Σ_i L_max,i / (4 K_i²) — worst-case PWL secant error across all
/// lossy lines, in the rule's ``R·f²`` units.
[[nodiscard]] double realised_error(const std::vector<double>& R,
                                    const std::vector<double>& fmax,
                                    const std::vector<int>& K)
{
  double err = 0.0;
  for (std::size_t i = 0; i < R.size(); ++i) {
    if (R[i] > 0.0 && fmax[i] > 0.0 && K[i] > 0) {
      const double L = R[i] * fmax[i] * fmax[i];
      err += L / (4.0 * static_cast<double>(K[i]) * static_cast<double>(K[i]));
    }
  }
  return err;
}

/// Σ_i L_max,i across all lossy lines.
[[nodiscard]] double total_L(const std::vector<double>& R,
                             const std::vector<double>& fmax)
{
  double L = 0.0;
  for (std::size_t i = 0; i < R.size(); ++i) {
    if (R[i] > 0.0 && fmax[i] > 0.0) {
      L += R[i] * fmax[i] * fmax[i];
    }
  }
  return L;
}

}  // namespace
}  // namespace line_losses_adaptive_test_ns

using namespace line_losses_adaptive_test_ns;  // NOLINT

TEST_CASE("compute_adaptive_loss_segments — cube-root scaling")  // NOLINT
{
  // Three lines with L_max = R·f² in ratio 1 : 8 : 64 (= 2³ : 4³ : ...).
  //   Lₐ = 0.01·100² = 100     → L^(1/3) =  ~4.64
  //   Lᵦ = 0.08·100² = 800     → L^(1/3) =  ~9.28
  //   Lᵧ = 0.64·100² = 6400    → L^(1/3) = ~18.57
  // KKT rule says K_i ∝ L_i^(1/3); doubling L_i gives ~25.99% more K.
  const std::vector<double> R {0.01, 0.08, 0.64};
  const std::vector<double> fmax {100.0, 100.0, 100.0};

  // Loose budget so no ceiling clamp.
  const auto K = run(R, fmax, {.err_pct = 0.05, .floor = 1, .ceiling = 20});

  REQUIRE(K.size() == 3);
  CHECK(K[0] < K[1]);
  CHECK(K[1] < K[2]);
  // Ratio K_c / K_a should be close to (Lᵧ/Lₐ)^(1/3) = 4.0.
  // Integer ceiling rounding can inflate the ratio slightly; allow ±25%.
  const double ratio_ca = static_cast<double>(K[2]) / static_cast<double>(K[0]);
  CHECK(ratio_ca >= 3.0);
  CHECK(ratio_ca <= 5.5);
}

TEST_CASE(
    "compute_adaptive_loss_segments — KKT exact match (no clamps)")  // NOLINT
{
  // Pick L values and err_pct so the raw KKT solution lands inside
  // [floor, ceiling] for every line — then K must equal ⌈c·L^(1/3)⌉.
  const std::vector<double> R {0.01, 0.01, 0.01};
  const std::vector<double> fmax {
      500.0, 250.0, 125.0};  // L = 2500, 625, 156.25
  const line_losses::AdaptiveSegmentsOpts opts {
      .err_pct = 0.04, .floor = 2, .ceiling = 6};
  const auto K = run(R, fmax, opts);

  // Reconstruct the analytic prediction.
  const double L_sum = total_L(R, fmax);
  double S = 0.0;
  for (std::size_t i = 0; i < R.size(); ++i) {
    S += std::cbrt(R[i] * fmax[i] * fmax[i]);
  }
  const double B = opts.err_pct * L_sum;
  const double c = std::sqrt(S / (4.0 * B));

  for (std::size_t i = 0; i < R.size(); ++i) {
    const double cb = std::cbrt(R[i] * fmax[i] * fmax[i]);
    const auto expected = std::clamp(
        static_cast<int>(std::ceil(c * cb)), opts.floor, opts.ceiling);
    CHECK(K[i] == expected);
  }
  // Bigger L → bigger K within this fixture (all clamps clear).
  CHECK(K[0] >= K[1]);
  CHECK(K[1] >= K[2]);
}

TEST_CASE("compute_adaptive_loss_segments — floor clamps tiny lines")  // NOLINT
{
  // Pair a tiny line (L = 0.0025) with a dominator (L = 2500).  At
  // err_pct = 0.05 the cube-root rule wants K_tiny ≪ 2 → floor=2 wins.
  const std::vector<double> R {0.01, 0.0001};  // L=2500, 0.01
  const std::vector<double> fmax {500.0, 10.0};
  const auto K = run(R, fmax, {.err_pct = 0.05, .floor = 2, .ceiling = 8});

  // Tiny line clamps to floor.
  CHECK(K[1] == 2);
  // Dominator stays inside [floor, ceiling].
  CHECK(K[0] >= 2);
  CHECK(K[0] <= 8);
}

TEST_CASE(
    "compute_adaptive_loss_segments — ceiling clamps fat lines")  // NOLINT
{
  // Single very heavy line at a tight budget: raw K ≈ 1/(2·√err_pct) ≈
  // 11.2 at err_pct=0.002; ceiling=6 must clamp it.
  const std::vector<double> R {0.01};
  const std::vector<double> fmax {1000.0};
  const auto K = run(R, fmax, {.err_pct = 0.002, .floor = 2, .ceiling = 6});

  REQUIRE(K.size() == 1);
  CHECK(K[0] == 6);
}

TEST_CASE(
    "compute_adaptive_loss_segments — error budget honoured (no clamps)")  // NOLINT
{
  // 4-line mix sized so cube-root rule lands every K in [3, 5] at
  // err_pct=0.02; the realised total error must stay ≤ budget.
  const std::vector<double> R {0.01, 0.05, 0.10, 0.20};
  const std::vector<double> fmax {200.0, 100.0, 50.0, 20.0};
  const line_losses::AdaptiveSegmentsOpts opts {
      .err_pct = 0.02, .floor = 2, .ceiling = 10};
  const auto K = run(R, fmax, opts);

  const double L_sum = total_L(R, fmax);
  const double budget = opts.err_pct * L_sum;
  const double realised = realised_error(R, fmax, K);
  CHECK(realised <= budget);
}

TEST_CASE(
    "compute_adaptive_loss_segments — beats uniform at same Σ K")  // NOLINT
{
  // Heterogeneous mix (4 orders of magnitude in L).  Adaptive must
  // produce LOWER worst-case error than uniform K at the same total
  // segment cost.  This is the KKT-optimum claim, the rule's whole pitch.
  const std::vector<double> R {0.0001, 0.001, 0.01, 0.1};
  const std::vector<double> fmax {3000.0, 300.0, 100.0, 10.0};
  // Loose ceiling so the rule has room to allocate freely.
  const auto K_adaptive =
      run(R, fmax, {.err_pct = 0.01, .floor = 1, .ceiling = 20});

  const auto n_lossy = K_adaptive.size();
  int sumK = 0;
  for (auto k : K_adaptive) {
    sumK += k;
  }
  // Uniform K spread across the SAME segment count → ⌊Σ K / N⌋.
  const int K_uniform = std::max(1, sumK / static_cast<int>(n_lossy));
  const std::vector<int> K_unif(n_lossy, K_uniform);

  const double err_a = realised_error(R, fmax, K_adaptive);
  const double err_u = realised_error(R, fmax, K_unif);
  CHECK(err_a < err_u);
}

TEST_CASE("compute_adaptive_loss_segments — Σ K monotone in err_pct")  // NOLINT
{
  // Looser err_pct must never spend MORE segments than tighter — the
  // rule's main economic guarantee.
  const std::vector<double> R {0.0001, 0.001, 0.01, 0.1};
  const std::vector<double> fmax {3000.0, 300.0, 100.0, 10.0};
  int prev_sum = std::numeric_limits<int>::max();
  for (const double eps : {0.001, 0.005, 0.01, 0.02, 0.05, 0.10}) {
    const auto K = run(R, fmax, {.err_pct = eps});
    int s = 0;
    for (auto k : K) {
      s += k;
    }
    CHECK(s <= prev_sum);
    prev_sum = s;
  }
}

TEST_CASE("compute_adaptive_loss_segments — lossless lines get K=0")  // NOLINT
{
  // Mix of lossless and lossy.  Lossless (R==0 OR fmax==0 OR both)
  // must return K=0 so the PWL builder can skip them entirely.
  const std::vector<double> R {0.0, 0.01, 0.05, 0.0};
  const std::vector<double> fmax {100.0, 100.0, 0.0, 0.0};
  const auto K = run(R, fmax);

  REQUIRE(K.size() == 4);
  CHECK(K[0] == 0);  // R=0 → lossless
  CHECK(K[1] >= 2);  // only lossy line in the mix
  CHECK(K[1] <= 6);
  CHECK(K[2] == 0);  // fmax=0 → lossless
  CHECK(K[3] == 0);  // both zero → lossless
}

TEST_CASE("compute_adaptive_loss_segments — empty input")  // NOLINT
{
  const std::vector<double> empty;
  const auto K = run(empty, empty);
  CHECK(K.empty());
}

TEST_CASE("compute_adaptive_loss_segments — all lossless → all zero")  // NOLINT
{
  const std::vector<double> R(5, 0.0);
  const std::vector<double> fmax(5, 100.0);
  const auto K = run(R, fmax);

  REQUIRE(K.size() == 5);
  for (auto k : K) {
    CHECK(k == 0);
  }
}

TEST_CASE(
    "compute_adaptive_loss_segments — err_pct ≤ 0 falls back to ceiling")  // NOLINT
{
  // The Python wrapper treats ``GTOPT_LOSS_ERROR_PCT=0`` as "disable
  // adaptive mode"; the C++ helper mirrors that with err_pct ≤ 0 →
  // every lossy line gets ``ceiling``.
  const std::vector<double> R {0.001, 0.01, 0.0};
  const std::vector<double> fmax {1000.0, 100.0, 50.0};
  const auto K = run(R, fmax, {.err_pct = 0.0, .floor = 2, .ceiling = 4});

  CHECK(K[0] == 4);
  CHECK(K[1] == 4);
  CHECK(K[2] == 0);  // still lossless
}

TEST_CASE(
    "compute_adaptive_loss_segments — large heterogeneous system")  // NOLINT
{
  // 4-line mimic of the CEN-PCP trunk/backbone/regional/stub mix
  // (cf. test_parsers.py::_realistic_line_mix).  At err_pct = 1 % the
  // ceiling=6 binds on the trunk line; realised error then exceeds
  // budget by the (raw/ceiling)² ratio on that line — which is the
  // rule's documented behaviour and the headline finding of the
  // CEN-PCP LP-relax sweep.
  const std::vector<double> R {0.0001, 0.0008, 0.005, 0.025};
  const std::vector<double> fmax {3000.0, 300.0, 100.0, 10.0};
  const auto K = run(R, fmax, {.err_pct = 0.01, .floor = 2, .ceiling = 6});

  // Trunk hits the ceiling.
  CHECK(K[0] == 6);
  // Backbone gets moderate K.
  CHECK(K[1] >= 2);
  CHECK(K[1] <= 6);
  // Smallest line floors at 2.
  CHECK(K[3] == 2);
  // KKT ordering preserved within the clamped band.
  CHECK(K[0] >= K[1]);
  CHECK(K[1] >= K[2]);
  CHECK(K[2] >= K[3]);

  // Even though the ceiling clamp pushes realised error above the
  // 1 % target, the absolute realised error must stay below the
  // all-floor case  Σ L/(4·floor²)  (i.e. the rule never does WORSE
  // than dropping every line to the floor).
  const double L_sum = total_L(R, fmax);
  const double floor_worst = L_sum / (4.0 * 2.0 * 2.0);
  const double realised = realised_error(R, fmax, K);
  CHECK(realised < floor_worst);
}

// ───────────────────────────────────────────────────────────────────
// compute_dynamic_loss_layout — per-line (K, layout) under err_pct
// Companion to the adaptive K tests; mirrors the Python suite at
// scripts/plexos2gtopt/tests/test_parsers.py::test_dynamic_*.
// ───────────────────────────────────────────────────────────────────

namespace line_losses_adaptive_test_ns
{
namespace
{

/// Convenience wrapper: same shape as ``run`` but invokes the dynamic
/// rule and returns a vector of (K, layout) pairs.
[[nodiscard]] auto run_dynamic(const std::vector<double>& R,
                               const std::vector<double>& fmax,
                               line_losses::DynamicLayoutOpts opts = {})
{
  return line_losses::compute_dynamic_loss_layout(
      std::span<const double> {R}, std::span<const double> {fmax}, opts);
}

/// System-wide signed mean error
///   Σ_uniform L/(6K²) − Σ_midpoint L/(12K²)
/// — the invariant the dynamic rule's greedy minimizes.
[[nodiscard]] double signed_mean_error(
    const std::vector<double>& R,
    const std::vector<double>& fmax,
    const std::vector<line_losses::DynamicAssignment>& asgn)
{
  double total = 0.0;
  for (std::size_t i = 0; i < R.size(); ++i) {
    if (R[i] <= 0.0 || fmax[i] <= 0.0 || asgn[i].K == 0) {
      continue;
    }
    const double L = R[i] * fmax[i] * fmax[i];
    const double kk = static_cast<double>(asgn[i].K * asgn[i].K);
    if (asgn[i].layout == LinePwlLayout::midpoint) {
      total -= L / (12.0 * kk);
    } else {
      total += L / (6.0 * kk);
    }
  }
  return total;
}

}  // namespace
}  // namespace line_losses_adaptive_test_ns

TEST_CASE(
    "compute_dynamic_loss_layout — loose budget keeps all uniform")  // NOLINT
{
  // Tiny lines, low error contribution, easy budget — no promotions needed.
  const std::vector<double> R {0.01, 0.05, 0.10};
  const std::vector<double> fmax {200.0, 100.0, 50.0};
  const auto asgn = run_dynamic(R, fmax, {.err_pct = 0.10});  // very loose

  REQUIRE(asgn.size() == 3);
  for (const auto& a : asgn) {
    CHECK(a.layout == LinePwlLayout::uniform);
    CHECK(a.K >= 2);
  }
}

TEST_CASE(
    "compute_dynamic_loss_layout — tight budget promotes heaviest")  // NOLINT
{
  // 5-line spread that forces at least one promotion at err_pct = 0.1 %.
  // Heaviest line L0 has L_max = 9000, ~7× the next.
  const std::vector<double> R {0.001, 0.005, 0.01, 0.05, 0.10};
  const std::vector<double> fmax {3000.0, 500.0, 100.0, 50.0, 10.0};
  const auto asgn = run_dynamic(R, fmax, {.err_pct = 0.001});

  REQUIRE(asgn.size() == 5);
  int midpoint_count = 0;
  for (const auto& a : asgn) {
    if (a.layout == LinePwlLayout::midpoint) {
      ++midpoint_count;
    }
  }
  CHECK(midpoint_count >= 1);
  // Heaviest line MUST be among the promoted ones (greedy goes top-down).
  CHECK(asgn[0].layout == LinePwlLayout::midpoint);
}

TEST_CASE("compute_dynamic_loss_layout — reaches local optimum")  // NOLINT
{
  // After the rule decides, no remaining uniform→midpoint single
  // flip should improve abs(signed_mean).  This is the greedy's
  // local-optimality guarantee — the algorithm cannot always hit
  // abs(running) ≤ budget when a single heavy line dominates, but it
  // MUST have considered every improving flip.
  const std::vector<double> R {0.001, 0.005, 0.01, 0.05, 0.10};
  const std::vector<double> fmax {3000.0, 500.0, 100.0, 50.0, 10.0};
  const auto asgn = run_dynamic(R, fmax, {.err_pct = 0.001});
  const double final_err = signed_mean_error(R, fmax, asgn);

  for (std::size_t i = 0; i < R.size(); ++i) {
    if (asgn[i].layout != LinePwlLayout::uniform) {
      continue;
    }
    if (R[i] <= 0.0 || fmax[i] <= 0.0 || asgn[i].K == 0) {
      continue;
    }
    const double L = R[i] * fmax[i] * fmax[i];
    const double kk = static_cast<double>(asgn[i].K * asgn[i].K);
    const double delta = L / (4.0 * kk);
    const double after_flip = final_err - delta;
    // Flipping this uniform line should NOT strictly improve abs.
    CAPTURE(i);
    CAPTURE(final_err);
    CAPTURE(after_flip);
    CHECK(std::abs(after_flip) >= std::abs(final_err) - 1e-9);
  }
}

TEST_CASE(
    "compute_dynamic_loss_layout — meets budget when achievable")  // NOLINT
{
  // Happy path: 10 small equal lines, none budget-dominant.  Greedy
  // can land abs(error) ≤ budget cleanly.
  const std::vector<double> R(10, 0.01);
  const std::vector<double> fmax(10, 100.0);  // L_max = 100 each; Σ = 1000
  const double err_pct = 0.02;  // budget = 20
  const auto asgn = run_dynamic(R, fmax, {.err_pct = err_pct});
  const double L_total = 1000.0;
  const double budget = err_pct * L_total;
  const double realised = signed_mean_error(R, fmax, asgn);
  CHECK(std::abs(realised) <= budget * 1.05);  // 5 % discrete-greedy slack
}

TEST_CASE(
    "compute_dynamic_loss_layout — err_pct ≤ 0 fallback to uniform")  // NOLINT
{
  // Same contract as compute_adaptive_loss_segments: err_pct ≤ 0
  // disables adaptive ⇒ every lossy line gets ceiling K, uniform layout.
  const std::vector<double> R {0.001, 0.01, 0.0};
  const std::vector<double> fmax {1000.0, 100.0, 50.0};
  const auto asgn =
      run_dynamic(R, fmax, {.err_pct = 0.0, .floor = 2, .ceiling = 4});

  REQUIRE(asgn.size() == 3);
  CHECK(asgn[0].K == 4);
  CHECK(asgn[0].layout == LinePwlLayout::uniform);
  CHECK(asgn[1].K == 4);
  CHECK(asgn[1].layout == LinePwlLayout::uniform);
  CHECK(asgn[2].K == 0);  // lossless
  CHECK(asgn[2].layout == LinePwlLayout::uniform);
}

TEST_CASE(
    "compute_dynamic_loss_layout — K ≤ adaptive rule (looser two-sided budget)")  // NOLINT
{
  // Dynamic uses Phase 1' — the cube-root rule with a ``2 × budget``
  // effective budget.  The two-sided worst-case bound lets each layout
  // side carry up to ``budget`` independently, so dynamic's per-line
  // K is ``≤`` the adaptive rule's K (factor √2 lower on unclamped
  // middle-band lines, equal where floor/ceiling clamps dominate).
  // This replaces the previous "K identical" invariant which no longer
  // holds — adaptive is the K for static layouts, dynamic uses the
  // looser two-sided budget and produces a smaller K per line.
  const std::vector<double> R {0.0001, 0.0008, 0.005, 0.025};
  const std::vector<double> fmax {3000.0, 300.0, 100.0, 10.0};
  const line_losses::DynamicLayoutOpts opts {.err_pct = 0.01};
  const auto dyn = run_dynamic(R, fmax, opts);
  const auto adapt = line_losses::compute_adaptive_loss_segments(
      std::span<const double> {R},
      std::span<const double> {fmax},
      {.err_pct = opts.err_pct, .floor = opts.floor, .ceiling = opts.ceiling});
  REQUIRE(dyn.size() == adapt.size());
  for (std::size_t i = 0; i < dyn.size(); ++i) {
    CHECK(dyn[i].K <= adapt[i]);
    CHECK(dyn[i].K >= opts.floor);
  }
}

TEST_CASE("compute_dynamic_loss_layout — empty input")  // NOLINT
{
  const std::vector<double> empty;
  const auto asgn = run_dynamic(empty, empty);
  CHECK(asgn.empty());
}

TEST_CASE(
    "compute_dynamic_loss_layout — lossless lines stay uniform K=0")  // NOLINT
{
  const std::vector<double> R {0.0, 0.01, 0.05, 0.0};
  const std::vector<double> fmax {100.0, 100.0, 0.0, 0.0};
  const auto asgn = run_dynamic(R, fmax);

  REQUIRE(asgn.size() == 4);
  // Lossless: K=0, layout falls through to uniform (the default).
  CHECK(asgn[0].K == 0);
  CHECK(asgn[0].layout == LinePwlLayout::uniform);
  CHECK(asgn[2].K == 0);
  CHECK(asgn[3].K == 0);
  // Only the one lossy line gets a meaningful assignment.
  CHECK(asgn[1].K >= 2);
}

TEST_CASE(
    "compute_dynamic_loss_layout — Phase 1.5 preserves budgets")  // NOLINT
{
  // Phase 1.5 attempts K reduction after Phase 1+2.  Safety invariant:
  // it must NEVER produce a final state that violates either the
  // worst-case or the mean budget once those budgets were satisfiable
  // at the Phase 2 output.  Run on a 5-line mix at err_pct = 0.05
  // (loose enough that the cube-root rule lands every K_i inside
  // [floor, ceiling] without clamps) and verify both budgets hold.
  const std::vector<double> R {0.0001, 0.001, 0.005, 0.05, 0.10};
  const std::vector<double> fmax {3000.0, 500.0, 100.0, 50.0, 10.0};
  const line_losses::DynamicLayoutOpts opts {.err_pct = 0.05};
  const auto asgn = run_dynamic(R, fmax, opts);

  double L_total = 0.0;
  double worst = 0.0;
  double running = 0.0;
  for (std::size_t i = 0; i < R.size(); ++i) {
    if (R[i] <= 0.0 || fmax[i] <= 0.0 || asgn[i].K == 0) {
      continue;
    }
    const double L = R[i] * fmax[i] * fmax[i];
    const double kk =
        static_cast<double>(asgn[i].K) * static_cast<double>(asgn[i].K);
    L_total += L;
    worst += L / (4.0 * kk);
    if (asgn[i].layout == LinePwlLayout::midpoint) {
      running -= L / (12.0 * kk);
    } else {
      running += L / (6.0 * kk);
    }
  }
  const double budget = opts.err_pct * L_total;
  CHECK(worst <= budget);  // worst-case bound holds
  CHECK(std::abs(running) <= budget);  // signed mean bound holds
}

TEST_CASE("compute_dynamic_loss_layout — Phase 1.5 is idempotent")  // NOLINT
{
  // Running the rule twice on the same inputs must produce the same
  // assignment.  This pins the determinism / fixed-point property of
  // Phase 1.5's iterative reduction pass — without it the algorithm
  // would be sensitive to call-order quirks.
  const std::vector<double> R {0.0001, 0.001, 0.005, 0.05, 0.10};
  const std::vector<double> fmax {3000.0, 500.0, 100.0, 50.0, 10.0};
  const auto first = run_dynamic(R, fmax, {.err_pct = 0.05});
  const auto second = run_dynamic(R, fmax, {.err_pct = 0.05});
  REQUIRE(first.size() == second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CHECK(first[i].K == second[i].K);
    CHECK(first[i].layout == second[i].layout);
  }
}
