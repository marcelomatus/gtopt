
#include <array>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <ranges>
#include <sstream>

#include <gtopt/cpu_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

void CPUMonitor::start()
{
  if (running_.exchange(true)) {
    return; // Already running
  }

  try {
    std::jthread temp_thread([this](const std::stop_token& stoken) {
      while (!stoken.stop_requested() && running_.load(std::memory_order_relaxed)) {
        try {
          const double load = get_system_cpu_usage();
          if (load >= 0.0 && load <= 100.0) {
            current_load_.store(load, std::memory_order_relaxed);
          } else {
            SPDLOG_WARN("Invalid CPU load value: {}", load);
          }
        } catch (const std::exception& e) {
          SPDLOG_ERROR("Exception in CPU monitoring thread: {} - continuing", e.what());
        } catch (...) {
          SPDLOG_ERROR("Unknown exception in CPU monitoring thread - continuing");
        }
        std::this_thread::sleep_for(monitor_interval_);
      }
    });

    // Verify thread was actually created
    if (!temp_thread.joinable()) {
      throw std::runtime_error("Failed to create monitoring thread");
    }
    monitor_thread_ = std::move(temp_thread);
  } catch (...) {
    running_.store(false); // Rollback state on failure
    throw;
  }
}

void CPUMonitor::stop()
{
  running_.store(false, std::memory_order_relaxed);
  if (monitor_thread_.joinable()) {
    monitor_thread_.request_stop();
    monitor_thread_.join();
  }
}

double CPUMonitor::get_system_cpu_usage(double fallback_value) noexcept
{
  static uint64_t last_idle = 0;
  static uint64_t last_total = 0;

  const std::filesystem::path proc_stat_path("/proc/stat");

  std::string line;
  try {
    if (!std::filesystem::exists(proc_stat_path)) {
      SPDLOG_WARN(fmt::format("{} does not exist, using fallback CPU value: {}",
                              proc_stat_path.string(),
                              fallback_value));
      return fallback_value;
    }

    std::ifstream proc_stat(proc_stat_path);
    if (!proc_stat.is_open()) {
      SPDLOG_WARN("Failed to open /proc/stat");
      return fallback_value;
    }

    if (!std::getline(proc_stat, line)) {
      return fallback_value;
    }
  } catch (const std::filesystem::filesystem_error& e) {
    SPDLOG_WARN("Filesystem error accessing /proc/stat");
    return fallback_value;
  } catch (const std::exception& e) {
    SPDLOG_WARN("Exception while reading /proc/stat");
    return fallback_value;
  }

  std::istringstream ss(std::move(line));
  // skip the 'cpu' string until we reach the first space/number
  ss.ignore(std::numeric_limits<std::streamsize>::max(), ' ');

  std::array<uint64_t, 10> times {};
  auto count = std::ranges::distance(
      std::ranges::copy(std::ranges::istream_view<uint64_t>(ss)
                            | std::views::take(times.size()),
                        times.begin())
          .out,
      times.begin());

  if (count < 4) {
    SPDLOG_WARN("Insufficient CPU stats values read from /proc/stat");
    return fallback_value;
  }

  const auto idle = times[3];
  const auto total =
      std::accumulate(times.begin(), times.begin() + count, 0ULL);

  const auto idle_delta = idle - last_idle;
  const auto total_delta = total - last_total;

  last_idle = idle;
  last_total = total;

  if (total_delta == 0) {
    return 0.0;
  }

  return 100.0
      * (1.0
         - static_cast<double>(idle_delta) / static_cast<double>(total_delta));
}

}  // namespace gtopt
