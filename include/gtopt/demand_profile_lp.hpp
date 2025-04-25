/**
 * @file      demand_profile_lp.hpp
 * @brief     Header of
 * @date      Tue Apr  1 23:54:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile.hpp>

namespace gtopt
{
class DemandProfileLP : public ObjectLP<DemandProfile>
{
public:
  constexpr static std::string_view ClassName = "DemandProfile";

  explicit DemandProfileLP(InputContext& ic, DemandProfile pdemand_profile);

  [[nodiscard]] constexpr auto&& demand_profile() { return object(); }
  [[nodiscard]] constexpr auto&& demand_profile() const { return object(); }

  [[nodiscard]] auto&& demand() const { return demand_profile().demand; }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioIndex& scenario_index,
                               const StageIndex& stage_index,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched scost;
  STBRealSched profile;

  STBIndexHolder spillover_cols;
  STBIndexHolder spillover_rows;

  ElementIndex<DemandLP> demand_index;
};

}  // namespace gtopt
