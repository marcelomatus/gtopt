/**
 * @file      cost_helper.hpp
 * @brief     Cost calculation and discounting utilities for optimization
 * problems
 * @date      Thu May 15 10:56:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the CostHelper class and supporting utilities for:
 * - Calculating time-discounted costs across scenarios, stages and blocks
 * - Applying probability factors to scenario costs
 * - Generating cost scaling factors for LP formulation
 * - Managing cumulative discount factors across the optimization horizon
 *
 * Key Features:
 * - Handles multi-period discounting with stage-specific rates
 * - Supports scenario probability weighting
 * - Provides both individual cost calculations and batch factor generation
 * - Constexpr and noexcept where possible for performance
 * - Thread-safe operations (all methods are const)
 *
 * @see CostHelper for the main class documentation
 * @see detail::stage_factors() for discount factor calculation
 */

#pragma once

#include <gtopt/block_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt::detail
{

/**
 * @brief Helper function to calculate stage discount factors
 *
 * Computes cumulative discount factors for each stage based on:
 * - Previous stage's discount factor
 * - Current stage's discount rate
 *
 * @param stages Vector of StageLP objects
 * @return Vector of cumulative discount factors
 */
constexpr auto stage_factors(const auto& stages) noexcept
{
  return stages
      | std::views::transform([](const auto& st)
                              { return st.discount_factor(); })
      | std::ranges::to<std::vector>();
}

}  // namespace gtopt::detail

namespace gtopt
{

/**
 * @class CostHelper
 * @brief Handles cost calculations with time discounting for optimization
 * problems
 *
 * Provides methods for:
 * - Calculating discounted costs for blocks, stages and scenarios
 * - Generating cost scaling factors
 * - Applying probability and discount factors
 */

// TODO(marcelo): change to "ecost_factors" methods names to "icost_factor"
// since they calculate the inverse cost factor required for example to compute
// dual prices. Also document the methods changed and apply the changes to
// cost_helper.cpp/hpp files.

class CostHelper
{
public:
  explicit CostHelper(const OptionsLP& options,
                      const std::vector<ScenarioLP>& scenarios,
                      const std::vector<StageLP>& stages)
      : m_options_(options)
      , m_scenarios_(scenarios)
      , m_stages_(stages)
      , m_stage_discount_factors_(detail::stage_factors(stages))
  {
  }

  /**
   * @brief Calculates the energy cost coefficient for a block
   *
   * Computes the total energy cost for a power variable over a block duration,
   * applying:
   * - Scenario probability weighting
   * - Stage discount factor
   * - Block duration
   * - Objective scaling
   *
   * Formula: cost * probability * discount * duration / scale_objective
   *
   * @param scenario_index Scenario index for probability factor
   * @param stage_index Stage index for discount factor
   * @param block Block containing duration
   * @param cost Energy cost in $/MWh
   * @return Total energy cost coefficient for LP formulation
   */
  [[nodiscard]] double block_ecost(const ScenarioIndex& scenario_index,
                                   const StageIndex& stage_index,
                                   const BlockLP& block,
                                   double cost) const;

  /**
   * @brief Calculates the energy cost coefficient for a stage
   *
   * Computes the total energy cost for a power variable over a stage duration,
   * applying:
   * - Probability weighting (default 1.0)
   * - Stage discount factor
   * - Stage duration
   * - Objective scaling
   *
   * Formula: cost * probability * discount * duration / scale_objective
   *
   * @param stage_index Stage index for discount factor and duration
   * @param cost Energy cost in $/MWh
   * @param probability_factor Probability weight (default 1.0)
   * @return Total energy cost coefficient for LP formulation
   */
  [[nodiscard]] double stage_ecost(const StageIndex& stage_index,
                                   double cost,
                                   double probability_factor = 1.0) const;

  /**
   * @brief Calculates the energy cost coefficient for a scenario-stage pair
   *
   * Computes the total energy cost for a power variable over a stage duration,
   * applying:
   * - Scenario probability weighting
   * - Stage discount factor
   * - Stage duration
   * - Objective scaling
   *
   * Formula: cost * probability * discount * duration / scale_objective
   *
   * @param scenario_index Scenario index for probability factor
   * @param stage_index Stage index for discount factor and duration
   * @param cost Energy cost in $/MWh
   * @return Total energy cost coefficient for LP formulation
   */
  [[nodiscard]] double scenario_stage_ecost(const ScenarioIndex& scenario_index,
                                            const StageIndex& stage_index,
                                            double cost) const;

  /**
   * @brief Calculates inverse cost factors for blocks (1/cost_factor)
   *
   * Computes the inverse of the cost factors used to scale dual prices,
   * applying:
   * - Scenario probability weighting
   * - Stage discount factor
   * - Block duration
   * - Objective scaling
   *
   * Formula: 1 / (probability * discount * duration / scale_objective)
   *
   * @return Matrix of inverse cost factors by scenario/stage/block
   */
  [[nodiscard]] block_factor_matrix_t block_icost_factors() const;

  /**
   * @brief Calculates inverse cost factors for stages (1/cost_factor)
   *
   * Computes the inverse of the cost factors used to scale dual prices,
   * applying:
   * - Stage discount factor
   * - Stage duration
   * - Objective scaling
   *
   * Formula: 1 / (discount * duration / scale_objective)
   *
   * @return Vector of inverse cost factors by stage
   */
  [[nodiscard]] stage_factor_matrix_t stage_icost_factors() const;

  /**
   * @brief Calculates inverse cost factors for scenario-stage pairs
   * (1/cost_factor)
   *
   * Computes the inverse of the cost factors used to scale dual prices,
   * applying:
   * - Scenario probability weighting
   * - Stage discount factor
   * - Stage duration
   * - Objective scaling
   *
   * Formula: 1 / (probability * discount * duration / scale_objective)
   *
   * @return Matrix of inverse cost factors by scenario/stage
   */
  [[nodiscard]] scenario_stage_factor_matrix_t scenario_stage_icost_factors()
      const;

private:
  std::reference_wrapper<const OptionsLP> m_options_;
  std::reference_wrapper<const std::vector<ScenarioLP>> m_scenarios_;
  std::reference_wrapper<const std::vector<StageLP>> m_stages_;
  std::vector<double> m_stage_discount_factors_;
};

}  // namespace gtopt
