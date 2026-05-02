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

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirLP::ReservoirLP(const Reservoir& preservoir, const InputContext& ic)
    : StorageBase(preservoir, ic, Element::class_name)
    , capacity(ic, Element::class_name, id(), std::move(reservoir().capacity))
    , scost(ic, Element::class_name, id(), std::move(reservoir().scost))
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
  static constexpr const auto& cname = Element::class_name;
  static constexpr auto ampl_name = Element::class_name.snake_case();

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

  // Resolve energy_scale from VariableScaleMap (default 1.0 if not set).
  const double energy_scale =
      sc.options().variable_scale_map().lookup("Reservoir", "energy", uid());

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
        .class_name = Element::class_name.full_name(),
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
      .class_name = Element::class_name.full_name(),
      .variable_uid = uid(),
      .energy_scale = energy_scale,
      .flow_scale = flow_scale,
      .scost = rsv_scost,
  };
  if (!StorageBase::add_to_lp(cname,
                              ampl_name,
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

  // PLP-style spill routing: when the reservoir specifies an optional
  // `spill_junction`, also wire the drain column into that junction's
  // per-block balance row with coefficient +1 (m³/s as inflow).  This
  // mirrors PLP's `qv` chain (qv28 appears in COLBUN's balance AND in
  // the downstream MACHICURA junction balance via SerVer).  When unset
  // the drain remains a pure storage sink — matching PLP behaviour for
  // reservoirs whose `SerVer = 0`.
  if (const auto& spill_junction_uid = reservoir().spill_junction;
      spill_junction_uid.has_value())
  {
    const auto& spill_junction =
        sc.element<JunctionLP>(JunctionLPSId {*spill_junction_uid});
    if (spill_junction.is_active(stage)) {
      const auto& spill_balance_rows =
          spill_junction.balance_rows_at(scenario, stage);
      if (const auto* dcols_ptr = find_drain_cols(scenario, stage); dcols_ptr) {
        for (auto&& block : blocks) {
          const auto buid = block.uid();
          auto& sbrow = lp.row_at(spill_balance_rows.at(buid));
          sbrow[dcols_ptr->at(buid)] = 1.0;
        }
      }
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  extraction_cols[st_key] = std::move(rcols);

  // Register reservoir-specific PAMPL columns.  Storage-generic variables
  // (energy/drain/eini/efin/soft_emin) are registered centrally by
  // StorageBase::add_to_lp.
  sc.add_ampl_variable(ampl_name,
                       uid(),
                       ExtractionName,
                       scenario,
                       stage,
                       extraction_cols.at(st_key));

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
  static constexpr const auto& cname = Element::class_name;

  // Extraction columns have .scale = flow_scale; auto-descaled by
  // LinearInterface's get_col_sol() / get_col_cost().
  out.add_col_sol(cname, ExtractionName, id(), extraction_cols);
  out.add_col_cost(cname, ExtractionName, id(), extraction_cols);

  return StorageBase::add_to_output(out, Element::class_name.full_name());
}

}  // namespace gtopt
