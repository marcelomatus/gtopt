/**
 * @file      planning_method.hpp
 * @brief     Abstract interface for planning methods (monolithic and SDDP)
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a common interface for different solving strategies used by
 * PlanningLP::resolve().  The two built-in implementations are:
 *
 * - **MonolithicMethod** – solves each (scene, phase) LP independently in
 *   parallel using the adaptive work pool (the default).
 *
 * - **SDDPPlanningMethod** – wraps SDDPMethod with iterative forward/backward
 *   Benders decomposition across phases and scenes.
 *
 * The solver type is selected via the `method` option in the JSON
 * options block (`"monolithic"` or `"sddp"`).
 */

#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <gtopt/enum_option.hpp>
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

// ─── MonolithicMethod ───────────────────────────────────────────────────────

/**
 * @class MonolithicMethod
 * @brief Solves each (scene, phase) LP independently using a work pool
 *
 * This is the default solver.  Each scene's phases are solved sequentially
 * (propagating state variables), but different scenes are solved in parallel
 * via the adaptive work pool.
 *
 * ## Monitoring
 *
 * When `enable_api` is true and `api_status_file` is non-empty, the solver
 * writes a JSON status file at `api_status_file` on completion.  The file
 * contains the following indicators:
 *  - `"total_scenes"`: total number of scenes to process.
 *  - `"scenes_done"`:  number of scenes solved (incremented per scene).
 *  - `"scene_times"`:  wall-clock time in seconds for each scene.
 *  - `"elapsed_s"`:    total wall time since solve() was called.
 *  - `"status"`:       `"done"` on completion.
 *  - `"realtime"`:     rolling CPU-load and active-worker history sampled
 *                      by a background thread at `api_update_interval`.
 */
class MonolithicMethod final : public PlanningMethod
{
public:
  /// When true, write a JSON status file after solving completes.
  bool enable_api {false};
  /// Path for the JSON status file (empty = no file written).
  std::string api_status_file {};
  /// Interval between background monitoring samples.
  std::chrono::milliseconds api_update_interval {500};
  /// When true, write LP debug files to lp_debug_directory before solving.
  bool lp_debug {false};
  /// Directory for LP debug files (used when lp_debug is true).
  std::string lp_debug_directory {};
  /// Compression format for LP debug files ("gzip" / "uncompressed" / "").
  /// Empty or "uncompressed" means no compression; any other value uses gzip.
  std::string lp_debug_compression {};
  /// Monolithic solve mode.
  SolveMode solve_mode {SolveMode::monolithic};
  /// CSV file with boundary (future-cost) cuts (empty = none).
  std::string boundary_cuts_file {};
  /// Boundary cuts load mode.
  BoundaryCutsMode boundary_cuts_mode {BoundaryCutsMode::separated};
  /// Maximum iterations to load from boundary cuts file (0 = all).
  int boundary_max_iterations {0};
  /// Global solve timeout in seconds (0 = no timeout).
  /// When non-zero, each LP solve is given this time limit; if exceeded,
  /// the LP is saved to a debug file and a CRITICAL message is logged.
  double solve_timeout {0.0};

  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;
};

// ─── Factory ────────────────────────────────────────────────────────────────

class OptionsLP;

/**
 * @brief Create a solver instance based on options
 * @param options The OptionsLP with all resolved SDDP configuration
 * @param num_phases Number of phases in the simulation (used to validate
 *        SDDP requirements; falls back to monolithic when < 2)
 * @return Unique pointer to the selected solver
 */
[[nodiscard]] std::unique_ptr<PlanningMethod> make_planning_method(
    const OptionsLP& options, size_t num_phases = 0);

}  // namespace gtopt
