/**
 * @file      solver_monitor.hpp
 * @brief     Solver monitoring API: real-time workpool statistics and JSON
 *            status file for SDDP and Monolithic solvers
 * @date      2026-03-10
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a reusable `SolverMonitor` class that:
 *  - Samples CPU load and active worker-thread count from an
 *    `AdaptiveWorkPool` via a background `std::jthread`.
 *  - Stores the sampled history (a `std::vector<MonitorPoint>`).
 *  - Writes atomic JSON status files for external monitoring tools
 *    (e.g. `scripts/sddp_monitor.py`).
 *
 * The class is used by both `SDDPSolver` (SDDP iteration history +
 * real-time workpool stats) and `MonolithicSolver` (scene-solve progress +
 * real-time workpool stats).
 *
 * ### Indicators monitored by MonolithicSolver
 *
 * The MonolithicSolver adds the following keys to its JSON status file:
 *  - `"total_scenes"`: total number of scenes to process.
 *  - `"scenes_done"`:  scenes solved so far (thread-safe counter).
 *  - `"scene_times"`:  wall-clock time in seconds for each completed scene.
 *  - `"elapsed_s"`:    total wall time since solve() was called.
 *  - `"status"`:       `"running"` while in progress, `"done"` on completion.
 *  - `"realtime"`:     rolling CPU-load and active-worker history
 *                      (same format as the SDDP status file).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtopt/work_pool.hpp>

#ifdef __linux__
#  include <pthread.h>
#endif

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Work pool factory ───────────────────────────────────────────────────────

/**
 * @brief Create and start an AdaptiveWorkPool configured for solver use.
 *
 * Both MonolithicSolver and SDDPSolver (auxiliary pool) use this factory.
 *
 * @param cpu_factor  Over-commit factor applied to hardware_concurrency.
 *                    Default 1.25 (25 % more threads than physical cores).
 * @return A started AdaptiveWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<AdaptiveWorkPool> make_solver_work_pool(
    double cpu_factor = 1.25)
{
  WorkPoolConfig pool_config {};
  pool_config.max_threads = static_cast<int>(
      std::lround(cpu_factor * std::thread::hardware_concurrency()));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  auto pool = std::make_unique<AdaptiveWorkPool>(pool_config);
  pool->start();
  SPDLOG_TRACE("Solver work pool started: max_threads={} cpu_threshold={:.0f}%",
               pool_config.max_threads,
               pool_config.max_cpu_threshold);
  return pool;
}

/**
 * @brief Create and start an SDDPWorkPool configured for the SDDP solver.
 *
 * Uses `SDDPTaskKey` (tuple) as the secondary priority key so that the
 * SDDP forward/backward LP solves are ordered by
 * (iteration, is_backward, phase, is_nonlp) with the default
 * `std::less<SDDPTaskKey>` comparator (smaller tuple → higher priority).
 *
 * @param cpu_factor  Over-commit factor applied to hardware_concurrency.
 *                    Default 1.25.
 * @return A started SDDPWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<SDDPWorkPool> make_sddp_work_pool(
    double cpu_factor = 1.25)
{
  WorkPoolConfig pool_config {};
  pool_config.max_threads = static_cast<int>(
      std::lround(cpu_factor * std::thread::hardware_concurrency()));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  auto pool = std::make_unique<SDDPWorkPool>(pool_config);
  pool->start();
  SPDLOG_TRACE("SDDP work pool started: max_threads={} cpu_threshold={:.0f}%",
               pool_config.max_threads,
               pool_config.max_cpu_threshold);
  return pool;
}

// ─── MonitorPoint ────────────────────────────────────────────────────────────

/// A single real-time sample point (CPU load, active workers, timestamp).
struct MonitorPoint
{
  double timestamp {};  ///< Seconds since monitoring started
  double cpu_load {};  ///< CPU load percentage [0–100]
  int active_workers {};  ///< Number of active worker threads
};

// ─── SolverMonitor ───────────────────────────────────────────────────────────

/**
 * @class SolverMonitor
 * @brief Background thread that samples workpool statistics and writes JSON.
 *
 * Start by calling `start(pool, thread_name)`.  Stop by calling `stop()` or
 * letting the object go out of scope (RAII — the `std::jthread` destructor
 * automatically requests a stop and joins).
 *
 * The history is collected in `m_history_` under `m_mutex_`.  Call
 * `append_history_json(json)` to emit the `"realtime"` JSON block, and
 * `write_status(content, path)` for atomic file writes.
 */
