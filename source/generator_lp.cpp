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

namespace gtopt
{

/**
 * @brief Constructs a GeneratorLP from a Generator
 * @param ic Input context for parameter processing
 * @param generator Generator object to convert to LP representation
 *
 * Creates an LP representation of a generator including time-dependent
 * parameters like minimum/maximum generation limits, loss factors, and costs.
 */
GeneratorLP::GeneratorLP(const Generator& generator, const InputContext& ic)
    : CapacityBase(generator, ic, Element::class_name)
    , pmin(ic, Element::class_name, id(), std::move(object().pmin))
    , pmax(ic, Element::class_name, id(), std::move(object().pmax))
    , lossfactor(ic, Element::class_name, id(), std::move(object().lossfactor))
    , gcost(ic, Element::class_name, id(), std::move(object().gcost))
    , emission_factor(
          ic, Element::class_name, id(), std::move(object().emission_factor))
{
  SPDLOG_DEBUG("GeneratorLP created for generator with uid {}", uid());
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
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  // Register filter metadata (F9) so `sum(generator(all : type="hydro")...)`
  // predicates can be evaluated at row-assembly time.
  {
    AmplElementMetadata metadata;
    metadata.reserve(2);
    if (const auto& t = generator().type) {
      metadata.emplace_back(TypeKey, *t);
    }
    // Resolve via `sc.element<BusLP>` (handles both Uid and Name forms
    // of the JSON-side `bus` SingleId variant — `std::get<Uid>` would
    // throw if the JSON used a string name).
    metadata.emplace_back(
        BusKey, static_cast<double>(sc.element<BusLP>(bus_sid()).uid()));
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus = sc.element<BusLP>(bus_sid());
  if (!bus.is_active(stage)) [[unlikely]] {
    return true;
  }

  auto&& [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  const auto stage_gcost = gcost.optval(stage.uid()).value_or(0.0);
  const auto stage_lossfactor = lossfactor.optval(stage.uid()).value_or(0.0);

  const auto& balance_rows = bus.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> gcols;
  BIndexHolder<RowIndex> crows;
  map_reserve(gcols, blocks.size());
  map_reserve(crows, blocks.size());

  const auto guid = uid();
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto balance_row = balance_rows.at(buid);

    const auto [block_pmax, block_pmin] =
        sc.block_maxmin_at(stage, block, pmax, pmin, stage_capacity);

    SPDLOG_DEBUG(
        "GeneratorLP::add_to_lp: gen {} stage {} block {} pmin {} "
        "pmax {} capacity {}",
        guid,
        stage.uid(),
        block.uid(),
        block_pmin,
        block_pmax,
        stage_capacity);

    // Create generation variable for this time block
    const auto gcol = lp.add_col({
        .lowb = block_pmin,
        .uppb = block_pmax,
        .cost = CostHelper::block_ecost(scenario, stage, block, stage_gcost),
        .class_name = Element::class_name.full_name(),
        .variable_name = GenerationName,
        .variable_uid = guid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    gcols[buid] = gcol;

    // Add generator output to the bus power balance equation
    // Factor (1-lossfactor) accounts for generator losses
    auto& brow = lp.row_at(balance_row);
    brow[gcol] = 1 - stage_lossfactor;

    // Add capacity constraint if capacity expansion is modeled
    // Ensures generation <= installed capacity
    if (capacity_col) {
      auto crow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = CapacityName,
              .variable_uid = guid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .greater_equal(0);
      crow[*capacity_col] = 1;
      crow[gcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  // Store generation and capacity rows for output
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  if (!gcols.empty()) {
    generation_cols[st_key] = std::move(gcols);
    // Register PAMPL-visible columns with the variable registry.
    sc.add_ampl_variable(ampl_name,
                         guid,
                         GenerationName,
                         scenario,
                         stage,
                         generation_cols.at(st_key));
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }

  // `capainst` is registered centrally by CapacityBase::add_to_lp.

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
  static constexpr std::string_view cname = Element::class_name.full_name();

  const auto pid = id();
  out.add_col_sol(cname, GenerationName, pid, generation_cols);
  out.add_col_cost(cname, GenerationName, pid, generation_cols);
  out.add_row_dual(cname, CapacityName, pid, capacity_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
