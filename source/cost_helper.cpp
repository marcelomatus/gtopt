/**
 * @file      cost_helper.cpp
 * @brief     Implementation of cost factor computation helpers
 * @date      Sun Jun 22 15:57:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements CostHelper methods that compute block-level
 * inverse cost factors and discount factors for the LP objective.
 * Results are lazily cached on first access.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/utils.hpp>

namespace
{

using namespace gtopt;

auto compute_block_icost_factors(const CostHelper& helper,
                                 const std::vector<ScenarioLP>& scenarios,
                                 const std::vector<StageLP>& stages)
    -> block_factor_matrix_t
{
  const auto active_scenarios =
      std::ranges::to<std::vector>(enumerate_active<Index>(scenarios));
  const auto active_stages =
      std::ranges::to<std::vector>(enumerate_active<Index>(stages));

  block_factor_matrix_t factors(active_scenarios.size(), active_stages.size());

  for (auto&& [si, scenario] : active_scenarios) {
    for (auto&& [ti, stage] : active_stages) {
      factors[si][ti] = to_vector(
          stage.blocks(),
          [&](const auto& block)
          { return 1.0 / helper.cost_factor(scenario, stage, block); });
    }
  }

  return factors;
}

auto compute_block_discount_icost_factors(
    double scale_obj,
    const std::vector<ScenarioLP>& scenarios,
    const std::vector<StageLP>& stages) -> block_factor_matrix_t
{
  const auto active_scenarios =
      std::ranges::to<std::vector>(enumerate_active<Index>(scenarios));
  const auto active_stages =
      std::ranges::to<std::vector>(enumerate_active<Index>(stages));

  block_factor_matrix_t factors(active_scenarios.size(), active_stages.size());

  for (auto&& [si, scenario] : active_scenarios) {
    for (auto&& [ti, stage] : active_stages) {
      const double factor = scale_obj / stage.discount_factor();
      factors[si][ti] =
          to_vector(stage.blocks(), [factor](const auto&) { return factor; });
    }
  }

  return factors;
}

auto compute_stage_icost_factors(const CostHelper& helper,
                                 const std::vector<StageLP>& stages)
    -> stage_factor_matrix_t
{
  auto&& active_stages = enumerate_active<Index>(stages);
  return std::ranges::to<std::vector>(active_stages
                                      | std::views::transform(
                                          [&](const auto& is)
                                          {
                                            const auto& [i, s] = is;
                                            return 1.0 / helper.cost_factor(s);
                                          }));
}

auto compute_scenario_stage_icost_factors(
    const CostHelper& helper,
    const std::vector<ScenarioLP>& scenarios,
    const std::vector<StageLP>& stages) -> scenario_stage_factor_matrix_t
{
  const auto active_scenarios =
      std::ranges::to<std::vector>(enumerate_active<Index>(scenarios));
  const auto active_stages =
      std::ranges::to<std::vector>(enumerate_active<Index>(stages));

  scenario_stage_factor_matrix_t factors(active_scenarios.size(),
                                         active_stages.size());

  for (auto&& [si, scenario] : active_scenarios) {
    for (auto&& [ti, stage] : active_stages) {
      factors[si][ti] = 1.0 / helper.cost_factor(scenario, stage);
    }
  }

  return factors;
}

}  // namespace

namespace gtopt
{

auto CostHelper::block_icost_factors() const -> const block_factor_matrix_t&
{
  if (!m_block_icost_cache_) {
    m_block_icost_cache_ =
        compute_block_icost_factors(*this, m_scenarios_.get(), m_stages_.get());
  }
  return *m_block_icost_cache_;
}

auto CostHelper::block_discount_icost_factors() const
    -> const block_factor_matrix_t&
{
  if (!m_block_discount_icost_cache_) {
    m_block_discount_icost_cache_ =
        compute_block_discount_icost_factors(m_options_.get().scale_objective(),
                                             m_scenarios_.get(),
                                             m_stages_.get());
  }
  return *m_block_discount_icost_cache_;
}

auto CostHelper::stage_icost_factors() const -> const stage_factor_matrix_t&
{
  if (!m_stage_icost_cache_) {
    m_stage_icost_cache_ = compute_stage_icost_factors(*this, m_stages_.get());
  }
  return *m_stage_icost_cache_;
}

auto CostHelper::scenario_stage_icost_factors() const
    -> const scenario_stage_factor_matrix_t&
{
  if (!m_ss_icost_cache_) {
    m_ss_icost_cache_ = compute_scenario_stage_icost_factors(
        *this, m_scenarios_.get(), m_stages_.get());
  }
  return *m_ss_icost_cache_;
}

}  // namespace gtopt
