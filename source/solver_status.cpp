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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_status.hpp>
#include <gtopt/utils.hpp>
#include <unistd.h>

namespace gtopt
{

namespace
{

/// Lightweight discovery registry: write
/// ``$XDG_CACHE_HOME/gtopt/runs/<pid>`` (or ``~/.cache/gtopt/runs/<pid>``)
/// containing the absolute path of the output directory the solver is
/// writing to.  This lets ``run_gtopt --list`` enumerate live runs and
/// ``run_gtopt --attach <pid>`` resolve the case dir without scanning
/// the filesystem.  Stale entries (process gone) are skipped by the
/// reader via ``kill(pid, 0)``; we also delete our own entry on
/// graceful exit via std::atexit.  No-op on any I/O error — monitoring
/// must never affect solve correctness.
[[nodiscard]] std::filesystem::path runs_registry_dir()
{
  // ``std::getenv`` is the only portable way to read process env;
  // safe here because the env is never mutated after main() runs.
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  std::filesystem::path base;
  if (xdg != nullptr && *xdg != '\0') {
    base = xdg;
  } else if (const char* home = std::getenv("HOME"); home != nullptr) {
    base = std::filesystem::path {home} / ".cache";
  } else {
    return {};  // Unsupported environment; skip registry.
  }
  return base / "gtopt" / "runs";
}

void register_run_once(const std::string& filepath)
{
  static std::once_flag once;
  std::call_once(once,
                 [&filepath]() noexcept
                 {
                   try {
                     const auto dir = runs_registry_dir();
                     if (dir.empty()) {
                       return;
                     }
                     std::error_code ec;
                     std::filesystem::create_directories(dir, ec);
                     if (ec) {
                       return;
                     }
                     const auto entry = dir / std::to_string(::getpid());
                     std::ofstream out(entry, std::ios::trunc);
                     if (!out) {
                       return;
                     }
                     // Absolute path to the output directory (parent of the
                     // status file) — readers attach to the case via this
                     // directory.
                     const auto out_dir = std::filesystem::absolute(
                                              std::filesystem::path {filepath})
                                              .parent_path();
                     out << out_dir.string() << '\n';
                     out.close();
                     // Best-effort cleanup on normal exit.
                     static std::filesystem::path entry_to_remove;
                     entry_to_remove = entry;
                     // atexit returning non-zero just means "could not
                     // register" — registry cleanup is best-effort.
                     static_cast<void>(std::atexit(
                         []() noexcept
                         {
                           std::error_code ec;
                           std::filesystem::remove(entry_to_remove, ec);
                         }));
                   } catch (...) {
                     // Registry is monitoring-only; never propagate.
                   }
                 });
}

}  // namespace

void register_solver_run(const std::string& filepath)
{
  register_run_once(filepath);
}

std::string build_iteration_status_json(
    const std::vector<SDDPIterationResult>& results,
    double elapsed_seconds,
    const SolverStatusSnapshot& snapshot)
{
  // Build JSON manually using std::format to avoid adding a new
  // dependency.  This is monitoring output only — correctness over
  // aesthetics.
  //
  // This produces the ITERATION portion only: it omits both the
  // `pool_*` scalars and the trailing `realtime` block (and the closing
  // `}`).  Those are appended fresh by the SolverMonitor on its own
  // sampling cadence so the CPU/MEM/Workers numbers stay live between
  // iterations.  Key order within a JSON object is not significant for
  // the Python readers (`json.load`), so moving `pool_*` to the tail
  // alongside `realtime` keeps the schema (set of keys) byte-identical.

  std::string json;
  json.reserve(4096);

  const auto now_ts = std::chrono::duration<double>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  // Determine current state label
  const char* status_str = nullptr;
  if (snapshot.converged) {
    status_str = "converged";
  } else if (snapshot.iteration_index == IterationIndex {0}) {
    status_str = "initializing";
  } else {
    status_str = "running";
  }

  json += "{\n";
  json += std::format("  \"version\": 1,\n");
  json += std::format("  \"timestamp\": {:.3f},\n", now_ts);
  json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed_seconds);
  // PID lets external tools (run_gtopt --attach, --list) verify the
  // owning process is still alive via `kill(pid, 0)` before trusting
  // any of the bounds / iteration counters below.  Cheap; no I/O.
  json +=
      std::format("  \"pid\": {},\n", static_cast<std::int64_t>(::getpid()));
  json += std::format("  \"status\": \"{}\",\n", status_str);
  json += std::format("  \"iteration\": {},\n", snapshot.iteration_index);
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
    for (const auto& [si, iters] : enumerate(snapshot.scene_iterations)) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{}", iters);
    }
    json += "],\n";

    // Per-scene states
    json += "    \"scene_states\": [";
    for (const auto& [si, st] : enumerate(snapshot.scene_states)) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("\"{}\"", st);
    }
    json += "]\n";
    json += "  },\n";
  }

  // ── LP task stats ──
  //
  // NOTE: the `pool_memory_percent` / `pool_process_rss_mb` /
  // `pool_available_memory_mb` scalars are NOT emitted here — they are
  // appended fresh by the monitor's realtime block (see
  // `SolverMonitor::build_realtime_status_json`) so they stay live
  // between iterations.
  if (snapshot.lp_tasks_dispatched > 0) {
    json += "  \"lp_task_stats\": {\n";
    json +=
        std::format("    \"dispatched\": {},\n", snapshot.lp_tasks_dispatched);
    json += std::format("    \"avg_cpu_pct\": {:.1f},\n",
                        snapshot.avg_lp_task_cpu_pct);
    json += std::format("    \"avg_rss_delta_mb\": {:.1f}\n",
                        snapshot.avg_lp_task_rss_delta_mb);
    json += "  },\n";
  }

  // ── Iteration history ──
  json += "  \"history\": [\n";
  for (const auto& [i, r] : enumerate(results)) {
    json += "    {\n";
    json += std::format("      \"iteration\": {},\n", r.iteration_index);
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
    for (const auto& [si, ub] : enumerate(r.scene_upper_bounds)) {
      json += (si > 0) ? ", " : "";
      json += std::format("{:.6f}", ub);
    }
    json += "],\n";

    // Per-scene lower bounds
    json += "      \"scene_lower_bounds\": [";
    for (const auto& [si, lb] : enumerate(r.scene_lower_bounds)) {
      json += (si > 0) ? ", " : "";
      json += std::format("{:.6f}", lb);
    }
    json += ']';

    // Per-scene iteration snapshot (async mode only)
    if (!r.scene_iterations.empty()) {
      json += ",\n      \"scene_iterations\": [";
      for (const auto& [si, it] : enumerate(r.scene_iterations)) {
        json += (si > 0) ? ", " : "";
        json += std::format("{}", it);
      }
      json += ']';
    }
    json += '\n';

    const bool is_last = i + 1 == results.size();
    json += is_last ? "    }\n" : "    },\n";
  }
  json += "  ],\n";

  // ── Phase grid (per-iteration/scene/phase activity) ──
  if (snapshot.phase_grid != nullptr && !snapshot.phase_grid->empty()) {
    json += snapshot.phase_grid->to_json();
    json += ",\n";
  }

  // Intentionally NO closing `}` — the caller appends the fresh
  // pool/realtime block via SolverMonitor::build_realtime_status_json().
  return json;
}

void write_solver_status(const std::string& filepath,
                         const std::vector<SDDPIterationResult>& results,
                         double elapsed_seconds,
                         const SolverStatusSnapshot& snapshot,
                         SolverMonitor& monitor)
{
  // Register this run in the discovery registry on the very first
  // call.  Idempotent (std::call_once); cleaned up at process exit.
  register_run_once(filepath);

  // Build the iteration portion on the SOLVER thread (where `results`
  // and `phase_grid` are stable).
  std::string iteration_json =
      build_iteration_status_json(results, elapsed_seconds, snapshot);

  // Hand the prebuilt iteration string + path to the monitor so its
  // background thread keeps refreshing the realtime/pool block every
  // tick between iterations (it copies the string under its own mutex —
  // cheap, and it never touches live solver state).
  monitor.update_status(iteration_json, filepath);

  // Do one immediate fresh write at the iteration boundary so the file
  // is current the instant the iteration finishes.
  monitor.build_realtime_status_json(iteration_json);
  SolverMonitor::write_status(iteration_json, filepath);
}

}  // namespace gtopt
