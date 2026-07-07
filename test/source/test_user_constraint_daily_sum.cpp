/**
 * @file      test_user_constraint_daily_sum.cpp
 * @brief     Contract tests for the daily-sum (per-day energy) UserConstraint
 * @date      2026-05-27
 * @copyright BSD-3-Clause
 *
 * TDD contract for the pending daily-sum UserConstraint feature (GH #502):
 * a constraint that sums its (Δt-weighted) per-block terms over each 24 h
 * day and emits ONE row per day — modelling PLEXOS `RHS Day` constraints
 * (RALCO_max_e1/e2, CANUTILLARreserve, Diesel_OffTakeDay, *_Crew).
 *
 * ### API contract pinned here
 * - `"daily_sum": true` on a UserConstraint switches the LP from the default
 *   one-row-per-block expansion to **one row per day**: the per-block terms
 *   are accumulated into a running row that is flushed (`lp.add_row`) at each
 *   *day-ending block* (cumulative block duration crossing a 24 h boundary,
 *   or the stage's last block).
 * - `"constraint_type": "energy"` makes each block's contribution
 *   Δt-weighted (`coeff · Δt_b · col_b`), so the LHS is a daily ENERGY sum
 *   `Σ_day gen·Δt` [MWh] and the RHS is the daily energy budget [MWh].
 *   (Without `energy`, the daily sum is unweighted — a per-day COUNT, e.g.
 *   `Σ_day startup ≤ N` for crew limits.)
 *
 * The RHS unit is GWh in the PLEXOS source (`RHS Day`); the converter
 * multiplies by 1000 to MWh, so these tests state the RHS directly in MWh.
 *
 * HISTORY: these cases were originally authored as `doctest::skip(true)`
 * TDD placeholders, referencing the then-not-yet-existing `daily_sum`
 * JSON field (which would throw under StrictParsePolicy).  The skips were
 * removed once `user_constraint_lp.cpp` + `json_user_constraint.hpp`
 * gained `daily_sum`.
 */

#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions;
// the nested anonymous namespace gives the helpers internal linkage.
namespace ucds_test
{
namespace
{

// clang-format off

/// A 1-stage, 4-block system spanning 2 days (4 × 12 h = 48 h):
///   cumulative hours 12, 24, 36, 48 → day 0 = blocks {0,1}, day 1 = {2,3}.
/// Demand is a flat 100 MW every block (so each day needs 100 MW × 24 h =
/// 2400 MWh).  g1 is cheap (gcost 5), g2 expensive (gcost 50).
///
/// `uc` is spliced into `user_constraint_array` verbatim so each test can
/// vary the daily-sum clause / RHS.  `scale_objective = 1` keeps the raw
/// objective equal to the physical cost.
auto make_json(std::string_view uc) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "demand_fail_cost": 100000,
        "scale_objective": 1
      }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 12 }},
        {{ "uid": 2, "duration": 12 }},
        {{ "uid": 3, "duration": 12 }},
        {{ "uid": 4, "duration": 12 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 4, "active": 1,
           "chronological": true }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "daily_sum_uc_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 5,  "capacity": 100 }},
        {{ "uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 50, "capacity": 100 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1",
           "lmax": [[100.0, 100.0, 100.0, 100.0]] }}
      ],
      "user_constraint_array": [{}]
    }}
  }}
)",
      uc);
}

// clang-format on

[[nodiscard]] auto solve(std::string_view uc) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_json(uc)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

}  // namespace
}  // namespace ucds_test

TEST_CASE("UserConstraint daily_sum — baseline (no constraint)")  // NOLINT
{
  using namespace ucds_test;
  // No daily limit: cheap g1 serves all 4800 MWh.
  //   cost = 4800 MWh × 5 = 24000.
  const double obj = solve(R"({ "uid": 1, "name": "noop",
      "expression": "generator('g1').generation >= 0" })");
  CHECK(obj == doctest::Approx(24000.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint daily_sum — per-day energy budget binds independently")
{
  using namespace ucds_test;

  // Daily ENERGY budget on g1: Σ_day g1·Δt ≤ 1200 MWh.  Each day needs
  // 2400 MWh, so g1 supplies 1200 and the expensive g2 the other 1200,
  // EVERY day.
  //   per day:  1200·5 + 1200·50 = 66000  →  × 2 days = 132000.
  const double obj = solve(R"({ "uid": 1, "name": "g1_daily_energy",
      "daily_sum": true, "constraint_type": "energy",
      "expression": "generator('g1').generation <= 1200" })");

  CHECK(obj == doctest::Approx(132000.0));

  // Distinguish from the WRONG expansions:
  //  - per-STAGE single row (Σ over all 48 h ≤ 1200): g1=1200 total, g2=3600
  //      → 1200·5 + 3600·50 = 186000.
  //  - unconstrained: 24000.
  CHECK(obj != doctest::Approx(186000.0));
  CHECK(obj != doctest::Approx(24000.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint daily_sum — loose budget is non-binding")
{
  using namespace ucds_test;
  // Budget 3000 MWh/day > the 2400 MWh/day a single g1 could ever supply
  // (100 MW × 24 h = 2400) → never binds → cheap baseline 24000.
  const double obj = solve(R"({ "uid": 1, "name": "g1_loose",
      "daily_sum": true, "constraint_type": "energy",
      "expression": "generator('g1').generation <= 3000" })");
  CHECK(obj == doctest::Approx(24000.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint daily_sum — two-unit sum mirrors CANUTILLARreserve")
{
  using namespace ucds_test;
  // Σ_day (g1 + g2)·Δt ≤ 3000 MWh caps TOTAL daily energy below the
  // 2400×... no — total daily demand is 2400 MWh, so a 3000 cap is loose;
  // tighten to 1800 MWh/day total → 600 MWh/day of demand goes unserved at
  // demand_fail_cost.  Cheapest served: g1 supplies the full 1800 (cheap).
  //   per day:  1800·5 (g1)  +  600·100000 (unserved)  = 9000 + 60000000.
  //   × 2 days = 120 018 000.
  const double obj = solve(R"({ "uid": 1, "name": "total_daily_energy",
      "daily_sum": true, "constraint_type": "energy",
      "expression": "generator('g1').generation + generator('g2').generation <= 1800" })");
  CHECK(obj == doctest::Approx(120018000.0));
}
