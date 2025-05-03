/**
 * @file      optimization.cpp
 * @brief     Header of
 * @date      Fri May  2 00:21:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/optimization.hpp>

namespace gtopt
{

Optimization& Optimization::merge(Optimization& opt)
{
  options.merge(opt.options);
  simulation.merge(opt.simulation);
  system.merge(opt.system);

  return *this;
}
}  // namespace gtopt
