/**
 * @file      bus_lp.hpp
 * @brief     Linear Programming representation of a Bus for optimization
 * problems
 * @date      Mon Mar 24 09:40:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The BusLP class provides a linear programming (LP) compatible representation
 * of a Bus, which is a fundamental component for power system optimization.
 * It maintains the bus's electrical properties and provides methods for LP
 * formulation while using modern C++23 features for better ergonomics.
 *
 * @note Uses C++23 features including deducing this and structured bindings
 */

#pragma once

#include <gtopt/bus.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class BusLP : public ObjectLP<Bus>
{
public:
  static constexpr LPClassName ClassName {"Bus", "bus"};

  /// Constructs a BusLP from a Bus and input context
  /// @param pbus The bus to wrap
  /// @param ic Input context for LP construction
  [[nodiscard]]
  explicit constexpr BusLP(const Bus& pbus,
                           [[maybe_unused]] const InputContext& ic) noexcept
      : ObjectLP<Bus>(pbus)
  {
  }

  [[nodiscard]]
  constexpr const auto& bus() const noexcept
  {
    return object();
  }

  [[nodiscard]]
  constexpr auto reference_theta() const noexcept
  {
    return bus().reference_theta;
  }

  [[nodiscard]]
  constexpr auto voltage() const noexcept
  {
    return bus().voltage.value_or(1.0);
  }

  [[nodiscard]]
  constexpr auto use_kirchhoff() const noexcept
  {
    return bus().use_kirchhoff.value_or(true);
  }

  [[nodiscard]]
  auto needs_kirchhoff(const SystemContext& sc) const -> bool;

  [[nodiscard]]
  bool add_to_lp(const SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  [[nodiscard]]
  bool add_to_output(OutputContext& out) const;

  [[nodiscard]]
  constexpr const auto& balance_rows_at(const ScenarioLP& scenario,
                                        const StageLP& stage) const
  {
    return balance_rows.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]]
  constexpr auto theta_cols_at(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp,
                               const std::vector<BlockLP>& blocks) const
      -> const BIndexHolder<ColIndex>&
  {
    const auto key = std::pair {scenario.uid(), stage.uid()};
    if (const auto it = theta_cols.find(key); it != theta_cols.end()) {
      return it->second;
    }
    return lazy_add_theta(sc, scenario, stage, lp, blocks);
  }

private:
  [[nodiscard]]
  auto lazy_add_theta(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      LinearProblem& lp,
                      std::span<const BlockLP> blocks) const
      -> const BIndexHolder<ColIndex>&;

  STBIndexHolder<RowIndex> balance_rows;
  mutable STBIndexHolder<ColIndex> theta_cols;
};

using BusLPId = ObjectId<class BusLP>;
using BusLPSId = ObjectSingleId<class BusLP>;

}  // namespace gtopt
