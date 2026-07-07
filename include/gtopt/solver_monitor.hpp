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
 * The class is used by both `SDDPMethod` (SDDP iteration history +
 * real-time workpool stats) and `MonolithicMethod` (scene-solve progress +
 * real-time workpool stats).
 *
 * ### Indicators monitored by MonolithicMethod
 *
 * The MonolithicMethod adds the following keys to its JSON status file:
 *  - `"total_scenes"`: total number of scenes to process.
 *  - `"scenes_done"`:  scenes solved so far (thread-safe counter).
 *  - `"scene_times"`:  wall-clock time in seconds for each completed scene.
 *  - `"elapsed_s"`:    total wall time since solve() was called.
 *  - `"status"`:       `"running"` while in progress, `"done"` on completion.
 *  - `"realtime"`:     rolling CPU-load and active-worker history
 *                      (same format as the SDDP status file).
 */

#pragma once

#include <algorithm>
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

#include <gtopt/hardware_info.hpp>
#include <gtopt/sddp_pool.hpp>
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
 * Both MonolithicMethod and SDDPMethod (auxiliary pool) use this factory.
 *
 * @param cpu_factor  Over-commit factor applied to hardware_concurrency.
 *                    Default 2.0 — extra threads compensate for mutex
 *                    contention during LP clone operations.
 * @param cpu_threshold_override  If provided and > 0, use this value as
 *                    the pool's CPU-load dispatch gate instead of the
 *                    default `100 - 50/max_threads` formula.  Useful
 *                    for I/O-bound workloads (e.g. parquet encode) where
 *                    the formula's thread-count-driven cap is too
 *                    conservative.
 * @return A started AdaptiveWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<AdaptiveWorkPool> make_solver_work_pool(
    double cpu_factor = 2.0,
    double cpu_threshold_override = 0.0,
    std::chrono::milliseconds scheduler_interval =
        std::chrono::milliseconds(50),
    double memory_limit_mb = 0.0,
    std::string_view pool_label = "SolverWorkPool")
{
  WorkPoolConfig pool_config {};
  pool_config.name = std::string {pool_label};
  // Use physical cores as the base — hyperthreads add little for
  // compute-bound LP solves and inflate the thread count.  Worker-thread
  // count is DECOUPLED from any memory limit (see `memory_clamp_threads`):
  // the pool always runs at the `cpu_factor × cores` ceiling, and a
  // memory limit is enforced by the live measured-memory DISPATCH gate
  // (`BasicWorkPool::can_dispatch_task`), not by clamping workers.
  const auto clamp =
      memory_clamp_threads(cpu_factor, memory_limit_mb, pool_config.name);
  pool_config.max_threads = clamp.initial_threads;
  pool_config.max_threads_ceiling = clamp.ceiling_threads;
  // CPU saturation threshold is computed from the growth ceiling (the
  // eventual thread count), not the clamped start, so a memory-clamped
  // pool that later grows is not left with an over-tight CPU gate.
  pool_config.max_cpu_threshold = (cpu_threshold_override > 0.0)
      ? cpu_threshold_override
      : static_cast<int>(100.0
                         - (50.0 / static_cast<double>(clamp.ceiling_threads)));
  pool_config.max_process_rss_mb = memory_limit_mb;
  pool_config.scheduler_interval = scheduler_interval;
  pool_config.enable_periodic_stats = false;

  auto pool = std::make_unique<AdaptiveWorkPool>(pool_config);
  pool->start();
  SPDLOG_TRACE(
      "Solver work pool started: max_threads={} cpu_threshold={:.0f}% "
      "(physical_cores={} logical_cores={})",
      pool_config.max_threads,
      pool_config.max_cpu_threshold,
      physical_concurrency(),
      std::thread::hardware_concurrency());
  return pool;
}

// ─── MonitorPoint ────────────────────────────────────────────────────────────

/// A single real-time sample point (CPU load, memory, active workers,
/// timestamp).
struct MonitorPoint
{
  double timestamp {};  ///< Seconds since monitoring started
  double cpu_load {};  ///< CPU load percentage [0–100]
  int active_workers {};  ///< Number of active worker threads
  double memory_percent {};  ///< System memory usage percentage [0–100]
  double process_rss_mb {};  ///< Process RSS in MB
  double available_memory_mb {};  ///< System available (free) memory in MB
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

