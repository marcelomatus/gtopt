/**
 * @file      reservoir_lp.hpp
 * @brief     Defines the ReservoirLP class for linear programming
 * representation
 * @date      Wed Jul 30 23:15:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ReservoirLP class which provides the linear
 * programming representation of water reservoirs, including their storage
 * constraints and relationships with junctions and waterways.
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using ReservoirLPId = ObjectId<class ReservoirLP>;
using ReservoirLPSId = ObjectSingleId<class ReservoirLP>;

/**
 * @brief Linear programming representation of a water reservoir
 *
 * This class extends StorageLP to provide LP-specific functionality for
 * reservoirs, including:
 * - Storage capacity constraints
 * - Water extraction constraints
 * - Relationships with connected junctions
 */
class ReservoirLP : public StorageLP<ObjectLP<Reservoir>>
{
public:
  static constexpr LPClassName ClassName {"Reservoir", "rsv"};

  using StorageBase = StorageLP<ObjectLP<Reservoir>>;

  explicit ReservoirLP(const Reservoir& preservoir, const InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
  }

  [[nodiscard]] constexpr auto junction_sid() const noexcept
  {
    return JunctionLPSId {reservoir().junction};
  }

  [[nodiscard]] constexpr auto flow_conversion_rate() const noexcept
  {
    return reservoir().flow_conversion_rate.value_or(3.6);
  }

  [[nodiscard]] constexpr auto spillway_cost() const noexcept
  {
    return reservoir().spillway_cost;
  }

  [[nodiscard]] constexpr auto spillway_capacity() const noexcept
  {
    return reservoir().spillway_capacity.value_or(+6'000.0);
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched capacity;
  STBIndexHolder<ColIndex> extraction_cols;
};

}  // namespace gtopt
