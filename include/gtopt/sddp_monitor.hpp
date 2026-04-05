/**
 * @file      sddp_monitor.hpp
 * @brief     SDDP monitoring API: write iteration status to a JSON file
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the `write_sddp_api_status()` free function that serialises
 * SDDP iteration history and real-time workpool statistics into a JSON
 * file for external monitoring tools (e.g. `scripts/sddp_monitor.py`).
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

/// Snapshot of the SDDP solver's live convergence state.
///
/// Passed to `write_sddp_api_status()` so the free function does not
/// need access to the solver's atomic members or options struct.
struct SDDPStatusSnapshot
{
  int iteration {};  ///< Current iteration number
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
};

/// Write SDDP status JSON to a file.
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
void write_sddp_api_status(const std::string& filepath,
                           const std::vector<SDDPIterationResult>& results,
                           double elapsed_seconds,
                           const SDDPStatusSnapshot& snapshot,
                           const SolverMonitor& monitor);

}  // namespace gtopt
