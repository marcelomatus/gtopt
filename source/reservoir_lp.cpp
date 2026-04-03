/**
 * @file      reservoir_lp.cpp
 * @brief     Implementation of ReservoirLP class
 * @date      Wed Jul 30 23:22:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the ReservoirLP class which provides the linear
 * programming representation of water reservoirs, including their storage
 * constraints and relationships with other system components.
 */

#include <cmath>
#include <limits>

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirLP::ReservoirLP(const Reservoir& preservoir, const InputContext& ic)
    : StorageBase(preservoir, ic, ClassName)
    , capacity(ic, ClassName, id(), std::move(reservoir().capacity))
{
}

/**
 * @brief Adds reservoir constraints to the linear problem
 *
 * @param sc System context containing component relationships
 * @param scenario Current scenario being processed
 * @param stage Current stage being processed
 * @param lp Linear problem to add constraints to
 * @return true if successful, false on error
 *
 * Adds constraints for:
 * - Water extraction from reservoir
 * - Storage capacity limits
 * - Connection to junction balance equations
 */
bool ReservoirLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto stage_capacity =
      capacity.at(stage.uid()).value_or(LinearProblem::DblMax);

  const auto& junction = sc.element<JunctionLP>(junction_sid());
  if (!junction.is_active(stage)) {
    return true;
  }

  auto&& balance_rows = junction.balance_rows_at(scenario, stage);
  auto&& blocks = stage.blocks();

  const auto fmin = reservoir().fmin.value_or(-LinearProblem::DblMax);
  const auto fmax = reservoir().fmax.value_or(+LinearProblem::DblMax);

  // Resolve energy_scale: explicit field > auto-scale > VariableScaleMap.
  const double energy_scale = [&]
  {
    // 1. Explicit per-element field always wins.
    if (reservoir().energy_scale.has_value()) {
      return *reservoir().energy_scale;
    }
    // 2. VariableScaleMap override (from PlanningOptions::variable_scales).
    const auto vs =
        sc.options().variable_scale_map().lookup("Reservoir", "energy", uid());
    if (vs != 1.0) {
      return vs;
    }
    // 3. Auto-scale mode: round up capacity/1000 to next power of 10.
    //    cap=6000 → 6 → 10;  cap=6e6 → 6000 → 10000.
    if (reservoir().energy_scale_mode_enum() == EnergyScaleMode::auto_scale) {
      const auto raw = stage_capacity / 1000.0;
      if (raw <= 1.0) {
        return 1.0;
      }
      return std::pow(10.0, std::ceil(std::log10(raw)));
    }
    return Reservoir::default_energy_scale;
  }();

  // Resolve flow_scale independently from energy_scale.
  // Default 1.0: extraction flow fext stays in physical m³/s.
  // Setting flow_scale = energy_scale keeps energy-balance
  // coefficients O(1) but couples two different physical quantities.
  const double flow_scale = [&]
  {
    const auto fs =
        sc.options().variable_scale_map().lookup("Reservoir", "flow", uid());
    return (fs != 1.0) ? fs : 1.0;
  }();
  const double inv_flow_scale = 1.0 / flow_scale;

  BIndexHolder<ColIndex> rcols;
  BIndexHolder<ColIndex> scols;
  map_reserve(rcols, blocks.size());
  map_reserve(scols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // rsv_fext LP variable = physical_flow [m³/s] / flow_scale.
    // Bounds are scaled accordingly.
    const auto rc = lp.add_col(SparseCol {
        .name = sc.lp_col_label(scenario, stage, block, cname, "fext", uid()),
        .lowb = fmin * inv_flow_scale,
        .uppb = fmax * inv_flow_scale,
        .scale = flow_scale,
    });

    rcols[buid] = rc;

    // The extraction adds flow to the junction balance (in m³/s).
    // Since rsv_fext_LP = fext_m3s / flow_scale, restore the physical unit
    // by using flow_scale as the coefficient.
    auto& brow = lp.row_at(balance_rows.at(buid));
    brow[rc] = flow_scale;
  }

  const StorageOptions opts {
      .use_state_variable = reservoir().use_state_variable.value_or(true),
      .daily_cycle = reservoir().daily_cycle.value_or(false),
      .energy_scale = energy_scale,
      .flow_scale = flow_scale,
  };
  if (!StorageBase::add_to_lp(cname,
                              sc,
                              scenario,
                              stage,
                              lp,
                              flow_conversion_rate(),
                              rcols,
                              1.0,
                              rcols,
                              1.0,
                              stage_capacity,
                              std::nullopt,
                              spillway_cost(),
                              spillway_capacity(),
                              opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for reservoir {}",
                    uid());

    return false;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  extraction_cols[st_key] = std::move(rcols);
  return true;
}

/**
 * @brief Adds reservoir solution variables to the output context
 *
 * @param out Output context to write results to
 * @return true if successful, false on error
 *
 * Outputs the solution values for:
 * - Water extraction flows
 * - Storage variables
 * - Associated costs
 */
bool ReservoirLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  // Extraction columns have .scale = flow_scale; auto-descaled by
  // LinearInterface's get_col_sol() / get_col_cost().
  out.add_col_sol(cname, "extraction", id(), extraction_cols);
  out.add_col_cost(cname, "extraction", id(), extraction_cols);

  return StorageBase::add_to_output(out, ClassName.full_name());
}

}  // namespace gtopt
