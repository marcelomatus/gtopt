/**
 * @file      solver_options.hpp
 * @brief     Linear programming solver configuration options
 * @date      Mon Mar 24 10:24:13 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines options specific to controlling linear programming
 * solvers, including algorithm selection, solver parameters, and tolerances.
 */

#pragma once

#include <cstdint>

namespace gtopt
{
/**
 * @brief Enumeration of linear programming solution algorithms
 */
enum class LPAlgo : uint8_t
{
  /** @brief Use the solver's default algorithm */
  default_algo = 0,

  /** @brief Use the primal simplex algorithm */
  primal = 1,

  /** @brief Use the dual simplex algorithm */
  dual = 2,

  /** @brief Use the interior point (barrier) algorithm */
  barrier = 3,

  /** @brief Sentinel value for iteration/validation */
  last_algo = 4
};

/**
 * @brief Configuration options for linear programming solvers
 *
 * The SolverOptions structure contains parameters that control how linear
 * programming problems are solved, including algorithm selection, parallel
 * processing settings, numerical tolerances, and logging preferences.
 */
struct SolverOptions
{
  /** @brief The solution algorithm to use */
  int algorithm {0};

  /** @brief Number of parallel threads to use (0 = automatic) */
  int threads {0};

  /** @brief Whether to apply presolve optimizations (default: true) */
  bool presolve {true};

  /** @brief Optimality tolerance for solution */
  double optimal_eps {1e-6};

  /** @brief Feasibility tolerance for constraints */
  double feasible_eps {1e-6};

  /** @brief Convergence tolerance for barrier algorithm */
  double barrier_eps {1e-6};

  /** @brief Verbosity level for solver output (0 = none) */
  int log_level {0};
};

}  // namespace gtopt
