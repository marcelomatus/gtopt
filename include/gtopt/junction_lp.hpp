/**
 * @file      junction_lp.hpp
 * @brief     Header for junction linear programming formulation
 * @date      Tue Jul 29 23:08:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the linear programming formulation for junctions in power
 * system optimization models. It handles flow balance constraints and optional
 * drain effects at network connection points.
 *
 * @details The JunctionLP class extends ObjectLP to provide:
 * - Flow balance constraints for each scenario/stage combination
 * - Modeling of energy drain/loss effects
 * - Integration with the overall system LP formulation
 *
 * @see Junction for the base junction data structure
 * @see ObjectLP for the base LP object functionality
 */

#pragma once

#include <gtopt/junction.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @class JunctionLP
 * @brief Linear programming formulation for power system junctions
 *
 * @details This class provides the LP formulation for network junctions,
 * including:
 * - Flow balance constraints (Kirchhoff's current law)
 * - Optional drain/loss terms
 * - Scenario/stage specific constraint indexing
 *
 * The class inherits from ObjectLP<Junction> to provide basic LP object
 * functionality while adding junction-specific constraints.
 */
class JunctionLP : public ObjectLP<Junction>
{
public:
  constexpr static std::string_view ClassName = "Junction"; ///< Class identifier

  /**
   * @brief Construct a JunctionLP from input data
   * @param ic Input context providing system-wide parameters
   * @param pjunction Junction data to model
   */
  explicit JunctionLP([[maybe_unused]] const InputContext& ic,
                      Junction pjunction)
      : ObjectLP<Junction>(std::move(pjunction))
  {
  }

  /// @return Reference to the underlying junction data
  [[nodiscard]]
  constexpr auto&& junction() const noexcept
  {
    return ObjectLP<Junction>::object();
  }

  /// @return Whether this junction has drain effects enabled
  [[nodiscard]] constexpr auto drain() const noexcept
  {
    return junction().drain.value_or(false);
  }

  /// @copydoc drain()
  [[nodiscard]] constexpr auto is_drain() const noexcept
  {
    return junction().drain.value_or(false);
  }

  /**
   * @brief Add junction constraints to the linear program
   * @param sc System context containing model parameters
   * @param lp Linear program to modify
   * @return true if successful, false otherwise
   */
  bool add_to_lp(const SystemContext& sc, LinearProblem& lp);

  /**
   * @brief Add junction results to output context
   * @param out Output context to populate
   * @return true if successful, false otherwise
   */
  bool add_to_output(OutputContext& out) const;

  /**
   * @brief Get balance constraint rows for a scenario/stage combination
   * @param scenario Scenario identifier
   * @param stage Stage identifier
   * @return Const reference to the balance constraint rows
   */
  [[nodiscard]] auto&& balance_rows_at(const ScenarioUid scenario,
                                       const StageUid stage) const
  {
    return balance_rows.at({scenario, stage});
  }

private:
  STBIndexHolder<RowIndex> balance_rows; ///< Balance constraint row indices
  STBIndexHolder<ColIndex> drain_cols;   ///< Drain variable column indices
};

}  // namespace gtopt
