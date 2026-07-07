// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file   test_user_constraint_diagnostics.cpp
 * @brief  Parse-time source-location diagnostics for unresolved AMPL
 *         references in UserConstraint expressions (task #55, the P1
 *         safety win).
 *
 * The parser stamps each ``ConstraintTerm`` with the 1-based column of
 * its anchor token at parse time (``ConstraintTerm::column``).  When
 * ``UserConstraintLP`` later fails to resolve a reference at LP-build
 * time, ``make_unresolved_element_error`` surfaces the stored column
 * in the message as ``at column N`` so the user sees the exact source
 * offset of the offending token, not just the constraint name.
 *
 * These tests pin the contract on two distinct cases:
 *   * the bad reference is the FIRST term — column 1
 *   * the bad reference is NOT the first term — column == byte offset
 *     of the offending element-type token (1-based)
 *
 * Independent of ``test_user_constraint_strict_errors.cpp`` so this
 * file ships cleanly on its own.
 */

#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace uc_diagnostics_test  // unique outer namespace (unity-build safe)
{

// Build a PlanningLP from JSON and return the exception message, or an
// empty string if no exception was thrown.  Construction triggers LP
// build (UserConstraintLP ctor parses; add_to_lp lowers the rows), so
// resolver-time diagnostics surface here.
[[nodiscard]] std::string build_error_message(std::string_view json)
{
  try {
    auto planning = parse_planning_json(json);
    PlanningLP planning_lp(std::move(planning));
    auto result = planning_lp.resolve();
    (void)result;
  } catch (const std::exception& ex) {
    return ex.what();
  }
  return {};
}

// clang-format off
[[nodiscard]] std::string make_json(std::string_view uc_name,
                                    std::string_view expr)
{
  static constexpr std::string_view tmpl = R"json(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 1000
      }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 1 }} ],
      "stage_array": [ {{ "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }} ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "uc_diagnostics",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": 1, "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": 1, "lmax": [ [ 80.0 ] ] }}
      ],
      "user_constraint_array": [
        {{ "uid": 1, "name": "{}", "expression": "{}" }}
      ]
    }}
  }})json";
  return std::format(tmpl, uc_name, expr);
}
// clang-format on

TEST_CASE(  // NOLINT
    "UC P1 — leading-term unresolved ref reports 'at column 1'")
{
  // For ``"generator('does_not_exist').generation <= 100"`` the anchor
  // is the leading ``generator`` token at byte offset 0 — 1-based
  // column 1.  This pins the no-off-by-one contract: the parser stores
  // column 1-based natively (sentinel 0 = unset) so a term anchored at
  // byte offset 0 surfaces as ``at column 1`` and is NOT silently
  // dropped by an ``if (column > 0)`` guard.
  const auto msg = build_error_message(make_json(
      "uc_leading_bad", "generator('does_not_exist').generation <= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_leading_bad") != std::string::npos);
  CHECK(msg.find("does_not_exist") != std::string::npos);
  CHECK(msg.find("unknown generator name") != std::string::npos);
  CHECK(msg.find("at column 1") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC P1 — middle-term unresolved ref reports byte-offset 1-based column")
{
  // ``"generator('g1').generation + generator('does_not_exist').generation
  // <= 100"`` — the SECOND ``generator`` token starts at byte offset
  // 29 (after the leading ``generator('g1').generation + `` prefix,
  // 0-indexed offset 29) → 1-based column 30.  Pins the contract that
  // ``at column N`` actually tracks the parser's ``Token.start_pos``
  // and isn't just hardcoded to 1.
  const auto msg = build_error_message(make_json(
      "uc_middle_bad",
      "generator('g1').generation + generator('does_not_exist').generation "
      "<= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_middle_bad") != std::string::npos);
  CHECK(msg.find("does_not_exist") != std::string::npos);
  CHECK(msg.find("at column 30") != std::string::npos);
}

}  // namespace uc_diagnostics_test
