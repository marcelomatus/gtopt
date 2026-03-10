/**
 * @file      filtration_lp.hpp
 * @brief     Linear Programming representation of a Filtration system
 * @date      Thu Jul 31 01:49:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The FiltrationLP class provides a linear programming (LP) compatible
 * representation of a Filtration system for optimization problems.
 */

#pragma once

#include <gtopt/filtration.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// filtration_lp.hpp).
class SystemLP;

/**
 * @brief LP wrapper for Filtration systems
 *
 * Provides methods for LP formulation of filtration constraints while
 * maintaining connections to waterways and reservoirs.
 */
class FiltrationLP : public ObjectLP<Filtration>
{
public:
  static constexpr LPClassName ClassName {"Filtration", "fil"};

  /// Constructs a FiltrationLP from a Filtration and input context
  /// @param pfiltration The filtration system to wrap
  /// @param ic Input context for LP construction
  [[nodiscard]]
  explicit constexpr FiltrationLP(const Filtration& pfiltration,
                                  [[maybe_unused]] InputContext& ic) noexcept
      : ObjectLP<Filtration>(pfiltration)
  {
  }

  [[nodiscard]] constexpr auto&& filtration(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {filtration().reservoir};
  }
  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {filtration().waterway};
  }

  [[nodiscard]] constexpr auto slope() const noexcept
  {
    return filtration().slope;
  }
  [[nodiscard]] constexpr auto constant() const noexcept
  {
    return filtration().constant;
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Update reservoir-dependent LP coefficients for this filtration.
   *
   * Currently a no-op (the filtration slope and constant are fixed
   * parameters that do not depend on the reservoir volume at run time).
   * This method exists for interface consistency with TurbineLP::update_lp
   * and will be extended in the future if volume-dependent filtration rates
   * are required.
   *
   * @return Always 0 (no coefficients updated)
   */
  [[nodiscard]] constexpr int update_lp(
      [[maybe_unused]] SystemLP& sys,
      [[maybe_unused]] const ScenarioLP& scenario,
      [[maybe_unused]] const StageLP& stage,
      [[maybe_unused]] PhaseIndex phase,
      [[maybe_unused]] int iteration) noexcept
  {
    return 0;
  }

private:
  STBIndexHolder<ColIndex> filtration_cols;
  STBIndexHolder<RowIndex> filtration_rows;
};

}  // namespace gtopt
