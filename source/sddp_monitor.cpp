/**
 * @file      sddp_monitor.cpp
 * @brief     SDDP monitoring API implementation
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements `write_sddp_api_status()`: serialises SDDP iteration
 * history and real-time workpool statistics into a JSON status file
 * for external monitoring tools.
 *
 * Extracted from SDDPSolver::write_api_status().
 */

#include <chrono>
#include <cstddef>
#include <format>
#include <string>

#include <gtopt/sddp_monitor.hpp>
#include <gtopt/solver_monitor.hpp>

namespace gtopt
{

void write_sddp_api_status(const std::string& filepath,
                           const std::vector<SDDPIterationResult>& results,
                           double elapsed_seconds,
                           const SDDPStatusSnapshot& snapshot,
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
    json += "]\n";

    json += (i + 1 < results.size()) ? "    },\n" : "    }\n";
  }
  json += "  ],\n";

  // ── Real-time workpool monitoring history ──
  monitor.append_history_json(json);

  json += "}\n";

  // Write atomically via SolverMonitor::write_status (write tmp, rename)
  SolverMonitor::write_status(json, filepath);
}

}  // namespace gtopt
