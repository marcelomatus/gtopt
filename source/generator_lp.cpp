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

#include "gtopt/block.hpp"

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
GeneratorLP::GeneratorLP(const InputContext& ic, Generator pgenerator)
    : CapacityBase(ic, ClassName, std::move(pgenerator))
    , pmin(ic, ClassName, id(), std::move(generator().pmin))
    , pmax(ic, ClassName, id(), std::move(generator().pmax))
    , lossfactor(ic, ClassName, id(), std::move(generator().lossfactor))
    , gcost(ic, ClassName, id(), std::move(generator().gcost))
    , bus_index(ic.element_index(bus()))
{
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
                            const ScenarioIndex& scenario_index,
                            const StageIndex& stage_index,
                            LinearProblem& lp)
{
  constexpr std::string_view cname = "gen";
  if (!CapacityBase::add_to_lp(sc, scenario_index, stage_index, lp, cname))
      [[unlikely]]
  {
    return false;
  }

  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }

  auto&& bus = sc.element(bus_index);
  if (!bus.is_active(stage_index)) [[unlikely]] {
    return true;
  }

  auto&& [stage_capacity, capacity_col] = capacity_and_col(stage_index, lp);

  const auto stage_gcost = gcost.optval(stage_index).value_or(0.0);
  const auto stage_lossfactor = lossfactor.optval(stage_index).value_or(0.0);

  const auto& balance_rows = bus.balance_rows_at(scenario_index, stage_index);
  const auto& blocks = sc.stage_blocks(stage_index);

  BIndexHolder gcols;
  gcols.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (auto&& [block_index, block, balance_row] :
       enumerate<BlockIndex>(blocks, balance_rows))
  {
    const auto [block_pmax, block_pmin] = sc.block_maxmin_at(
        stage_index, block_index, pmax, pmin, stage_capacity);

    // Create generation variable for this time block
    const auto gc = lp.add_col(
        {.name = sc.stb_label(
             scenario_index, stage_index, block, cname, "gen", uid()),
         .lowb = block_pmin,
         .uppb = block_pmax,
         .cost =
             sc.block_cost(scenario_index, stage_index, block, stage_gcost)});
    gcols.push_back(gc);

    // Add generator output to the bus power balance equation
    // Factor (1-lossfactor) accounts for generator losses
    auto& brow = lp.row_at(balance_row);
    brow[gc] = 1 - stage_lossfactor;

    // Add capacity constraint if capacity expansion is modeled
    // Ensures generation <= installed capacity
    if (capacity_col.has_value()) {
      SparseRow crow {
          .name = sc.stb_label(
              scenario_index, stage_index, block, cname, "cap", uid())};
      crow[capacity_col.value()] = 1;
      crow[gc] = -1;

      crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
    }
  }
  return emplace_bholder(
             scenario_index, stage_index, capacity_rows, std::move(crows))
             .second
      && emplace_bholder(
             scenario_index, stage_index, generation_cols, std::move(gcols))
             .second;
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
  constexpr std::string_view cname = ClassName;

  const auto pid = id();
  out.add_col_sol(cname, "generation", pid, generation_cols);
  out.add_col_cost(cname, "generation", pid, generation_cols);
  out.add_row_dual(cname, "capacity", pid, capacity_rows);

  return CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
