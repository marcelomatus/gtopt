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
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirLP::ReservoirLP(Reservoir preservoir, const InputContext& ic)
    : StorageBase(std::move(preservoir), ic, ClassName)
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
bool ReservoirLP::add_to_lp(const SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  constexpr std::string_view cname = ClassName;

  if (!is_active(stage)) {
    return true;
  }

  const auto stage_capacity = capacity.at(stage.uid()).value_or(CoinDblMax);

  const auto& junction = sc.element<JunctionLP>(this->junction());
  if (!junction.is_active(stage)) {
    return true;
  }

  auto&& balance_rows = junction.balance_rows_at(scenario, stage);
  auto&& blocks = stage.blocks();

  BIndexHolder<ColIndex> rcols;
  rcols.reserve(blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto rc = lp.add_col(SparseCol {
        .name = sc.lp_label(scenario, stage, block, cname, "fext", uid())}
                                   .free());
    rcols[buid] = rc;

    // the flow in the reservoir is an extraction, therefore, it adds flow  to
    // the junction balance
    auto& brow = lp.row_at(balance_rows.at(buid));
    brow[rc] = 1;
  }

  if (!StorageBase::add_to_lp(
          sc, scenario, stage, lp, cname, rcols, stage_capacity))
  {
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
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "extraction", id(), extraction_cols);
  out.add_col_cost(cname, "extraction", id(), extraction_cols);

  return StorageBase::add_to_output(out, cname);
}

}  // namespace gtopt
