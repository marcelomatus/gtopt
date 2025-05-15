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
  std::vector<double> factors;
  factors.reserve(stages.size());

  for (auto&& st : stages) {
    factors.push_back(st.discount_factor());
  }

  return factors;
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
