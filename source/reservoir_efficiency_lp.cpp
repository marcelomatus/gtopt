/**
 * @file      reservoir_efficiency_lp.cpp
 * @brief     Implementation of ReservoirEfficiencyLP
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP registration and coefficient tracking for
 * reservoir-dependent turbine efficiency.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_efficiency_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool ReservoirEfficiencyLP::add_to_lp(const SystemContext& sc,
                                      const ScenarioLP& scenario,
                                      const StageLP& stage,
                                      [[maybe_unused]] LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Locate the turbine and its stored conversion rows and flow columns
  const auto& turbine = sc.element<TurbineLP>(turbine_sid());
  const auto& waterway =
      sc.element<WaterwayLP>(WaterwayLPSId {turbine.turbine().waterway});

  const auto& conv_rows = turbine.conversion_rows_at(scenario, stage);
  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);

  const auto& blocks = stage.blocks();

  BCoeffMap bmap;
  map_reserve(bmap, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    if (conv_rows.contains(buid) && flow_cols.contains(buid)) {
      bmap[buid] = CoeffIndex {
          .row = conv_rows.at(buid),
          .col = flow_cols.at(buid),
      };
    }
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  m_coeff_indices_[st_key] = std::move(bmap);

  return true;
}

bool ReservoirEfficiencyLP::add_to_output(
    [[maybe_unused]] OutputContext& out) const
{
  // No direct output for efficiency elements — the updated conversion
  // rate is reflected in the turbine's conversion dual output.
  return true;
}

}  // namespace gtopt
