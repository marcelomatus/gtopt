/**
 * @file      cost_helper.cpp
 * @brief     Header of
 * @date      Sun Jun 22 15:57:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/utils.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

auto CostHelper::block_icost_factors() const -> block_factor_matrix_t
{
  auto&& active_scenarios = enumerate_active<Index>(m_scenarios_.get());
  auto&& active_stages = enumerate_active<Index>(m_stages_.get());

  const auto n_scenarios = ranges::distance(active_scenarios);
  const auto n_stages = ranges::distance(active_stages);
  block_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  for (auto&& [si, scenario] : active_scenarios) {
    for (auto&& [ti, stage] : active_stages) {
      factors[si][ti] =
          to_vector(stage.blocks(),
                    [&](const auto& block)
                    { return 1.0 / cost_factor(scenario, stage, block); });
    }
  }

  return factors;
}

auto CostHelper::stage_icost_factors(double probability) const
    -> stage_factor_matrix_t
{
  auto&& active_stages = enumerate_active<Index>(m_stages_.get());

  return to_vector(active_stages,
                   [&](const auto& is)
                   { return 1.0 / cost_factor(is.second, probability); });
}

auto CostHelper::scenario_stage_icost_factors() const
    -> scenario_stage_factor_matrix_t
{
  auto&& active_scenarios = enumerate_active<Index>(m_scenarios_.get());
  auto&& active_stages = enumerate_active<Index>(m_stages_.get());

  const auto n_scenarios = ranges::distance(active_scenarios);
  const auto n_stages = ranges::distance(active_stages);
  scenario_stage_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  for (auto&& [si, scenario] : active_scenarios) {
    for (auto&& [ti, stage] : active_stages) {
      const auto cfactor = cost_factor(scenario, stage);
      factors[si][ti] = 1.0 / cfactor;
    }
  }

  return factors;
}

}  // namespace gtopt
