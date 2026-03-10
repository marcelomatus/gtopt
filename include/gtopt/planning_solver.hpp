/**
 * @file      planning_solver.hpp
 * @brief     Abstract interface for planning solvers (monolithic and SDDP)
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a common interface for different solving strategies used by
 * PlanningLP::resolve().  The two built-in implementations are:
 *
 * - **MonolithicSolver** – solves each (scene, phase) LP independently in
 *   parallel using the adaptive work pool (the default).
 *
 * - **SDDPPlanningSolver** – wraps SDDPSolver with iterative forward/backward
 *   Benders decomposition across phases and scenes.
 *
 * The solver type is selected via the `solver_type` option in the JSON
 * options block (`"monolithic"` or `"sddp"`).
 */

#pragma once

#include <expected>
#include <memory>
#include <string_view>

#include <gtopt/error.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class PlanningLP;

// ─── PlanningSolver interface ───────────────────────────────────────────────

/**
 * @class PlanningSolver
 * @brief Abstract interface for planning problem solvers
 *
 * All solvers take a mutable reference to a PlanningLP and solve its LP
 * subproblems.  The return value is the number of successfully processed
 * scenes (or an error).
 */
class PlanningSolver
{
public:
  PlanningSolver() = default;
  virtual ~PlanningSolver() = default;

  PlanningSolver(const PlanningSolver&) = delete;
  PlanningSolver& operator=(const PlanningSolver&) = delete;
  PlanningSolver(PlanningSolver&&) = default;
  PlanningSolver& operator=(PlanningSolver&&) = default;

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

// ─── MonolithicSolver ───────────────────────────────────────────────────────

/**
 * @class MonolithicSolver
 * @brief Solves each (scene, phase) LP independently using a work pool
 *
 * This is the default solver.  Each scene's phases are solved sequentially
 * (propagating state variables), but different scenes are solved in parallel
 * via the adaptive work pool.
 */
class MonolithicSolver final : public PlanningSolver
{
public:
  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;
};

// ─── Factory ────────────────────────────────────────────────────────────────

/**
 * @brief Create a solver instance based on the solver type name
 * @param solver_type "monolithic" (default) or "sddp"
 * @param cut_sharing_mode "none", "expected", or "max" (for SDDP)
 * @param cut_directory Directory for Benders cut files (for SDDP)
 * @return Unique pointer to the selected solver
 */
[[nodiscard]] std::unique_ptr<PlanningSolver> make_planning_solver(
    std::string_view solver_type,
    std::string_view cut_sharing_mode = "none",
    std::string_view cut_directory = "cuts");

}  // namespace gtopt
