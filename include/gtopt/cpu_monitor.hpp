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

  [[nodiscard]] constexpr double get_load() const noexcept
  {
    return current_load_.load(std::memory_order_relaxed);
  }

  static double get_system_cpu_usage();

private:
  std::atomic<double> current_load_ {0.0};
  std::atomic<bool> running_ {false};
  std::jthread monitor_thread_;
};

}  // namespace gtopt
