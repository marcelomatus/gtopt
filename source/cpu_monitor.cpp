
#include <array>
#include <fstream>
#include <numeric>
#include <sstream>

#include <gtopt/cpu_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

void CPUMonitor::start()
{
  running_.store(true, std::memory_order_relaxed);
  monitor_thread_ =
      std::jthread {[this](const std::stop_token& stoken)
                    {
                      while (!stoken.stop_requested()
                             && running_.load(std::memory_order_relaxed))
                      {
                        current_load_.store(get_system_cpu_usage(),
                                            std::memory_order_relaxed);
                        std::this_thread::sleep_for(monitor_interval_);
                      }
                    }};
}

void CPUMonitor::stop()
{
  running_.store(false, std::memory_order_relaxed);
  if (monitor_thread_.joinable()) {
    monitor_thread_.request_stop();
    monitor_thread_.join();
  }
}

double CPUMonitor::get_system_cpu_usage()
{
  static uint64_t last_idle = 0;
  static uint64_t last_total = 0;

  std::ifstream proc_stat("/proc/stat");
  if (!proc_stat) {
    SPDLOG_WARN("Failed to open /proc/stat, using fallback CPU value");
    return 50.0;  // fallback
  }

  std::string line;
  std::getline(proc_stat, line);

  std::istringstream ss(line);
  std::string cpu_name;
  ss >> cpu_name;

  std::vector<uint64_t> times;
  times.reserve(10);
  uint64_t time = 0;
  while (ss >> time) {
    times.push_back(time);
  }

  if (times.size() >= 4) {
    auto idle = times[3];
    auto total = std::accumulate(times.begin(), times.end(), 0ULL);

    auto idle_delta = idle - last_idle;
    auto total_delta = total - last_total;

    last_idle = idle;
    last_total = total;

    if (total_delta > 0) {
      return 100.0
          * (1.0
             - static_cast<double>(idle_delta)
                 / static_cast<double>(total_delta));
    }
  }
  return 0.0;
}

}  // namespace gtopt
