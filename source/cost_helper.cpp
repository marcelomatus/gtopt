#include <gtopt/cost_helper.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{
namespace
{

/**
 * @brief Helper function to compute final cost factor
 *
 * Calculates: (probability * discount * duration) / objective_scale
 *
 * @param scale_obj Objective scaling factor
 * @param probability Scenario probability factor
 * @param discount Stage discount factor
 * @param duration Time duration
 * @return Combined cost factor
 */
constexpr auto cost_factor(const auto scale_obj,
                           const auto probability,
                           const auto discount,
                           const auto duration) noexcept
{
  return probability * discount * duration / scale_obj;
}

}  // namespace

double CostHelper::block_ecost(const ScenarioIndex& scenario_index,
                              const StageIndex& stage_index,
                              const BlockLP& block,
                              double cost) const
{
  return cost
      * cost_factor(m_options_.get().scale_objective(),
                    m_scenarios_.get()[scenario_index].probability_factor(),
                    m_stage_discount_factors_[stage_index],
                    block.duration());
}

auto CostHelper::block_ecost_factors() const -> block_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(m_scenarios_.get().size());
  const auto n_stages = static_cast<Index>(m_stages_.get().size());
  block_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  const auto scale_obj = m_options_.get().scale_objective();

  for (auto&& [si, scenario] : enumerate_active<Index>(m_scenarios_.get())) {
    const auto probability_factor = scenario.probability_factor();
    for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
      factors[si][ti].resize(stage.blocks().size());
      for (auto&& [bi, block] : enumerate<Index>(stage.blocks())) {
        const auto cfactor = cost_factor(scale_obj,
                                         probability_factor,
                                         m_stage_discount_factors_[ti],
                                         block.duration());
        factors[si][ti][bi] = 1.0 / cfactor;
      }
    }
  }

  return factors;
}

double CostHelper::stage_ecost(const StageIndex& stage_index,
                              const double cost,
                              const double probability_factor) const
{
  if (m_stage_discount_factors_.empty()) {
    return cost;
  }

  return cost
      * cost_factor(m_options_.get().scale_objective(),
                    probability_factor,
                    m_stage_discount_factors_[stage_index],
                    m_stages_.get()[stage_index].duration());
}

auto CostHelper::stage_ecost_factors() const -> stage_factor_matrix_t
{
  stage_factor_matrix_t factors(m_stages_.get().size());

  const auto scale_obj = m_options_.get().scale_objective();
  const auto probability_factor = 1.0;
  for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
    const auto cfactor = cost_factor(scale_obj,
                                     probability_factor,
                                     m_stage_discount_factors_[ti],
                                     stage.duration());
    factors[ti] = 1.0 / cfactor;
  }

  return factors;
}

double CostHelper::scenario_stage_ecost(const ScenarioIndex& scenario_index,
                                       const StageIndex& stage_index,
                                       double cost) const
{
  if (m_scenarios_.get().empty()) {
    return cost;
  }

  return stage_cost(stage_index,
                    cost,
                    m_scenarios_.get()[scenario_index].probability_factor());
}

auto CostHelper::scenario_stage_ecost_factors() const
    -> scenario_stage_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(m_scenarios_.get().size());
  const auto n_stages = static_cast<Index>(m_stages_.get().size());
  scenario_stage_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  const auto scale_obj = m_options_.get().scale_objective();

  for (auto&& [si, scenario] : enumerate_active<Index>(m_scenarios_.get())) {
    const auto probability_factor = scenario.probability_factor();
    for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
      const auto cfactor = cost_factor(scale_obj,
                                       probability_factor,
                                       m_stage_discount_factors_[ti],
                                       stage.duration());
      factors[si][ti] = 1.0 / cfactor;
    }
  }

  return factors;
}

}  // namespace gtopt
