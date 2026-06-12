/**
 * @file      solver_status.hpp
 * @brief     Solver monitoring API: write iteration status to a JSON file
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the `write_solver_status()` free function that serialises
 * solver iteration history and real-time workpool statistics into a JSON
 * file for external monitoring tools (e.g. `scripts/gtopt_monitor.py`).
 *
 * Extracted from SDDPMethod::write_api_status() to decouple the
 * monitoring serialisation logic from the solver class.
 */

#pragma once

#include <string>
#include <vector>

#include <gtopt/sddp_method.hpp>

namespace gtopt
{

class SolverMonitor;

/// Snapshot of the solver's live convergence state.
///
/// Passed to `write_solver_status()` so the free function does not
/// need access to the solver's atomic members or options struct.
struct SolverStatusSnapshot
{
  /// Current iteration number.  Strong-typed so the ~2 callers that
  /// populate it don't need `static_cast<int>(IterationIndex)` unwraps;
  /// the JSON serialiser (`solver_status.cpp`) renders it as an int
  /// via `strong::formattable` without any conversion.
  IterationIndex iteration_index {};
  double gap {};  ///< Current relative convergence gap
  double lower_bound {};  ///< Current lower bound
  double upper_bound {};  ///< Current upper bound
  bool converged {};  ///< Whether the solver has converged
  int max_iterations {};  ///< SDDPOptions::max_iterations
  int min_iterations {};  ///< SDDPOptions::min_iterations
  int current_pass {};  ///< 0=idle, 1=forward, 2=backward
  int scenes_done {};  ///< Scenes completed in current pass
  std::string solver {};  ///< Solver identity ("name/version")
  std::string method {};  ///< Planning method ("sddp", "monolithic", …)
  const PhaseGridRecorder* phase_grid {};  ///< Non-owning; null if no grid

  // ── Async scene execution fields (populated only in async mode) ──

  /// Per-scene current iteration index (empty in sync mode).
  std::vector<int> scene_iterations {};
  /// Per-scene state label: "training", "simulation", "done" (empty in sync).
  std::vector<std::string> scene_states {};
  /// Number of scenes that have individually converged.
  int converged_scenes {};
  /// Current iteration spread (max - min across non-done scenes).
  int spread {};
  /// Configured maximum async spread (0 = synchronous).
  int max_async_spread {};
  /// WorkPool queue depth at snapshot time.
  int pool_tasks_pending {};
  /// WorkPool active tasks at snapshot time.
  int pool_tasks_active {};
  /// Current CPU load at snapshot time.
  double pool_cpu_load {};

  // ── Memory and LP task stats ──

  /// System memory usage % at snapshot time.
  double pool_memory_percent {};
  /// Process RSS in MB at snapshot time.
  double pool_process_rss_mb {};
  /// System available memory in MB at snapshot time.
  double pool_available_memory_mb {};
  /// Total LP tasks dispatched so far.
  size_t lp_tasks_dispatched {};
  /// Average CPU % per LP task.
  double avg_lp_task_cpu_pct {};
  /// Average RSS delta (MB) per LP task.
  double avg_lp_task_rss_delta_mb {};
};

/// Write solver status JSON to a file.
///
/// Builds a JSON string with the solver's current state, per-iteration
/// history, and real-time workpool statistics, then writes it atomically
/// (via `SolverMonitor::write_status()`).
///
/// @param filepath        Output JSON file path
/// @param results         Vector of per-iteration results
/// @param elapsed_seconds Total elapsed time since solve() started
/// @param snapshot        Current solver state snapshot
/// @param monitor         SolverMonitor for real-time workpool stats
void write_solver_status(const std::string& filepath,
                         const std::vector<SDDPIterationResult>& results,
                         double elapsed_seconds,
                         const SolverStatusSnapshot& snapshot,
                         const SolverMonitor& monitor);

}  // namespace gtopt
