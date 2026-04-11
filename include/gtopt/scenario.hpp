/**
 * @file      scenario.hpp
 * @brief     Stochastic scenario definition for power system planning
 * @date      Wed Mar 26 12:12:32 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A Scenario represents one possible realization of uncertain inputs (e.g.,
 * hydrology, demand level, wind output).  Scenarios are weighted by
 * `probability_factor` when computing the expected-cost objective.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "name": "dry_year", "probability_factor": 0.4}
 * ```
 *
 * @see ScenarioLP for the LP representation
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/uid.hpp>

namespace gtopt
{

/**
 * @struct Scenario
 * @brief One possible future realization in stochastic planning
 *
 * When `probability_factor` values across all scenarios do not sum to 1, the
 * solver normalises them internally.  A scenario can be deactivated (e.g. to
 * perform deterministic runs with a single scenario).
 */
struct Scenario
{
  Uid uid {unknown_uid};  ///< Unique identifier
  OptName name {};  ///< Optional human-readable label
  OptBool active {};  ///< Activation status (default: active)

  OptReal probability_factor {1};  ///< Probability weight of this scenario
                                   ///< [p.u.]; values are normalised to sum 1

  static constexpr std::string_view class_name = "scenario";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

using ScenarioUid = UidOf<Scenario>;
using ScenarioIndex = StrongPositionIndexType<struct Scenario>;

/// @brief First scenario index.
[[nodiscard]] constexpr auto first_scenario_index() noexcept -> ScenarioIndex
{
  return ScenarioIndex {0};
}

}  // namespace gtopt