  /// Maximum number of monitor points to retain.
  /// At 500ms intervals, 7200 points ≈ 1 hour of data.
  static constexpr std::size_t max_history_size {7200};

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
              if (m_history_.size() >= max_history_size) {
                // Drop oldest 25%, keep newest 75%
                const auto keep = max_history_size * 3 / 4;
                m_history_.erase(m_history_.begin(),
                                 m_history_.begin()
                                     + static_cast<std::ptrdiff_t>(
                                         m_history_.size() - keep));
              }
              m_history_.push_back(MonitorPoint {
                  .timestamp = elapsed,
                  .cpu_load = stats.current_cpu_load,
                  .active_workers = stats.active_threads,
                  .memory_percent = stats.current_memory_percent,
                  .process_rss_mb = stats.process_rss_mb,
                  .available_memory_mb = stats.available_memory_mb,
              });
            }
            // Rewrite the status file every tick with the cached iteration
            // data + this fresh ring sample, so the realtime CPU/MEM/Workers
            // numbers refresh on the sampling cadence instead of only once
            // per (multi-second) iteration.  Sole periodic writer.
            do_periodic_write();
            std::this_thread::sleep_for(m_update_interval_);
          }
        },
    };
  }

  /// Request the background thread to stop (non-blocking).
  void stop() noexcept { m_thread_.request_stop(); }

  /// Access the collected history (caller must hold no other locks).
  [[nodiscard]] std::vector<MonitorPoint> history() const
  {
    const std::scoped_lock lock(m_mutex_);
    return m_history_;
  }

  /// Append the fresh pool/realtime block (and the document-closing `}`)
  /// to a partial iteration JSON document.
  ///
  /// Emits the top-level `pool_memory_percent` / `pool_process_rss_mb` /
  /// `pool_available_memory_mb` scalars from the most recent sampled
  /// ring point (so they stay live between iterations), then the
  /// `"realtime"` history block, then closes the JSON object.
  ///
  /// `json` is expected to be the partial document produced by
  /// `build_iteration_status_json()` (begins with `{`, no trailing `}`).
  void build_realtime_status_json(std::string& json) const
  {
    const std::scoped_lock lock(m_mutex_);

    // ── Fresh pool scalars (latest sampled ring point) ──
    double mem_pct = 0.0;
    double rss_mb = 0.0;
    double avail_mb = 0.0;
    if (!m_history_.empty()) {
      const auto& last = m_history_.back();
      mem_pct = last.memory_percent;
      rss_mb = last.process_rss_mb;
      avail_mb = last.available_memory_mb;
    }
    json += std::format("  \"pool_memory_percent\": {:.1f},\n", mem_pct);
    json += std::format("  \"pool_process_rss_mb\": {:.0f},\n", rss_mb);
    json += std::format("  \"pool_available_memory_mb\": {:.0f},\n", avail_mb);

    append_history_json_locked(json);

    json += "}\n";
  }

  /// Append the `"realtime"` JSON block to `json` using collected history.
  void append_history_json(std::string& json) const
  {
    const std::scoped_lock lock(m_mutex_);
    append_history_json_locked(json);
  }

  /// Register the cached iteration-portion JSON and target path so the
  /// background sampling thread rewrites the status file every tick with
  /// FRESH realtime/pool numbers (and the last iteration's unchanged
  /// iteration data).  Called on the solver thread once per iteration —
  /// cheap (a guarded string move).
  void update_status(std::string iteration_json, std::string path)
  {
    const std::scoped_lock lock(m_status_mutex_);
    m_iteration_json_ = std::move(iteration_json);
    m_status_path_ = std::move(path);
    m_have_status_.store(true, std::memory_order_release);
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
  /// Append the `"realtime"` JSON block to `json` (caller holds m_mutex_).
  void append_history_json_locked(std::string& json) const
  {
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
    json += "],\n";

    json += "    \"memory_percent\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{:.1f}", m_history_[i].memory_percent);
    }
    json += "],\n";

    json += "    \"process_rss_mb\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{:.0f}", m_history_[i].process_rss_mb);
    }
    json += "],\n";

    json += "    \"available_memory_mb\": [";
    for (std::size_t i = 0; i < m_history_.size(); ++i) {
      if (i > 0) {
        json += ", ";
      }
      json += std::format("{:.0f}", m_history_[i].available_memory_mb);
    }
    json += "]\n";
    json += "  }\n";
  }

  /// Rewrite the status file with the cached iteration JSON plus a freshly
  /// built pool/realtime block.  No-op until the solver thread has called
  /// `update_status()` at least once.  Runs on the monitor sampling thread
  /// (the sole periodic writer) — it NEVER touches live solver state, only
  /// the cached prebuilt iteration string.
  void do_periodic_write()
  {
    if (!m_have_status_.load(std::memory_order_acquire)) {
      return;
    }
    std::string doc;
    std::string path;
    {
      const std::scoped_lock lock(m_status_mutex_);
      if (m_status_path_.empty()) {
        return;
      }
      doc = m_iteration_json_;  // copy cached iteration portion
      path = m_status_path_;
    }
    // Append the fresh pool/realtime block + closing brace (locks m_mutex_).
    build_realtime_status_json(doc);
    write_status(doc, path);
  }

  Interval m_update_interval_;
  std::jthread m_thread_;
  mutable std::mutex m_mutex_;
  std::vector<MonitorPoint> m_history_;

  // ── Cached iteration JSON + target path for periodic refresh ──
  // Guarded by m_status_mutex_ (separate from m_mutex_ so the cheap
  // per-iteration setter does not contend with ring sampling).
  mutable std::mutex m_status_mutex_;
  std::string m_iteration_json_;
  std::string m_status_path_;
  std::atomic<bool> m_have_status_ {false};
};

}  // namespace gtopt
