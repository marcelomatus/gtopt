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
  constexpr static std::string_view ClassName = "Reservoir";

  using StorageBase = StorageLP<ObjectLP<Reservoir>>;

  explicit ReservoirLP(Reservoir preservoir, const InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir(this auto&& self) noexcept
  {
    return std::forward<decltype(self)>(self).object();
  }

  [[nodiscard]] constexpr auto junction() const noexcept
  {
    return JunctionLPSId {reservoir().junction};
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
