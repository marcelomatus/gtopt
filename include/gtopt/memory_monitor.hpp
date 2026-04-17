/**
 * @file      memory_monitor.hpp
 * @brief     System and process memory monitoring
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides real-time memory monitoring capabilities:
 * - System-wide memory utilization from /proc/meminfo
 * - Process RSS from /proc/self/status
 * - Thread-safe atomic reads for lock-free access
 * - RAII lifecycle with std::jthread
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace gtopt
{

/// Snapshot of memory state at a point in time.
struct MemorySnapshot
{
  double total_mb {};  ///< Total system memory in MB
  double available_mb {};  ///< Available system memory in MB
  double process_rss_mb {};  ///< Process resident set size in MB
  double process_swap_mb {};  ///< Process VmSwap in MB (pages on swap)
  double swap_total_mb {};  ///< System SwapTotal in MB
  double swap_used_mb {};  ///< System swap in use (SwapTotal-SwapFree) in MB
  /// Cumulative counts of swap-in/out pages since boot.  Use the delta
  /// between two snapshots divided by the elapsed time to estimate the
  /// per-second swap I/O rate (thrash indicator).
  double vmstat_pswpin {};
  double vmstat_pswpout {};
};

class MemoryMonitor
{
public:
  MemoryMonitor() = default;
  MemoryMonitor(const MemoryMonitor&) = delete;
  MemoryMonitor& operator=(const MemoryMonitor&) = delete;
  MemoryMonitor(MemoryMonitor&&) = delete;
  MemoryMonitor& operator=(MemoryMonitor&&) = delete;

  ~MemoryMonitor() { stop(); }

  void start();
  void stop() noexcept;

  void set_interval(std::chrono::milliseconds interval) noexcept
  {
    monitor_interval_ = interval;
  }

  /// System available memory in MB.
  [[nodiscard]] constexpr double get_available_mb() const noexcept
  {
    return available_mb_.load(std::memory_order_relaxed);
  }

  /// Total system memory in MB.
  [[nodiscard]] constexpr double get_total_mb() const noexcept
  {
    return total_mb_.load(std::memory_order_relaxed);
  }

  /// System memory usage as percentage (0–100).
  [[nodiscard]] constexpr double get_memory_percent() const noexcept
  {
    const auto total = total_mb_.load(std::memory_order_relaxed);
    const auto avail = available_mb_.load(std::memory_order_relaxed);
    return total > 0.0 ? 100.0 * (total - avail) / total : 0.0;
  }

  /// Process RSS in MB.
  [[nodiscard]] constexpr double get_process_rss_mb() const noexcept
  {
    return process_rss_mb_.load(std::memory_order_relaxed);
  }

  /// Process bytes paged out to swap (VmSwap from /proc/self/status) in MB.
  [[nodiscard]] constexpr double get_process_swap_mb() const noexcept
  {
    return process_swap_mb_.load(std::memory_order_relaxed);
  }

  /// System-wide swap usage in MB (SwapTotal - SwapFree).
  [[nodiscard]] constexpr double get_swap_used_mb() const noexcept
  {
    return swap_used_mb_.load(std::memory_order_relaxed);
  }

  /// Observed swap I/O rate (pages in + pages out per second), averaged
  /// over the previous monitor interval.  Near zero under normal
  /// operation; rises sharply when the kernel is thrashing pages to or
  /// from swap.
  [[nodiscard]] constexpr double get_swap_io_rate() const noexcept
  {
    return swap_io_rate_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] constexpr auto get_interval() const noexcept
  {
    return monitor_interval_;
  }

  /// Read current memory state from /proc (static, no background thread).
  static MemorySnapshot get_system_memory_snapshot(
      double fallback_total_mb = 4096.0) noexcept;

private:
  std::atomic<double> total_mb_ {0.0};
  std::atomic<double> available_mb_ {0.0};
  std::atomic<double> process_rss_mb_ {0.0};
  std::atomic<double> process_swap_mb_ {0.0};
  std::atomic<double> swap_used_mb_ {0.0};
  std::atomic<double> swap_io_rate_ {0.0};
  std::atomic<bool> running_ {false};
  std::chrono::milliseconds monitor_interval_ {500};
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  std::jthread monitor_thread_;
};

}  // namespace gtopt
