/**
 * @file      cpu_monitor.hpp
 * @brief     CPU usage monitoring and statistics collection
 * @date      Wed Jun 25 21:33:13 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides real-time CPU usage monitoring capabilities:
 * - System-wide CPU utilization tracking
 * - Thread-safe load measurement
 * - Configurable sampling interval
 * - Fallback mechanisms for robustness
 * - Efficient /proc/stat parsing
 *
 * Key Features:
 * - Lightweight monitoring with minimal overhead
 * - Accurate CPU percentage calculation
 * - RAII-style lifecycle management
 * - Exception-safe implementation
 * - Suitable for adaptive workload scheduling
 */

#pragma once

#include <atomic>
#include <thread>

namespace gtopt
{

class CPUMonitor
{
public:
  CPUMonitor() = default;
  CPUMonitor(const CPUMonitor&) = delete;
  CPUMonitor& operator=(const CPUMonitor&) = delete;
  CPUMonitor(CPUMonitor&&) = delete;
  CPUMonitor& operator=(CPUMonitor&&) = delete;

  ~CPUMonitor() { stop(); }

  void start();
  void stop();

  void set_interval(std::chrono::milliseconds interval) noexcept
  {
    monitor_interval_ = interval;
  }

  /**
   * @brief Gets current CPU load percentage
   * @return Value between 0.0 and 100.0, or negative if invalid
   * @note Provides noexcept guarantee
   */
  [[nodiscard]] constexpr double get_load() const noexcept
  {
    return current_load_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] constexpr auto get_interval() const noexcept
  {
    return monitor_interval_;
  }

  static double get_system_cpu_usage(double fallback_value = 50.0) noexcept;

private:
  std::atomic<double> current_load_ {0.0};
  std::atomic<bool> running_ {false};
  std::chrono::milliseconds monitor_interval_ {100};
  std::jthread monitor_thread_;
};

}  // namespace gtopt
