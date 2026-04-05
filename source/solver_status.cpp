/**
 * @file      solver_status.cpp
 * @brief     Solver monitoring API implementation
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements `write_solver_status()`: serialises solver iteration
 * history and real-time workpool statistics into a JSON status file
 * for external monitoring tools.
 *
 * Extracted from SDDPMethod::write_api_status().
 */

#include <chrono>
#include <cstddef>
#include <format>
#include <string>

#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_status.hpp>

namespace gtopt
{

void write_solver_status(const std::string& filepath,
                         const std::vector<SDDPIterationResult>& results,
                         double elapsed_seconds,
                         const SolverStatusSnapshot& snapshot,
                         const SolverMonitor& monitor)
{
  // Build JSON manually using std::format to avoid adding a new
  // dependency.  This is monitoring output only — correctness over
  // aesthetics.

  std::string json;
  json.reserve(4096);

  const auto now_ts = std::chrono::duration<double>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  // Determine current state label
  const char* status_str = nullptr;
  if (snapshot.converged) {
    status_str = "converged";
  } else if (snapshot.iteration == 0) {
    status_str = "initializing";
  } else {
    status_str = "running";
  }

  json += "{\n";
  json += std::format("  \"version\": 1,\n");
  json += std::format("  \"timestamp\": {:.3f},\n", now_ts);
  json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed_seconds);
  json += std::format("  \"status\": \"{}\",\n", status_str);
  json += std::format("  \"iteration\": {},\n", snapshot.iteration);
  json += std::format("  \"lower_bound\": {:.6f},\n", snapshot.lower_bound);
  json += std::format("  \"upper_bound\": {:.6f},\n", snapshot.upper_bound);
  json += std::format("  \"gap\": {:.6f},\n", snapshot.gap);
  json += std::format("  \"converged\": {},\n",
                      snapshot.converged ? "true" : "false");
  json += std::format("  \"max_iterations\": {},\n", snapshot.max_iterations);
  json += std::format("  \"min_iterations\": {},\n", snapshot.min_iterations);
  json += std::format("  \"current_pass\": {},\n", snapshot.current_pass);
  json += std::format("  \"scenes_done\": {},\n", snapshot.scenes_done);
  if (!snapshot.solver.empty()) {
    json += std::format("  \"solver\": \"{}\",\n", snapshot.solver);
  }
  if (!snapshot.method.empty()) {
    json += std::format("  \"method\": \"{}\",\n", snapshot.method);
  }

  // ── Async scene execution state (only when max_async_spread > 0) ──
  if (snapshot.max_async_spread > 0) {
    json += "  \"async\": {\n";
    json += std::format("    \"max_async_spread\": {},\n",
                        snapshot.max_async_spread);
    json += std::format("    \"converged_scenes\": {},\n",
                        snapshot.converged_scenes);
    json += std::format("    \"spread\": {},\n", snapshot.spread);
    json += std::format("    \"pool_tasks_pending\": {},\n",
                        snapshot.pool_tasks_pending);
    json += std::format("    \"pool_tasks_active\": {},\n",
                        snapshot.pool_tasks_active);
    json +=
        std::format("    \"pool_cpu_load\": {:.1f},\n", snapshot.pool_cpu_load);

    // Per-scene iterations
    json += "    \"scene_iterations\": [";
    for (std::size_t si = 0; si < snapshot.scene_iterations.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{}", snapshot.scene_iterations[si]);
    }
    json += "],\n";

    // Per-scene states
    json += "    \"scene_states\": [";
    for (std::size_t si = 0; si < snapshot.scene_states.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("\"{}\"", snapshot.scene_states[si]);
    }
    json += "]\n";
    json += "  },\n";
  }

  // ── Iteration history ──
  json += "  \"history\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    json += "    {\n";
    json += std::format("      \"iteration\": {},\n", r.iteration);
    json += std::format("      \"lower_bound\": {:.6f},\n", r.lower_bound);
    json += std::format("      \"upper_bound\": {:.6f},\n", r.upper_bound);
    json += std::format("      \"gap\": {:.6f},\n", r.gap);
    json += std::format("      \"converged\": {},\n",
                        r.converged ? "true" : "false");
    json += std::format("      \"cuts_added\": {},\n", r.cuts_added);
    json += std::format("      \"infeasible_cuts_added\": {},\n",
                        r.infeasible_cuts_added);
    json +=
        std::format("      \"forward_pass_s\": {:.4f},\n", r.forward_pass_s);
    json +=
        std::format("      \"backward_pass_s\": {:.4f},\n", r.backward_pass_s);
    json += std::format("      \"iteration_s\": {:.4f},\n", r.iteration_s);

    // Per-scene upper bounds
    json += "      \"scene_upper_bounds\": [";
    for (std::size_t si = 0; si < r.scene_upper_bounds.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{:.6f}", r.scene_upper_bounds[si]);
    }
    json += "],\n";

    // Per-scene lower bounds
    json += "      \"scene_lower_bounds\": [";
    for (std::size_t si = 0; si < r.scene_lower_bounds.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{:.6f}", r.scene_lower_bounds[si]);
    }
    json += "]";

    // Per-scene iteration snapshot (async mode only)
    if (!r.scene_iterations.empty()) {
      json += ",\n      \"scene_iterations\": [";
      for (std::size_t si = 0; si < r.scene_iterations.size(); ++si) {
        if (si > 0) {
          json += ", ";
        }
        json += std::format("{}", r.scene_iterations[si]);
      }
      json += "]";
    }
    json += "\n";

    json += (i + 1 < results.size()) ? "    },\n" : "    }\n";
  }
  json += "  ],\n";

  // ── Phase grid (per-iteration/scene/phase activity) ──
  if (snapshot.phase_grid != nullptr && !snapshot.phase_grid->empty()) {
    json += snapshot.phase_grid->to_json();
    json += ",\n";
  }

  // ── Real-time workpool monitoring history ──
  monitor.append_history_json(json);

  json += "}\n";

  // Write atomically via SolverMonitor::write_status (write tmp, rename)
  SolverMonitor::write_status(json, filepath);
}

}  // namespace gtopt
