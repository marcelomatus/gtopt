#pragma once

#include <boost/multi_array.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt {
namespace detail {

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

} // namespace detail
} // namespace gtopt

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
  using block_factor_matrix_t = boost::multi_array<std::vector<double>, 2>;
  using stage_factor_matrix_t = std::vector<double>;
  using scenario_stage_factor_matrix_t = boost::multi_array<double, 2>;

  explicit CostHelper(const OptionsLP& options,
                     const std::vector<ScenarioLP>& scenarios,
                     const std::vector<StageLP>& stages)
    : m_options_(options)
    , m_scenarios_(scenarios)
    , m_stages_(stages)
    , m_stage_discount_factors_(detail::stage_factors(stages))
  {}

  [[nodiscard]] double block_cost(const ScenarioIndex& scenario_index,
                                  const StageIndex& stage_index,
                                  const BlockLP& block,
                                  double cost) const;

  [[nodiscard]] block_factor_matrix_t block_cost_factors() const;

  [[nodiscard]] double stage_cost(const StageIndex& stage_index,
                                  double cost) const;

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
