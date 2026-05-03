// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_renormalize_scenario_probabilities.cpp
 * @brief     Unit tests for ``PlanningLP::renormalize_scenario_probabilities``
 * @date      2026-05-02
 *
 * Pins the contract from ``Scenario.hpp`` that the solver normalises
 * ``probability_factor`` values to sum to 1.0 over scenarios in the
 * active subproblem (active scenes containing active scenarios).
 *
 * Coverage:
 *   1. Already-normalised input → no-op.
 *   2. Sum > 1 (e.g. 16 × 1/16 == 1, or 16 × 1.0 == 16) → rescale to 1.
 *   3. Sum < 1 (e.g. 7 × 1/16 == 0.4375 with 9 scenes deactivated) →
 *      rescale active subset to sum 1, leaving inactive scenarios
 *      untouched.
 *   4. Inactive-scene exclusion: scenarios in inactive scenes are not
 *      rescaled and not counted toward the renormalisation total.
 *   5. Idempotent — second call is a no-op.
 *   6. Degenerate input (every active scenario has prob = 0) →
 *      WARN-level log, no rescale.
 *   7. Empty scenario_array → silent no-op.
 */

#include <doctest/doctest.h>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

/// Helper: build a Planning fixture with N scenarios (each in its own
/// scene) and explicit probability factors.  Each scene is active by
/// default; pass ``inactive_scene_uids`` to deactivate specific scenes.
[[nodiscard]] Planning make_planning(
    const std::vector<double>& scenario_probs,
    const std::vector<int>& inactive_scene_uids = {})
{
  Array<Scenario> scenario_array;
  Array<Scene> scene_array;
  scenario_array.reserve(scenario_probs.size());
  scene_array.reserve(scenario_probs.size());
  for (std::size_t i = 0; i < scenario_probs.size(); ++i) {
    const auto uid = static_cast<gtopt::Uid>(i + 1);
    scenario_array.push_back(Scenario {
        .uid = uid,
        .probability_factor = scenario_probs[i],
    });
    Scene sc {
        .uid = uid,
        .name = {},
        .active = {},
        .first_scenario = i,
        .count_scenario = 1,
    };
    if (std::ranges::find(inactive_scene_uids, uid)
        != inactive_scene_uids.end())
    {
      sc.active = false;
    }
    scene_array.push_back(std::move(sc));
  }
  return Planning {
      .simulation =
          {
              .scenario_array = std::move(scenario_array),
              .scene_array = std::move(scene_array),
          },
  };
}

}  // namespace

TEST_CASE(
    "renormalize_scenario_probabilities — already-normalised input is a "
    "no-op")  // NOLINT
{
  // Two scenarios summing to 1.0 already: rescale must not change them.
  Planning p = make_planning({0.7, 0.3});
  PlanningLP::renormalize_scenario_probabilities(p);
  REQUIRE(p.simulation.scenario_array.size() == 2);
  CHECK(p.simulation.scenario_array[0].probability_factor.value_or(-1.0)
        == doctest::Approx(0.7));
  CHECK(p.simulation.scenario_array[1].probability_factor.value_or(-1.0)
        == doctest::Approx(0.3));
}

TEST_CASE(
    "renormalize_scenario_probabilities — sum > 1 rescales to 1")  // NOLINT
{
  // Four scenarios with prob 1.0 each → total 4.0; expect 0.25 each.
  Planning p = make_planning({1.0, 1.0, 1.0, 1.0});
  PlanningLP::renormalize_scenario_probabilities(p);
  for (const auto& sc : p.simulation.scenario_array) {
    CHECK(sc.probability_factor.value_or(-1.0) == doctest::Approx(0.25));
  }
}

