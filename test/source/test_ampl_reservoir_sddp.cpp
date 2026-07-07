// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_ampl_reservoir_sddp.cpp
 * @brief     Multi-phase user-constraint resolution validated against the
 *            native gtopt reservoir under method=sddp.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * Companion coverage for the fix that makes element-attribute user
 * constraints (`generator('g').generation`, `reservoir('r').energy`,
 * `turbine('t').generation`, …) resolve under `method=sddp` / `cascade`
 * — i.e. on the per-cell rebuild flatten (`write_out` /
 * `rebuild_collections_if_needed`) where the production path had freed the
 * AMPL registry (`release_ampl_after_flatten`).
 *
 * Strategy — the native reservoir is the ORACLE.  We reuse the shared
 * `make_2scene_10phase_two_reservoir_planning()` fixture (2 scenes,
 * 10 phases, 2 native reservoirs each with a hard efin terminal target,
 * 2 hydro turbines + 1 thermal backup), already exercised by
 * test_sddp_bounds_sanity / test_benders_cut_pi_convention.  We then layer
 * AMPL user constraints that REFERENCE those native elements across every
 * phase and assert:
 *
 *   1. the native fixture converges (LB ≤ UB, finite) — the oracle;
 *   2. non-binding user constraints resolve on every phase (they would
 *      throw "unknown attribute" without the fix) and leave the native
 *      bounds unchanged;
 *   3. a binding user constraint (forces curtailment) raises the bound,
 *      proving it is assembled into every phase's LP (not silently dropped);
 *   4. a multi-phase `state` variable (DecisionVariable state+link) links
 *      across all 10 phases and the SDDP solve still converges.
 *
 * NOTE on "replicating" a reservoir purely in AMPL: gtopt's cross-phase
 * volume balance is an ENGINE primitive (the native Reservoir / the
 * StateVariable cut coupling); the user-facing `state(...)` wrapper is a
 * typing assertion, not a value-coupling operator.  The faithful
 * equivalence check is therefore "AMPL constraints layered on the native
 * reservoir reproduce the native result", which is what these tests assert.
 */

#include <cmath>
#include <string>
#include <utility>

#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace ampl_reservoir_sddp_test
{
namespace
{

/// Solve `planning` with SDDP (cut_sharing=none — the only valid mode for
/// the heterogeneous 2-scene fixture) and return {lower_bound, upper_bound}
/// at the last iteration.  Settings mirror test_sddp_bounds_sanity so the
/// strict LB ≤ UB invariant holds.
[[nodiscard]] auto solve_sddp_bounds(Planning planning)
    -> std::pair<double, double>
{
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 8;
  opts.convergence_tol = 1.0e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  return {results->back().lower_bound, results->back().upper_bound};
}

/// Append one user constraint to a fresh copy of the 2-reservoir fixture and
/// solve.  This is the exact production path that used to throw on the
/// per-phase rebuild flatten.
[[nodiscard]] auto solve_with_uc(Uid uid,
                                 std::string name,
                                 std::string expression)
    -> std::pair<double, double>
{
  auto planning = make_2scene_10phase_two_reservoir_planning();
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = uid,
      .name = std::move(name),
      .expression = std::move(expression),
  });
  return solve_sddp_bounds(std::move(planning));
}

}  // namespace
}  // namespace ampl_reservoir_sddp_test

TEST_CASE(
    "AMPL reservoir SDDP — native 2-reservoir 10-phase fixture converges "
    "(oracle)")
{
  using namespace ampl_reservoir_sddp_test;

  const auto [lb, ub] =
      solve_sddp_bounds(make_2scene_10phase_two_reservoir_planning());
  CHECK(std::isfinite(lb));
  CHECK(std::isfinite(ub));
  // cut_sharing=none ⇒ the lower bound is a valid under-estimate.
  CHECK(lb <= ub + 1.0e-4 * (1.0 + std::abs(ub)));
}

