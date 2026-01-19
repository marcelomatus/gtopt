/**c
 * @file      planning.hpp<gtopt>
 * @brief     Header of Planning class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Planning class, which contains all the
 * planning elements.
 */

#pragma once

#include <gtopt/options.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>

namespace gtopt
{

/**
 * @brief Represents a complete power planning model
 */
struct Planning
{
  Options options {};
  Simulation simulation {};
  System system {};

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
  constexpr void merge(Planning&& plan)  // NOLINT
  {
    options.merge(std::move(plan.options));
    simulation.merge(std::move(plan.simulation));
    system.merge(std::move(plan.system));
  }
};

}  // namespace gtopt
