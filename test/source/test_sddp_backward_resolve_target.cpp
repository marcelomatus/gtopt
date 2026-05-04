// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_backward_resolve_target.cpp
 * @brief     Plumbing tests for `SDDPOptions::backward_resolve_target`.
 * @date      2026-05-03
 *
 * Coverage axes:
 *   B1: ``SddpOptions::backward_resolve_target`` defaults to nullopt.
 *   B2: ``PlanningOptionsLP::sddp_backward_resolve_target()`` returns the
 *       documented default ``false`` when the option is absent.
 *   B3: An explicit JSON ``"backward_resolve_target": true`` round-trips
 *       through the option struct unchanged.
 *   B4: ``SddpOptions::merge`` propagates the override.
 *
 * Semantic verification (does the cut at α_{t-1} actually become
 * tighter when ``backward_resolve_target=true``?) is covered by the
 * integration tests under ``integration_test/`` rather than this unit
 * file — wiring up a full SDDP fixture in doctest is heavier than the
 * payoff of a single-iteration RHS comparison.  The plumbing tests
 * here ensure the option flows from JSON to the materialised
 * ``SDDPOptionsLP`` struct read by ``backward_pass_single_phase`` and
 * the aperture bcut fallback.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_options.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(  // NOLINT
    "SddpOptions::backward_resolve_target defaults to nullopt")
{
  SddpOptions opts;
  CHECK_FALSE(opts.backward_resolve_target.has_value());
}

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_backward_resolve_target defaults to false")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  // No explicit setter call → resolves to
  // default_sddp_backward_resolve_target which is `false`.
  PlanningLP plp(std::move(planning));
  CHECK_FALSE(plp.options().sddp_backward_resolve_target());
}

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_backward_resolve_target respects explicit true")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.backward_resolve_target = true;
  PlanningLP plp(std::move(planning));
  CHECK(plp.options().sddp_backward_resolve_target());
}

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_backward_resolve_target respects explicit false")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.backward_resolve_target = false;
  PlanningLP plp(std::move(planning));
  CHECK_FALSE(plp.options().sddp_backward_resolve_target());
}

TEST_CASE(  // NOLINT
    "SddpOptions::merge propagates backward_resolve_target override")
{
  SddpOptions base;
  REQUIRE_FALSE(base.backward_resolve_target.has_value());

  SddpOptions override_opts;
  override_opts.backward_resolve_target = true;

  base.merge(std::move(override_opts));
  REQUIRE(base.backward_resolve_target.has_value());
  CHECK(*base.backward_resolve_target);
}

TEST_CASE(  // NOLINT
    "SddpOptions::merge keeps base.backward_resolve_target when override "
    "absent")
{
  SddpOptions base;
  base.backward_resolve_target = true;

  SddpOptions override_opts;
  REQUIRE_FALSE(override_opts.backward_resolve_target.has_value());

  base.merge(std::move(override_opts));
  REQUIRE(base.backward_resolve_target.has_value());
  CHECK(*base.backward_resolve_target);
}
