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

  const auto stage_capacity = capacity.at(stage.uid()).value_or(CoinDblMax);

  const auto& junction = sc.element<JunctionLP>(junction_sid());
  if (!junction.is_active(stage)) {
    return true;
  }

  auto&& balance_rows = junction.balance_rows_at(scenario, stage);
  auto&& blocks = stage.blocks();

  const auto fmin = reservoir().fmin.value_or(-LinearProblem::DblMax);
  const auto fmax = reservoir().fmax.value_or(+LinearProblem::DblMax);

  // vol_scale is used both as the energy (volume) scale and as the flow
  // variable scale so that the energy-balance (rsv_vol) row coefficients
  // become flow_conversion_rate × duration — O(1) — instead of the tiny
  // values (~1e-5) that arise when vol_scale is large (e.g. 100 000).
  //
  // Trade-off: the junction-balance (jct_bal) coefficient for rsv_fext
  // becomes vol_scale (instead of 1). In practice a single large coefficient
  // in an otherwise ±1 matrix is handled well by LP solvers, whereas many
  // energy-balance rows with 1e-5 coefficients degrade convergence.
  const double vol_scale =
      reservoir().vol_scale.value_or(Reservoir::default_vol_scale);
  const double inv_vol_scale = 1.0 / vol_scale;

  BIndexHolder<ColIndex> rcols;
  BIndexHolder<ColIndex> scols;
  map_reserve(rcols, blocks.size());
  map_reserve(scols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // rsv_fext LP variable = physical_flow [m³/s] / vol_scale.
    // Bounds are scaled accordingly.
    const auto rc = lp.add_col(SparseCol {
        .name = sc.lp_label(scenario, stage, block, cname, "fext", uid()),
        .lowb = fmin * inv_vol_scale,
        .uppb = fmax * inv_vol_scale,
        .scale = vol_scale,
    });

    rcols[buid] = rc;

    // The extraction adds flow to the junction balance (in m³/s).
    // Since rsv_fext_LP = fext_m3s / vol_scale, restore the physical unit by
    // using vol_scale as the coefficient: fext_m3s = rsv_fext_LP × vol_scale.
    auto& brow = lp.row_at(balance_rows.at(buid));
    brow[rc] = vol_scale;
  }

  const StorageOptions opts {
      .use_state_variable = reservoir().use_state_variable.value_or(true),
      .daily_cycle = reservoir().daily_cycle.value_or(false),
      .energy_scale = vol_scale,
      .flow_scale = vol_scale,
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

  // rsv_fext LP variable is in physical_flow / energy_scale units; multiply
  // primal by energy_scale to recover m³/s, divide reduced cost accordingly.
  // Uses col_scale_sol/cost helpers for uniform rescaling.
  if (std::abs(energy_scale() - 1.0) > std::numeric_limits<double>::epsilon()) {
    out.add_col_sol(cname,
                    "extraction",
                    id(),
                    extraction_cols,
                    col_scale_sol(energy_scale()));
    out.add_col_cost(cname,
                     "extraction",
                     id(),
                     extraction_cols,
                     col_scale_cost(energy_scale()));
  } else {
    out.add_col_sol(cname, "extraction", id(), extraction_cols);
    out.add_col_cost(cname, "extraction", id(), extraction_cols);
  }

  return StorageBase::add_to_output(out, ClassName.full_name());
}

}  // namespace gtopt
