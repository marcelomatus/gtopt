/**
 * @file      cost_helper.hpp
 * @brief     Cost calculation and discounting utilities for optimization problems
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

  [[nodiscard]] double block_cost(const ScenarioIndex& scenario_index,
                                  const StageIndex& stage_index,
                                  const BlockLP& block,
                                  double cost) const;

  [[nodiscard]] block_factor_matrix_t block_cost_factors() const;

  [[nodiscard]] double stage_cost(const StageIndex& stage_index,
                                  double cost,
                                  double probability_factor = 1.0) const;

  [[nodiscard]] stage_factor_matrix_t stage_cost_factors() const;

  [[nodiscard]] double scenario_stage_cost(const ScenarioIndex& scenario_index,
                                           const StageIndex& stage_index,
                                           double cost) const;

  [[nodiscard]] scenario_stage_factor_matrix_t scenario_stage_cost_factors()
      const;

private:
  std::reference_wrapper<const OptionsLP> m_options_;
  std::reference_wrapper<const std::vector<ScenarioLP>> m_scenarios_;
  std::reference_wrapper<const std::vector<StageLP>> m_stages_;
  std::vector<double> m_stage_discount_factors_;
};

}  // namespace gtopt
