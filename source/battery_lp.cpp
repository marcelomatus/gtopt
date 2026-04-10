/**
 * @file      battery_lp.cpp
 * @brief     Implementation of BatteryLP class for battery LP formulation
 * @date      Wed Apr  2 02:19:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the BatteryLP class, which handles the representation
 * of battery energy storage systems in linear programming problems. It includes
 * methods to create variables for energy flows, state of charge tracking, and
 * capacity constraints.
 */

#include <gtopt/battery_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>

namespace gtopt
{

BatteryLP::BatteryLP(const Battery& pbattery, const InputContext& ic)
    : StorageBase(pbattery, ic, ClassName)
    , input_efficiency(
          ic, ClassName, id(), std::move(object().input_efficiency))
    , output_efficiency(
          ic, ClassName, id(), std::move(object().output_efficiency))
{
}

/**
 * @brief Adds battery variables and constraints to the linear problem
 * @param sc System context containing current state
 * @param lp Linear problem to add variables and constraints to
 * @return True if successful, false otherwise
 *
 * This method creates:
 * 1. Flow variables for each time block (can be positive for discharge or
 * negative for charge)
 * 2. State of charge tracking constraints between time blocks
 * 3. Capacity constraints linking battery operation to installed capacity
 */

bool BatteryLP::add_to_lp(SystemContext& sc,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.full_name();
  static const auto ampl_name = std::string {ClassName.snake_case()};
  static constexpr double flow_conversion_rate = 1.0;

  // Add capacity-related variables and constraints
  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  // F9: register filter metadata for sum(...) predicates.  Battery `bus`
  // is optional (standalone batteries are decomposed by
  // `System::expand_batteries()`), so we only register `type`.
  if (const auto& t = battery().type) {
    AmplElementMetadata metadata;
    metadata.emplace_back("type", *t);
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  // Get capacity information
  auto&& [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  const auto stage_input_efficiency =
      input_efficiency.optval(stage.uid()).value_or(1.0);
  const auto stage_output_efficiency =
      output_efficiency.optval(stage.uid()).value_or(1.0);

  // Get blocks for this stage
  const auto& blocks = stage.blocks();

  // Resolve energy_scale from VariableScaleMap (default 1.0 if not set).
  // add_col auto-resolves scale from metadata when class_name is set.
  const auto es =
      sc.options().variable_scale_map().lookup("Battery", "energy", uid());

  // Create finp/fout variables for each time block.
  // Flow scale is resolved by add_col from VariableScaleMap metadata.
  BIndexHolder<ColIndex> finps;
  BIndexHolder<ColIndex> fouts;
  map_reserve(finps, blocks.size());
  map_reserve(fouts, blocks.size());

  double fs = 1.0;
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    finps[buid] = lp.add_col(SparseCol {
        .class_name = ClassName.full_name(),
        .variable_name = FinpName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fouts[buid] = lp.add_col(SparseCol {
        .class_name = ClassName.full_name(),
        .variable_name = FoutName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fs = lp.get_col_scale(finps[buid]);
  }
  const StorageOptions opts {
      .use_state_variable = battery().use_state_variable.value_or(false),
      .daily_cycle = battery().daily_cycle.value_or(true),
      .class_name = ClassName.full_name(),
      .variable_uid = uid(),
      .energy_scale = es,
      .flow_scale = fs,
  };
  if (!StorageBase::add_to_lp(cname,
                              ampl_name,
                              sc,
                              scenario,
                              stage,
                              lp,
                              flow_conversion_rate,
                              finps,
                              stage_input_efficiency,
                              fouts,
                              stage_output_efficiency,
                              stage_capacity,
                              capacity_col,
                              {},
                              {},
                              opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for battery {}", uid());

    return false;
  }

  // Store finp variable indices for later use
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  finp_cols[st_key] = std::move(finps);
  fout_cols[st_key] = std::move(fouts);

  // Register battery-specific PAMPL columns.  Storage-generic variables
  // (energy/drain/eini/efin/soft_emin) are registered centrally by
  // StorageBase::add_to_lp; `capainst` by CapacityBase::add_to_lp.
  sc.add_ampl_variable(
      ampl_name, uid(), ChargeName, scenario, stage, finp_cols.at(st_key));
  sc.add_ampl_variable(
      ampl_name, uid(), DischargeName, scenario, stage, fout_cols.at(st_key));

  return true;
}

/**
 * @brief Adds battery output results to the output context
 * @param out Output context to add results to
 * @return True if successful, false otherwise
 *
 * Processes planning results for:
 * - Finp variables (charge/discharge decisions)
 * - Storage levels
 * - Capacity-related outputs
 */
bool BatteryLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  // Add finp variable solutions and costs to output
  out.add_col_sol(cname, FinpName, id(), finp_cols);
  out.add_col_cost(cname, FinpName, id(), finp_cols);

  // Add fout variable solutions and costs to output
  out.add_col_sol(cname, FoutName, id(), fout_cols);
  out.add_col_cost(cname, FoutName, id(), fout_cols);

  // Process storage and capacity outputs
  return StorageBase::add_to_output(out, cname)
      && CapacityBase::add_to_output(out);
}

}  // namespace gtopt
