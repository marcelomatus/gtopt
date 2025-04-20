/**
 * @file      lp_options.hpp
 * @brief     Header of
 * @date      Mon Mar 24 10:24:13 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <cstdint>

namespace gtopt
{

enum class LPAlgo : uint8_t
{
  default_algo = 0,
  primal = 1,
  dual = 2,
  barrier = 3,
  last_algo = 4
};

struct LPOptions
{
  constexpr LPOptions() noexcept = default;
  
  LPAlgo algorithm {};
  int threads {};
  bool presolve {true};
  double optimal_eps {};
  double feasible_eps {};
  double barrier_eps {};
  int log_level {};
};

}  // namespace gtopt