class SolverMonitor
{
public:
  /// Update interval for the background sampling thread.
  using Interval = std::chrono::milliseconds;

  explicit SolverMonitor(Interval update_interval = Interval {500}) noexcept
      : m_update_interval_(update_interval)
  {
  }

  // Not copyable; not movable (holds a mutex and jthread).
  SolverMonitor(const SolverMonitor&) = delete;
  SolverMonitor& operator=(const SolverMonitor&) = delete;
  SolverMonitor(SolverMonitor&&) = delete;
  SolverMonitor& operator=(SolverMonitor&&) = delete;

  ~SolverMonitor() = default;

  /// Start the background sampling thread.
  /// @tparam Pool        Any work pool type that provides `get_statistics()`.
  /// @param pool         The work pool to sample statistics from.
  /// @param start_time   Reference time-point for timestamp computation.
  /// @param thread_name  Name to assign the background thread (Linux only).
  template<typename Pool>
  void start(Pool& pool,
             std::chrono::steady_clock::time_point start_time,
             [[maybe_unused]] const char* thread_name = "SolverMonitor")
  {
    {
      const std::scoped_lock lock(m_mutex_);
      m_history_.clear();
    }
    m_thread_ = std::jthread {
        [this, &pool, start_time, thread_name](const std::stop_token& stoken)
        {
#ifdef __linux__
          pthread_setname_np(pthread_self(), thread_name);
#endif
          while (!stoken.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double>(now - start_time).count();

            const auto stats = pool.get_statistics();
            {
              const std::scoped_lock lck(m_mutex_);
              m_history_.push_back(MonitorPoint {
                  .timestamp = elapsed,
                  .cpu_load = stats.current_cpu_load,
                  .active_workers = stats.active_threads,
              });
            }
            std::this_thread::sleep_for(m_update_interval_);
          }
        }};
  }

  /// Request the background thread to stop (non-blocking).
  void stop() noexcept { m_thread_.request_stop(); }

  /// Access the collected history (caller must hold no other locks).
  [[nodiscard]] std::vector<MonitorPoint> history() const
  {
    const std::scoped_lock lock(m_mutex_);
    return m_history_;
  }

  /// Append the `"realtime"` JSON block to `json` using collected history.
  void append_history_json(std::string& json) const
  {
    const std::scoped_lock lock(m_mutex_);

    json += "  \"realtime\": {\n";

    json += "    \"timestamps\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{:.3f}", m_history_[i].timestamp);
    }
    json += "],\n";

    json += "    \"cpu_loads\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{:.1f}", m_history_[i].cpu_load);
    }
    json += "],\n";

    json += "    \"active_workers\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{}", m_history_[i].active_workers);
    }
    json += "]\n";
    json += "  }\n";
  }

  /// Write content atomically to path (write tmp, rename).
  static void write_status(const std::string& content,
                           const std::string& path) noexcept
  {
    if (path.empty()) {
      return;
    }
    const auto tmp = path + ".tmp";
    try {
      namespace fs = std::filesystem;
      fs::create_directories(fs::path(path).parent_path());
      {
        std::ofstream out(tmp);
        out << content;
      }
      fs::rename(tmp, path);
    } catch (const std::exception& e) {
      SPDLOG_WARN("SolverMonitor: could not write {}: {}", path, e.what());
    }
  }

private:
  Interval m_update_interval_;
  std::jthread m_thread_;
  mutable std::mutex m_mutex_;
  std::vector<MonitorPoint> m_history_;
};

}  // namespace gtopt
