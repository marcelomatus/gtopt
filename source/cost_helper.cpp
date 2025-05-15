#include <gtopt/cost_helper.hpp>
#include <gtopt/utils.hpp>

namespace gtopt {

namespace {

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
  std::vector<double> factors(stages.size(), 1.0);

  double discount_factor = 1.0;
  for (auto&& [ti, st] : enumerate_active(stages)) {
    factors[ti] = discount_factor;
    discount_factor *= st.discount_factor();
  }

  return factors;
}

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

} // namespace

CostHelper::CostHelper(const OptionsLP& options,
                      const std::vector<ScenarioLP>& scenarios,
                      const std::vector<StageLP>& stages)
    : options_(options)
    , scenarios_(scenarios)
    , stages_(stages)
    , stage_discount_factors_(stage_factors(stages))
{
}

double CostHelper::block_cost(const ScenarioIndex& scenario_index,
                             const StageIndex& stage_index,
                             const BlockLP& block,
                             double cost) const
{
    return cost * cost_factor(options_.scale_objective(),
                            scenarios_[scenario_index].probability_factor(),
                            stage_discount_factors_[stage_index],
                            block.duration());
}

auto CostHelper::block_cost_factors() const -> block_factor_matrix_t
{
    const auto n_scenarios = static_cast<Index>(scenarios_.size());
    const auto n_stages = static_cast<Index>(stages_.size());
    block_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

    const auto scale_obj = options_.scale_objective();

    for (auto&& [si, scenario] : enumerate_active<Index>(scenarios_)) {
        const auto probability_factor = scenario.probability_factor();
        for (auto&& [ti, stage] : enumerate_active<Index>(stages_)) {
            factors[si][ti].resize(stage.blocks().size());
            for (auto&& [bi, block] : enumerate<Index>(stage.blocks())) {
                const auto cfactor = cost_factor(scale_obj,
                                               probability_factor,
                                               stage_discount_factors_[ti],
                                               block.duration());
                factors[si][ti][bi] = 1.0 / cfactor;
            }
        }
    }

    return factors;
}

double CostHelper::stage_cost(const StageIndex& stage_index,
                            double cost) const
{
    const auto probability_factor = 1.0;
    return cost * cost_factor(options_.scale_objective(),
                            probability_factor,
                            stage_discount_factors_[stage_index],
                            stages_[stage_index].duration());
}

auto CostHelper::stage_cost_factors() const -> stage_factor_matrix_t
{
    stage_factor_matrix_t factors(stages_.size());

    const auto scale_obj = options_.scale_objective();
    const auto probability_factor = 1.0;
    for (auto&& [ti, stage] : enumerate_active<Index>(stages_)) {
        const auto cfactor = cost_factor(scale_obj,
                                       probability_factor,
                                       stage_discount_factors_[ti],
                                       stage.duration());
        factors[ti] = 1.0 / cfactor;
    }

    return factors;
}

double CostHelper::scenario_stage_cost(const ScenarioIndex& scenario_index,
                                     const StageIndex& stage_index,
                                     double cost) const
{
    return cost * cost_factor(options_.scale_objective(),
                            scenarios_[scenario_index].probability_factor(),
                            stage_discount_factors_[stage_index],
                            stages_[stage_index].duration());
}

auto CostHelper::scenario_stage_cost_factors() const 
    -> scenario_stage_factor_matrix_t
{
    const auto n_scenarios = static_cast<Index>(scenarios_.size());
    const auto n_stages = static_cast<Index>(stages_.size());
    scenario_stage_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

    const auto scale_obj = options_.scale_objective();

    for (auto&& [si, scenario] : enumerate_active<Index>(scenarios_)) {
        const auto probability_factor = scenario.probability_factor();
        for (auto&& [ti, stage] : enumerate_active<Index>(stages_)) {
            const auto cfactor = cost_factor(scale_obj,
                                           probability_factor,
                                           stage_discount_factors_[ti],
                                           stage.duration());
            factors[si][ti] = 1.0 / cfactor;
        }
    }

    return factors;
}

} // namespace gtopt