TEST_CASE(
    "renormalize_scenario_probabilities — sum < 1 (inactive scenes excluded) "
    "rescales the active subset to 1")  // NOLINT
{
  // 16 scenarios at 0.0625 each = total 1.0, but 9 of 16 scenes are
  // marked inactive (juan/gtopt_iplp pattern: only 7 hydrologies
  // feasible).  After rescale the 7 active scenarios should each have
  // 1/7 ≈ 0.1428…; the 9 inactive scenarios stay at 0.0625.
  std::vector<double> probs(16, 0.0625);
  // Deactivate scenes 1, 3, 7, 10, 11, 12, 13, 15, 16 (matches juan
  // single-bus failure set).
  std::vector<int> inactive {1, 3, 7, 10, 11, 12, 13, 15, 16};
  Planning p = make_planning(probs, inactive);

  PlanningLP::renormalize_scenario_probabilities(p);

  REQUIRE(p.simulation.scenario_array.size() == 16);
  // Scene UIDs 1..16 → scenario indices 0..15.  Active scenes:
  // {2, 4, 5, 6, 8, 9, 14} → scenario indices {1, 3, 4, 5, 7, 8, 13}.
  const std::vector<std::size_t> active_idx {1, 3, 4, 5, 7, 8, 13};
  const double expected = 1.0 / 7.0;
  for (auto i : active_idx) {
    CHECK(p.simulation.scenario_array[i].probability_factor.value_or(-1.0)
          == doctest::Approx(expected));
  }
  // Inactive scenarios untouched.
  const std::vector<std::size_t> inactive_idx {0, 2, 6, 9, 10, 11, 12, 14, 15};
  for (auto i : inactive_idx) {
    CHECK(p.simulation.scenario_array[i].probability_factor.value_or(-1.0)
          == doctest::Approx(0.0625));
  }

  // Sum over active subset must be exactly 1.0 (within FP tolerance).
  double sum_active = 0.0;
  for (auto i : active_idx) {
    sum_active +=
        p.simulation.scenario_array[i].probability_factor.value_or(0.0);
  }
  CHECK(sum_active == doctest::Approx(1.0).epsilon(1e-12));
}

TEST_CASE(
    "renormalize_scenario_probabilities — idempotent (second call is a "
    "no-op)")  // NOLINT
{
  // Run twice; second run should leave the result unchanged (within
  // FP tolerance).  Catches a regression where the rescale loop
  // forgets to gate on ``|total - 1| > tol``.
  Planning p = make_planning({2.0, 3.0, 5.0});  // total = 10
  PlanningLP::renormalize_scenario_probabilities(p);

  std::vector<double> after_first;
  for (const auto& sc : p.simulation.scenario_array) {
    after_first.push_back(sc.probability_factor.value_or(-1.0));
  }

  PlanningLP::renormalize_scenario_probabilities(p);

  REQUIRE(p.simulation.scenario_array.size() == after_first.size());
  for (std::size_t i = 0; i < after_first.size(); ++i) {
    CHECK(p.simulation.scenario_array[i].probability_factor.value_or(-1.0)
          == doctest::Approx(after_first[i]));
  }
}

TEST_CASE(
    "renormalize_scenario_probabilities — degenerate input (all-zero "
    "active probability) is a WARN-level no-op")  // NOLINT
{
  // Every active scenario has prob 0 → total = 0 → cannot rescale.
  // Function logs a WARN and leaves values unchanged (the LP cost
  // coefficients will be all-zero downstream, but that's a separate
  // concern surfaced via the WARN line).
  Planning p = make_planning({0.0, 0.0});
  PlanningLP::renormalize_scenario_probabilities(p);
  for (const auto& sc : p.simulation.scenario_array) {
    CHECK(sc.probability_factor.value_or(-1.0) == doctest::Approx(0.0));
  }
}

TEST_CASE(
    "renormalize_scenario_probabilities — empty scenario_array is a "
    "silent no-op")  // NOLINT
{
  Planning p {};
  REQUIRE(p.simulation.scenario_array.empty());
  PlanningLP::renormalize_scenario_probabilities(p);
  CHECK(p.simulation.scenario_array.empty());
}

TEST_CASE(
    "renormalize_scenario_probabilities — empty scene_array treats all "
    "scenarios as in-scope")  // NOLINT
{
  // ``create_scene_array`` synthesises a default scene covering all
  // scenarios when ``scene_array`` is empty.  ``renormalize_*``
  // matches: with no scene_array, every scenario is in scope.
  Planning p = make_planning({1.0, 1.0});
  p.simulation.scene_array.clear();  // empty scene_array
  PlanningLP::renormalize_scenario_probabilities(p);
  REQUIRE(p.simulation.scenario_array.size() == 2);
  CHECK(p.simulation.scenario_array[0].probability_factor.value_or(-1.0)
        == doctest::Approx(0.5));
  CHECK(p.simulation.scenario_array[1].probability_factor.value_or(-1.0)
        == doctest::Approx(0.5));
}
