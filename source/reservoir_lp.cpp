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
    , scost(ic, ClassName, id(), std::move(reservoir().scost))
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

  // Resolve energy_scale with priority:
  //   1. Explicit per-element field (immune to VariableScaleMap)
  //   2. VariableScaleMap entry (resolved by add_col when class_name is set)
  //   3. Auto-scale from capacity
  //   4. Default (1.0)
  //
  // When the per-element field is set, energy_class_name is left empty so
  // that add_col does NOT consult the VariableScaleMap.  Otherwise,
  // class_name metadata is set and add_col overrides auto_scale if the map
  // has an entry (returns != 1.0); if the map returns 1.0, auto_scale wins.
  const bool has_explicit_energy_scale = reservoir().energy_scale.has_value();
  const double energy_scale = [&]
  {
    if (has_explicit_energy_scale) {
      return *reservoir().energy_scale;
    }
    // Auto-scale mode: round down capacity to previous power of 10.
    // cap=72 → 10;  cap=5586 → 1000.
    // This is the fallback — VariableScaleMap entries override it in add_col.
    if (reservoir().energy_scale_mode_enum() == EnergyScaleMode::auto_scale) {
      if (stage_capacity <= 1.0) {
        return 1.0;
      }
      return std::pow(10.0, std::floor(std::log10(stage_capacity)));
    }
    return Reservoir::default_energy_scale;
  }();
  // Only opt into VariableScaleMap resolution when no per-element field.
  const auto energy_class_name =
      has_explicit_energy_scale ? std::string_view {} : ClassName.full_name();
  BIndexHolder<ColIndex> rcols;
  BIndexHolder<ColIndex> scols;
  map_reserve(rcols, blocks.size());
  map_reserve(scols, blocks.size());

  // flow_scale is resolved by add_col from the VariableScaleMap metadata.
  // We read back the resolved value from the first column for StorageOptions.
  double flow_scale = 1.0;

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // rsv_fext: physical flow bounds [m³/s].
    // add_col auto-resolves flow_scale from VariableScaleMap metadata.
    const auto rc = lp.add_col(SparseCol {
        .lowb = fmin,
        .uppb = fmax,
        .class_name = ClassName.full_name(),
        .variable_name = ExtractionName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });

    rcols[buid] = rc;
    flow_scale = lp.get_col_scale(rc);

    // The extraction adds flow to the junction balance (in m³/s).
    // Physical coefficient is 1.0; flatten() multiplies by col_scale.
    auto& brow = lp.row_at(balance_rows.at(buid));
    brow[rc] = 1.0;
  }

  const auto mpf = reservoir().mean_production_factor.value_or(
      Reservoir::default_mean_production_factor);
  const auto stage_scost = sc.state_fail_cost(stage, scost);
  const double rsv_scost = stage_scost.value_or(1.0) * mpf;

  const StorageOptions opts {
      .use_state_variable = reservoir().use_state_variable.value_or(true),
      .daily_cycle = reservoir().daily_cycle.value_or(false),
      .class_name = energy_class_name,
      .variable_uid = uid(),
      .energy_scale = energy_scale,
      .flow_scale = flow_scale,
      .scost = rsv_scost,
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
  out.add_col_sol(cname, ExtractionName, id(), extraction_cols);
  out.add_col_cost(cname, ExtractionName, id(), extraction_cols);

  return StorageBase::add_to_output(out, ClassName.full_name());
}

}  // namespace gtopt
