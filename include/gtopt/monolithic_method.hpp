/**
 * @file      monolithic_method.hpp
 * @brief     Monolithic planning method — solves each (scene, phase) LP
 *            independently using a parallel work pool
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * MonolithicMethod is the default solver.  Each scene's phases are solved
 * sequentially (propagating state variables), but different scenes are
 * solved in parallel via the adaptive work pool.
 */

#pragma once

#include <chrono>
#include <string>

#include <gtopt/enum_option.hpp>
#include <gtopt/planning_method.hpp>

namespace gtopt
{

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

  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;
};

}  // namespace gtopt
