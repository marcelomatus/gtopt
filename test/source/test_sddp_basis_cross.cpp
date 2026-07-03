// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_basis_cross.cpp
 * @brief     Tests for SDDP cross-pass basis warm-start (`BasisCrossMode`).
 * @date      2026-07-02
 *
 * Two axes:
 *
 *  1. Plumbing — `SddpOptions::basis_cross_mode` defaults to nullopt, the
 *     `PlanningOptionsLP` accessor resolves an absent value to `off`, an
 *     explicit value round-trips, `merge` propagates it, and the enum name
 *     parser accepts the canonical names + aliases.
 *
 *  2. Correctness invariant — a cross basis is only a *warm start*: it seeds
 *     the simplex from a related pass's optimum but the solver still solves
 *     to the same optimum, so it can never change the converged bounds or the
 *     cut sequence.  Every `BasisCrossMode` must therefore produce the SAME
 *     converged upper/lower bounds as `off` on a fixed SDDP fixture.  This
 *     holds on every backend: those without basis save/restore (get_basis →
 *     nullopt / set_basis → false) simply fall back to a cold solve, which is
 *     bit-for-bit `off`.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_options.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(bugprone-unchecked-optional-access, misc-const-correctness)

// ═══════════════════════════════════════════════════════════════════════════
// 1. Plumbing
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SddpOptions::basis_cross_mode defaults to nullopt")  // NOLINT
{
  const SddpOptions opts;
  CHECK_FALSE(opts.basis_cross_mode.has_value());
}

TEST_CASE("PlanningOptionsLP::sddp_basis_cross_mode defaults to full_cross")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));
  // Default flipped off→full_cross (2026-07-03): warm-start everywhere +
  // forward basis fed into the backward/aperture solves is the optimal SDDP
  // config; convergence-safe (valid vertex-dual cuts).
  CHECK(plp.options().sddp_basis_cross_mode() == BasisCrossMode::full_cross);
}

TEST_CASE(
    "PlanningOptionsLP::sddp_basis_cross_mode respects explicit")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.basis_cross_mode =
      BasisCrossMode::forward_to_backward;
  PlanningLP plp(std::move(planning));
  CHECK(plp.options().sddp_basis_cross_mode()
        == BasisCrossMode::forward_to_backward);
}

TEST_CASE("SddpOptions::merge propagates basis_cross_mode override")  // NOLINT
{
  SddpOptions base;
  REQUIRE_FALSE(base.basis_cross_mode.has_value());

  SddpOptions override_opts;
  override_opts.basis_cross_mode = BasisCrossMode::full_cross;

  base.merge(std::move(override_opts));
  REQUIRE(base.basis_cross_mode.has_value());
  CHECK(*base.basis_cross_mode == BasisCrossMode::full_cross);
}

TEST_CASE(
    "SddpOptions::merge keeps base basis_cross_mode when absent")  // NOLINT
{
  SddpOptions base;
  base.basis_cross_mode = BasisCrossMode::backward_to_forward;

  SddpOptions override_opts;
  REQUIRE_FALSE(override_opts.basis_cross_mode.has_value());

  base.merge(std::move(override_opts));
  REQUIRE(base.basis_cross_mode.has_value());
  CHECK(*base.basis_cross_mode == BasisCrossMode::backward_to_forward);
}

TEST_CASE("BasisCrossMode - enum name parsing and aliases")  // NOLINT
{
  SUBCASE("canonical names")
  {
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "off")
          == BasisCrossMode::off);
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "warm")
          == BasisCrossMode::warm);
    CHECK(
        require_enum<BasisCrossMode>("basis_cross_mode", "forward_to_backward")
        == BasisCrossMode::forward_to_backward);
    CHECK(
        require_enum<BasisCrossMode>("basis_cross_mode", "backward_to_forward")
        == BasisCrossMode::backward_to_forward);
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "full_cross")
          == BasisCrossMode::full_cross);
  }

  SUBCASE("aliases")
  {
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "fwd2bwd")
          == BasisCrossMode::forward_to_backward);
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "bwd2fwd")
          == BasisCrossMode::backward_to_forward);
    CHECK(require_enum<BasisCrossMode>("basis_cross_mode", "cross")
          == BasisCrossMode::full_cross);
  }

  SUBCASE("invalid value has no mapping")
  {
    CHECK_FALSE(enum_from_name<BasisCrossMode>("nope").has_value());
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Correctness invariant: every cross mode == off on the converged bounds
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SDDPMethod - basis_cross_mode is bound-invariant vs off")  // NOLINT
{
  constexpr int kIters = 8;
  constexpr double kConvTol = 1e-5;
  constexpr double kParityTol = 1e-6;

  const auto run_with_mode =
      [&](BasisCrossMode mode) -> std::pair<double, double>
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = kIters;
    sddp_opts.convergence_tol = kConvTol;
    sddp_opts.enable_api = false;
    // backward_resolve_target defaults true → the forward<->backward cross
    // basis actually fires at the LP_t re-solve site.
    sddp_opts.basis_cross_mode = mode;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    const auto& last = results->back();
    return {last.upper_bound, last.lower_bound};
  };

  const auto [ub0, lb0] = run_with_mode(BasisCrossMode::off);

  for (const auto mode : {BasisCrossMode::warm,
                          BasisCrossMode::forward_to_backward,
                          BasisCrossMode::backward_to_forward,
                          BasisCrossMode::full_cross})
  {
    CAPTURE(static_cast<int>(mode));
    const auto [ub, lb] = run_with_mode(mode);
    CHECK(ub == doctest::Approx(ub0).epsilon(kParityTol));
    CHECK(lb == doctest::Approx(lb0).epsilon(kParityTol));
  }
}

// NOLINTEND(bugprone-unchecked-optional-access, misc-const-correctness)
