/**
 * @file      planning.hpp
 * @brief     Header of Planning class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Planning class, which contains all the
 * planning elements.
 */

#pragma once

#include <gtopt/planning_options.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>

namespace gtopt
{

/**
 * @brief Hour → (stage, block) mapping persisted alongside the planning
 *        JSON so Python post-processing tools (`ts2gtopt`,
 *        `reconstruct_output_hours`) can expand block-level solver
 *        output back to an hourly series.
 *
 * The solver itself does not consume this data — it is a pure
 * passthrough stored in the planning JSON.  Declaring it in the schema
 * lets `StrictParsePolicy` accept planning files produced by case
 * writers that emit the map, without opening the door to arbitrary
 * unknown members.
 */
struct HourBlockEntry
{
  Uid hour {};
  Uid stage {};
  Uid block {};
};

/**
 * @brief Represents a complete power planning model
 */
struct Planning
{
  PlanningOptions options {};
  Simulation simulation {};
  System system {};
  /// Optional hour→(stage, block) mapping for downstream hourly
  /// reconstruction; not used by the solver, but accepted in the JSON
  /// schema so strict-parse cases from `ts2gtopt` pass.  See
  /// `scripts/ts2gtopt/ts2gtopt.py::build_hour_block_map`.
  std::optional<Array<HourBlockEntry>> hour_block_map {};

  /**
   * @brief Merges another planning object into this one
   *
   * This is a unified template method that handles both lvalue and rvalue
   * references. When merging from an rvalue reference, move semantics are used
   * automatically.
   *
   * @tparam T Planning reference type (can be lvalue or rvalue reference)
   * @param plan The planning object to merge from (will be moved from if it's
   * an rvalue)
   * @return Reference to this planning object
   */
  constexpr void merge(Planning&& plan)
  {
    options.merge(std::move(plan.options));
    simulation.merge(std::move(plan.simulation));
    system.merge(std::move(plan.system));
    if (plan.hour_block_map.has_value()) {
      hour_block_map = std::move(plan.hour_block_map);
    }

    auto _ = std::move(plan);
  }
};

}  // namespace gtopt
