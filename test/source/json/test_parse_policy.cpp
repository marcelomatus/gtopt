// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_parse_policy.cpp
 * @brief     Tests for the StrictParsePolicy exact-mapping contract
 * @date      2026-04-15
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * StrictParsePolicy is the sole daw::json parse-flag set used across gtopt.
 * It carries UseExactMappingsByDefault::yes so that any JSON key outside the
 * schema is rejected at parse time instead of being silently ignored.  These
 * tests pin that contract: if anyone relaxes the policy, the test fails.
 */

#include <string>

#include <daw/json/daw_json_exception.h>
#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("StrictParsePolicy - unknown top-level field is rejected")
{
  // "bogus_option" is not a member of PlanningOptions.  Under the exact-
  // mapping policy this must throw; under a lenient policy it would be
  // silently ignored.
  const std::string json_with_unknown = R"({
    "demand_fail_cost": 1000.0,
    "bogus_option": 42
  })";

  CHECK_THROWS_AS((void)daw::json::from_json<PlanningOptions>(
                      json_with_unknown, StrictParsePolicy),
                  daw::json::json_exception);
}

TEST_CASE("StrictParsePolicy - unknown nested field is rejected")
{
  // Unknown key nested inside planning.options must also fail.
  const std::string json_with_unknown = R"({
    "options": {
      "demand_fail_cost": 1000.0,
      "not_a_real_option": true
    }
  })";

  CHECK_THROWS_AS((void)daw::json::from_json<Planning>(json_with_unknown,
                                                       StrictParsePolicy),
                  daw::json::json_exception);
}

TEST_CASE("StrictParsePolicy - valid JSON parses cleanly")
{
  // Control case: the same JSON without the stray key must parse without
  // throwing.  Guards against accidentally tightening the policy to reject
  // legal input.
  const std::string valid_json = R"({
    "demand_fail_cost": 1000.0
  })";

  CHECK_NOTHROW((void)daw::json::from_json<PlanningOptions>(valid_json,
                                                            StrictParsePolicy));
}
