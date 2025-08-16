/**
 * @file      generator_lp.cpp
 * @brief     Implementation of GeneratorLP class for generator LP formulation
 * @date      Tue Apr  1 22:03:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the GeneratorLP class, which handles the
 * representation of power generators in linear programming problems. It
 * includes methods to:
 * - Create variables for generation across time blocks
 * - Add capacity constraints
 * - Add bus power balance contributions
 * - Process generation costs in the objective function
 * - Output planning results for generation variables
 */

#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

/**
 * @brief Constructs a GeneratorLP from a Generator
 * @param ic Input context for parameter processing
 * @param pgenerator Generator object to convert to LP representation
 *
 * Creates an LP representation of a generator including time-dependent
 * parameters like minimum/maximum generation limits, loss factors, and costs.
 */
GeneratorLP::GeneratorLP(Generator generator, const InputContext& ic)
    : CapacityBase(std::move(generator), ic, ClassName)
    , pmin(ic, ClassName, id(), std::move(object().pmin))
    , pmax(ic, ClassName, id(), std::move(object().pmax))
    , lossfactor(ic, ClassName, id(), std::move(object().lossfactor))
    , gcost(ic, ClassName, id(), std::move(object().gcost))
{
  SPDLOG_DEBUG(
      fmt::format("GeneratorLP created for generator with uid {}", uid()));
}

/**
 * @brief Adds generator variables and constraints to the linear problem
 * @param sc System context containing current state
 * @param lp Linear problem to add variables and constraints to
 * @return True if successful, false otherwise
 *
 * This method creates:
 * 1. Generation variables for each time block
 * 2. Capacity constraints linking generation to installed capacity
 * 3. Contributions to bus power balance equations
 *
 * It handles:
 * - Time-dependent generation limits
 * - Generator loss factors
 * - Generation costs in the objective function
 * - Capacity constraints when capacity expansion is modeled
 */
bool GeneratorLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) [[unlikely]] {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus = sc.element<BusLP>(bus_sid());
  if (!bus.is_active(stage)) [[unlikely]] {
    return true;
  }

  auto&& [stage_capacity, capacity_col] = capacity_and_col(stage, lp);

  const auto stage_gcost = gcost.optval(stage.uid()).value_or(0.0);
  const auto stage_lossfactor = lossfactor.optval(stage.uid()).value_or(0.0);

  const auto& balance_rows = bus.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> gcols;
  BIndexHolder<RowIndex> crows;
  gcols.reserve(blocks.size());
  crows.reserve(blocks.size());

  const auto guid = uid();
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto balance_row = balance_rows.at(buid);

    const auto [block_pmax, block_pmin] =
        sc.block_maxmin_at(stage, block, pmax, pmin, stage_capacity);

    SPDLOG_DEBUG(
        fmt::format("GeneratorLP::add_to_lp: gen {} stage {} block {} pmin {} "
                    "pmax {} capacity {}",
                    guid,
                    stage.uid(),
                    block.uid(),
                    block_pmin,
                    block_pmax,
                    stage_capacity));

    // Create generation variable for this time block
    const auto gcol = lp.add_col(
        {.name = sc.lp_label(scenario, stage, block, cname, "gen", guid),
         .lowb = block_pmin,
         .uppb = block_pmax,
         .cost = sc.block_ecost(scenario, stage, block, stage_gcost)});
    gcols[buid] = gcol;

    // Add generator output to the bus power balance equation
    // Factor (1-lossfactor) accounts for generator losses
    auto& brow = lp.row_at(balance_row);
    brow[gcol] = 1 - stage_lossfactor;

    // Add capacity constraint if capacity expansion is modeled
    // Ensures generation <= installed capacity
    if (capacity_col) {
      auto crow = SparseRow {.name = sc.lp_label(
                                 scenario, stage, block, cname, "cap", guid)}
                      .greater_equal(0);
      crow[*capacity_col] = 1;
      crow[gcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  // Store generation and capacity rows for output
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  if (!gcols.empty()) {
    generation_cols[st_key] = std::move(gcols);
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }

  return true;
}

/**
 * @brief Adds generator output results to the output context
 * @param out Output context to add results to
 * @return True if successful, false otherwise
 *
 * Processes planning results for:
 * - Generation variables (primal solution and costs)
 * - Capacity constraint dual values (shadow prices)
 * - Capacity-related outputs via base class
 */
bool GeneratorLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  const auto pid = id();
  out.add_col_sol(cname, "generation", pid, generation_cols);
  out.add_col_cost(cname, "generation", pid, generation_cols);
  out.add_row_dual(cname, "capacity", pid, capacity_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
