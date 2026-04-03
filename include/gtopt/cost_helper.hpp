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

#include <optional>

#include <gtopt/index_holder.hpp>
#include <gtopt/planning_options_lp.hpp>

namespace gtopt::detail
{

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
  explicit CostHelper(const PlanningOptionsLP& options,
                      const std::vector<ScenarioLP>& scenarios,
                      const std::vector<StageLP>& stages)
      : m_options_(options)
      , m_scenarios_(scenarios)
      , m_stages_(stages)
  {
  }

  [[nodiscard]] constexpr double cost_factor(
      const double probability,
      const double discount,
      const double duration = 1.0) const noexcept
  {
    const auto scale_obj = m_options_.get().scale_objective();
    return probability * discount * duration / scale_obj;
  }

  [[nodiscard]] constexpr double cost_factor(
      const ScenarioLP& scenario, const StageLP& stage) const noexcept
  {
    return cost_factor(scenario.probability_factor(),
                       stage.discount_factor(),
                       stage.duration());
  }

  [[nodiscard]] constexpr double cost_factor(
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BlockLP& block) const noexcept
  {
    return cost_factor(scenario.probability_factor(),
                       stage.discount_factor(),
                       block.duration());
  }

  [[nodiscard]] constexpr double cost_factor(
      const StageLP& stage, double probability = 1.0) const noexcept
  {
    return cost_factor(probability, stage.discount_factor(), stage.duration());
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
   * @return Total energy cost coefficient for LP formulation
   */
  [[nodiscard]] constexpr double block_ecost(const ScenarioLP& scenario,
                                             const StageLP& stage,
                                             const BlockLP& block,
                                             const double cost) const noexcept
  {
    return cost * cost_factor(scenario, stage, block);
  }

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
   * @return Total energy cost coefficient for LP formulation
   */
  [[nodiscard]] constexpr double scenario_stage_ecost(
      const ScenarioLP& scenario,
      const StageLP& stage,
      const double cost) const noexcept
  {
    return cost * cost_factor(scenario, stage);
  }

  /**
   * @brief Calculates the energy cost coefficient for a stage
   *
   * Computes the total energy cost for a power variable over a stage
   * duration, applying:
   * - Probability weighting (default 1.0)
   * - Stage discount factor
   * - Stage duration
   * - Objective scaling
   *
   * Formula: cost * probability * discount * duration / scale_objective
   * @return Total energy cost coefficient for LP formulation
   */

  [[nodiscard]] constexpr double stage_ecost(
      const StageLP& stage,
      double cost,
      double probability = 1.0) const noexcept
  {
    return cost * cost_factor(stage, probability);
  }

  /**
   * @brief Returns cached inverse cost factors for blocks (1/cost_factor).
   *
   * Lazily computed on first call, then cached for subsequent accesses.
   * Formula: 1 / (probability * discount * duration / scale_objective)
   *
   * @return Const reference to cached matrix of inverse cost factors
   */
  [[nodiscard]] const block_factor_matrix_t& block_icost_factors() const;

  /**
   * @brief Returns cached discount-only inverse cost factors for blocks.
   *
   * Lazily computed on first call, then cached.
   * Formula: scale_objective / discount[t]   (same for all s, b in stage t)
   *
   * @return Const reference to cached matrix of discount-only inverse cost
   * factors
   */
  [[nodiscard]] const block_factor_matrix_t& block_discount_icost_factors()
      const;

  /**
   * @brief Returns cached inverse cost factors for stages (1/cost_factor).
   *
   * Lazily computed on first call (probability = 1.0), then cached.
   * Formula: 1 / (discount * duration / scale_objective)
   *
   * @return Const reference to cached vector of inverse cost factors
   */
  [[nodiscard]] const stage_factor_matrix_t& stage_icost_factors() const;

  /**
   * @brief Returns cached inverse cost factors for scenario-stage pairs.
   *
   * Lazily computed on first call, then cached.
   * Formula: 1 / (probability * discount * duration / scale_objective)
   *
   * @return Const reference to cached matrix of inverse cost factors
   */
  [[nodiscard]] const scenario_stage_factor_matrix_t&
  scenario_stage_icost_factors() const;

private:
  std::reference_wrapper<const PlanningOptionsLP> m_options_;
  std::reference_wrapper<const std::vector<ScenarioLP>> m_scenarios_;
  std::reference_wrapper<const std::vector<StageLP>> m_stages_;

  mutable std::optional<block_factor_matrix_t> m_block_icost_cache_;
  mutable std::optional<block_factor_matrix_t> m_block_discount_icost_cache_;
  mutable std::optional<stage_factor_matrix_t> m_stage_icost_cache_;
  mutable std::optional<scenario_stage_factor_matrix_t> m_ss_icost_cache_;
};

}  // namespace gtopt
