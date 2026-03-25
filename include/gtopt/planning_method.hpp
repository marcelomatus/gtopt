/**
 * @file      planning_method.hpp
 * @brief     Abstract interface for planning methods and factory function
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a common interface for different solving strategies used by
 * PlanningLP::resolve().  The built-in implementations are:
 *
 * - **MonolithicMethod** (monolithic_method.hpp) – solves each (scene,
 *   phase) LP independently in parallel using the adaptive work pool
 *   (the default).
 *
 * - **SDDPPlanningMethod** (sddp_method.hpp) – wraps SDDPMethod with
 *   iterative forward/backward Benders decomposition across phases and
 *   scenes.
 *
 * - **CascadePlanningMethod** (cascade_method.hpp) – multi-level cascade
 *   decomposition with progressive refinement.
 *
 * The solver type is selected via the `method` option in the JSON
 * options block (`"monolithic"`, `"sddp"`, or `"cascade"`).
 */

#pragma once

#include <expected>
#include <memory>

#include <gtopt/error.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class PlanningLP;

// ─── PlanningMethod interface ───────────────────────────────────────────────

/**
 * @class PlanningMethod
 * @brief Abstract interface for planning problem solvers
 *
 * All solvers take a mutable reference to a PlanningLP and solve its LP
 * subproblems.  The return value is the number of successfully processed
 * scenes (or an error).
 */
class PlanningMethod
{
public:
  PlanningMethod() = default;
  virtual ~PlanningMethod() = default;

  PlanningMethod(const PlanningMethod&) = delete;
  PlanningMethod& operator=(const PlanningMethod&) = delete;
  PlanningMethod(PlanningMethod&&) = default;
  PlanningMethod& operator=(PlanningMethod&&) = default;

  /**
   * @brief Solve the planning problem
   * @param planning_lp The LP model to solve (modified in place)
   * @param opts Solver options for individual LP subproblems
   * @return Number of scenes processed, or an error
   */
  [[nodiscard]] virtual auto solve(PlanningLP& planning_lp,
                                   const SolverOptions& opts)
      -> std::expected<int, Error> = 0;
};

// ─── Factory ────────────────────────────────────────────────────────────────

class PlanningOptionsLP;

/**
 * @brief Create a solver instance based on options
 * @param options The PlanningOptionsLP with all resolved SDDP configuration
 * @param num_phases Number of phases in the simulation (used to validate
 *        SDDP requirements; falls back to monolithic when < 2)
 * @return Unique pointer to the selected solver
 */
[[nodiscard]] std::unique_ptr<PlanningMethod> make_planning_method(
    const PlanningOptionsLP& options, size_t num_phases = 0);

}  // namespace gtopt
