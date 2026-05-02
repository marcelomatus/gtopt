/**
 * @file      lng_terminal_lp.hpp
 * @brief     Defines the LngTerminalLP class for linear programming
 *            representation
 * @date      Sun Apr 13 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LngTerminalLP class which provides the linear
 * programming representation of LNG storage terminals, including their
 * volume balance constraints and generator fuel consumption coupling.
 */

#pragma once

#include <gtopt/lng_terminal.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{

using LngTerminalLPId = ObjectId<class LngTerminalLP>;
using LngTerminalLPSId = ObjectSingleId<class LngTerminalLP>;

/**
 * @brief Linear programming representation of an LNG storage terminal
 *
 * This class extends StorageLP to provide LP-specific functionality for
 * LNG terminals, including:
 * - Tank volume balance with boil-off losses
 * - Regasification (send-out) flow variables
 * - Delivery inflow schedule
 * - Generator fuel consumption coupling via heat rates
 */
class LngTerminalLP : public StorageLP<ObjectLP<LngTerminal>>
{
public:
  static constexpr std::string_view DeliveryName {"delivery"};

  using StorageBase = StorageLP<ObjectLP<LngTerminal>>;

  explicit LngTerminalLP(const LngTerminal& pterminal, const InputContext& ic);

  [[nodiscard]] constexpr auto&& terminal(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto flow_conversion_rate() const noexcept
  {
    return terminal().flow_conversion_rate.value_or(
        LngTerminal::default_flow_conversion_rate);
  }

  [[nodiscard]] constexpr auto spillway_cost() const noexcept
  {
    return terminal().spillway_cost;
  }

  [[nodiscard]] constexpr auto spillway_capacity() const noexcept
  {
    return terminal().spillway_capacity.value_or(
        LngTerminal::default_spillway_capacity);
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched delivery;
  OptTRealSched scost;
  STBIndexHolder<ColIndex> delivery_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `LngTerminal::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"LngTerminal"`).
static_assert(LngTerminalLP::Element::class_name == LPClassName {"LngTerminal"},
              "LngTerminal::class_name must remain \"LngTerminal\"");

}  // namespace gtopt
