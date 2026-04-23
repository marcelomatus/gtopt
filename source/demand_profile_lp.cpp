/**
 * @file      demand_profile_lp.cpp
 * @brief     Implementation of demand profile LP formulation
 * @date      Sat Apr  5 23:12:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements DemandProfileLP construction and add_to_lp,
 * which applies time-varying profile factors to demand load variables.
 */

#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

DemandProfileLP::DemandProfileLP(const DemandProfile& pdemand_profile,
                                 InputContext& ic)
    : ProfileObjectLP(pdemand_profile, ic, ClassName)
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

  const auto [opt_capacity, capacity_col] = demand.capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  if (!capacity_col && !demand.demand().capacity) {
    SPDLOG_WARN(
        "DemandProfile requires that Demand defines capacity or expansion");
    return false;
  }

  return add_profile_to_lp(ClassName.full_name(),
                           scenario,
                           stage,
                           lp,
                           UnservedName,
                           load_cols,
                           capacity_col,
                           stage_capacity);
}

bool DemandProfileLP::add_to_output(OutputContext& out) const
{
  return add_profile_to_output(ClassName.full_name(), out, UnservedName);
}

bool DemandProfileLP::update_aperture(
    LinearInterface& li,
    const ScenarioLP& base_scenario,
    const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
    const StageLP& stage) const
{
  return ProfileObjectLP::update_aperture(li, base_scenario, value_fn, stage);
}

}  // namespace gtopt
