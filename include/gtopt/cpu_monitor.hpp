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

  void set_interval(std::chrono::milliseconds interval) {
    monitor_interval_ = interval;
  }

  [[nodiscard]] constexpr double get_load() const noexcept
  {
    return current_load_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] constexpr auto get_interval() const noexcept {
    return monitor_interval_;
  }

  static double get_system_cpu_usage(double fallback_value = 50.0);

private:
  std::atomic<double> current_load_ {0.0};
  std::atomic<bool> running_ {false};
  std::chrono::milliseconds monitor_interval_ {100};
  std::jthread monitor_thread_;
};

}  // namespace gtopt
