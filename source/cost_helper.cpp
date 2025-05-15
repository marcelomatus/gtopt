#include <gtopt/cost_helper.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

auto CostHelper::block_icost_factors() const -> block_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(m_scenarios_.get().size());
  const auto n_stages = static_cast<Index>(m_stages_.get().size());
  block_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  for (auto&& [si, scenario] : enumerate_active<Index>(m_scenarios_.get())) {
    for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
      factors[si][ti].resize(stage.blocks().size());
      for (auto&& [bi, block] : enumerate<Index>(stage.blocks())) {
        const auto cfactor = cost_factor(scenario, stage, block);
        factors[si][ti][bi] = 1.0 / cfactor;
      }
    }
  }

  return factors;
}

auto CostHelper::stage_icost_factors(double probability) const
    -> stage_factor_matrix_t
{
  stage_factor_matrix_t factors(m_stages_.get().size());

  for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
    const auto cfactor = cost_factor(stage, probability);
    factors[ti] = 1.0 / cfactor;
  }

  return factors;
}

auto CostHelper::scenario_stage_icost_factors() const
    -> scenario_stage_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(m_scenarios_.get().size());
  const auto n_stages = static_cast<Index>(m_stages_.get().size());
  scenario_stage_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  for (auto&& [si, scenario] : enumerate_active<Index>(m_scenarios_.get())) {
    for (auto&& [ti, stage] : enumerate_active<Index>(m_stages_.get())) {
      const auto cfactor = cost_factor(scenario, stage);
      factors[si][ti] = 1.0 / cfactor;
    }
  }

  return factors;
}

}  // namespace gtopt