TEST_CASE(
    "AMPL reservoir SDDP — non-binding user constraints resolve per-phase and "
    "preserve the native bounds")
{
  using namespace ampl_reservoir_sddp_test;

  const auto [lb0, ub0] =
      solve_sddp_bounds(make_2scene_10phase_two_reservoir_planning());

  // Each references a native element attribute across EVERY one of the 10
  // phases.  Without the fix they throw "unknown attribute" on the
  // write_out / rebuild flatten; being non-binding, a correct build leaves
  // the native bounds bit-for-bit unchanged (the rows carry zero duals, so
  // the LP value, cuts and SDDP trajectory are identical).
  const auto same = [&](const std::pair<double, double>& r)
  {
    CHECK(r.first == doctest::Approx(lb0).epsilon(1e-3));
    CHECK(r.second == doctest::Approx(ub0).epsilon(1e-3));
  };

  SUBCASE("reservoir('rsv1').energy >= 0")
  {
    same(solve_with_uc(
        Uid {100}, "rsv1_floor", "reservoir('rsv1').energy >= 0"));
  }
  SUBCASE("generator('hydro_gen_1').generation >= 0")
  {
    same(solve_with_uc(
        Uid {101}, "gen_floor", "generator('hydro_gen_1').generation >= 0"));
  }
  SUBCASE("turbine('tur2').generation <= 100000")
  {
    same(solve_with_uc(
        Uid {102}, "tur_cap_loose", "turbine('tur2').generation <= 100000"));
  }
  SUBCASE("sum over both reservoirs and all generators (loose)")
  {
    same(solve_with_uc(Uid {103},
                       "sys_loose",
                       "sum(reservoir(all).energy) "
                       "+ sum(generator(all).generation) <= 1000000"));
  }
}

TEST_CASE(
    "AMPL reservoir SDDP — binding user constraint changes the bound across "
    "phases")
{
  using namespace ampl_reservoir_sddp_test;

  const auto [lb0, ub0] =
      solve_sddp_bounds(make_2scene_10phase_two_reservoir_planning());

  // Demand is 40/block; capping total generation at 30 forces ≥10/block of
  // unserved load (priced at demand_fail_cost) in every phase — a large,
  // unambiguous bound increase that only materialises if the constraint is
  // actually assembled into each phase's LP (the fix).
  const auto [lb, ub] = solve_with_uc(
      Uid {110}, "gen_cap", "sum(generator(all).generation) <= 30");
  CHECK(lb > lb0 + 1000.0);
  CHECK(lb <= ub + 1.0e-4 * (1.0 + std::abs(ub)));  // still a valid solve
}

TEST_CASE(
    "AMPL reservoir SDDP — multi-phase state variable links across 10 phases "
    "and SDDP converges")
{
  using namespace ampl_reservoir_sddp_test;

  // A global `state` DecisionVariable (state + link) is the multi-stage /
  // phase variable: it registers one column per phase and chains forward
  // through every phase, riding the SDDP backward-pass cuts — alongside the
  // two native reservoir states.
  const auto make_with_state = []
  {
    auto planning = make_2scene_10phase_two_reservoir_planning();
    planning.system.decision_variable_array.push_back(DecisionVariable {
        .uid = Uid {99},
        .name = "user_state",
        .lower_bound = OptReal {-1.0e6},
        .upper_bound = OptReal {1.0e6},
        .scope = OptName {"global"},
        .state = OptBool {true},
        .link = OptBool {true},
    });
    return planning;
  };

  SUBCASE("link chain spans all 10 phases (structural)")
  {
    PlanningLP planning_lp(make_with_state());
    const auto& sim = planning_lp.simulation();
    REQUIRE(static_cast<int>(sim.phases().size()) == 10);
    const auto scene = first_scene_index();

    for (int pi = 0; pi < 10; ++pi) {
      const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
      bool found = false;
      for (const auto& [key, svar] : sv_map) {
        if (key.class_name == DecisionVariableLP::StateClassName
            && key.uid == Uid {99})
        {
          found = true;
          if (pi < 9) {
            CHECK_FALSE(svar.dependent_variables().empty());
          } else {
            CHECK(svar.dependent_variables().empty());  // last phase
          }
        }
      }
      CHECK(found);
    }
  }

  SUBCASE("SDDP converges with the 10-phase state variable present")
  {
    const auto [lb, ub] = solve_sddp_bounds(make_with_state());
    CHECK(std::isfinite(lb));
    CHECK(std::isfinite(ub));
    CHECK(lb <= ub + 1.0e-4 * (1.0 + std::abs(ub)));
  }
}
