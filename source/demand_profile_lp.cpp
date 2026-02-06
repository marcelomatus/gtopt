/**
 * @file      demand_profile_lp.cpp
 * @brief     Header of
 * @date      Sat Apr  5 23:12:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

DemandProfileLP::DemandProfileLP(DemandProfile pdemand_profile,
                                 InputContext& ic)
    : ProfileObjectLP(std::move(pdemand_profile), ic, ClassName)
{
}

bool DemandProfileLP::add_to_lp(const SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp)
{
  auto&& demand = sc.element<DemandLP>(demand_sid());
  if (!demand.is_active(stage)) {
    return true;
  }

  auto&& load_cols = demand.load_cols_at(scenario, stage);

  const auto [stage_capacity, capacity_col] =
      demand.capacity_and_col(stage, lp);

  if (!capacity_col && !demand.demand().capacity) {
    SPDLOG_WARN(
        "DemandProfile requires that Demand defines capacity or expansion");
    return false;
  }

  return add_profile_to_lp(ClassName.short_name(),
                           sc,
                           scenario,
                           stage,
                           lp,
                           "usv",
                           load_cols,
                           capacity_col,
                           stage_capacity);
}

bool DemandProfileLP::add_to_output(OutputContext& out) const
{
  return add_profile_to_output(ClassName.full_name(), out, "unserved");
}

}  // namespace gtopt
