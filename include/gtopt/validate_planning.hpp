/**
 * @file      validate_planning.hpp
 * @brief     Semantic validation of a parsed Planning object
 * @date      Wed Mar 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides `validate_planning()`, which checks referential integrity,
 * range constraints, and structural completeness of a Planning object
 * after JSON parsing but before LP construction.
 */

#pragma once

#include <string>
#include <vector>

#include <gtopt/planning.hpp>

namespace gtopt
{

/**
 * @brief Result of semantic validation
 *
 * Contains separate lists of errors (fatal) and warnings (non-fatal).
 * When `errors` is non-empty the caller should abort LP construction.
 */
struct ValidationResult
{
  std::vector<std::string> errors {};
  std::vector<std::string> warnings {};

  [[nodiscard]] constexpr bool ok() const noexcept { return errors.empty(); }
};

/**
 * @brief Validate a Planning object for semantic correctness
 *
 * Checks performed:
 *   1. Referential integrity: cross-references between components
 *      (generator.bus, demand.bus, line.bus_a/bus_b, turbine.waterway,
 *      turbine.generator, flow.junction, waterway.junction_a/junction_b,
 *      converter.battery/generator/demand, reservoir.junction)
 *   2. Range checks: block duration > 0, stage count_block > 0,
 *      generator capacity non-negative
 *   3. Completeness: at least one bus, block, and stage
 *
 * All issues are collected (not short-circuited) so the user sees every
 * problem in a single run.
 *
 * @param planning The parsed and merged Planning object
 * @return ValidationResult with all errors and warnings
 */
[[nodiscard]] ValidationResult validate_planning(const Planning& planning);

}  // namespace gtopt
