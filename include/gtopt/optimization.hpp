/**c
 * @file      optimization.hpp<gtopt>
 * @brief     Header of Optimization class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Optimization class, which contains all the
 * optimization elements.
 */

#pragma once

#include <gtopt/options.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system.hpp>

namespace gtopt
{

/**
 * @brief Represents a complete power optimization model
 */
struct Optimization
{
  Options options {};
  Simulation simulation {};
  System system {};

  Optimization& merge(Optimization& opt);
};

}  // namespace gtopt
