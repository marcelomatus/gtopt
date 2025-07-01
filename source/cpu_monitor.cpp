/**
 * @file      cpu_monitor.cpp
 * @brief     CPU monitoring implementation with C++23 optimizations
 */

#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <ranges>
#include <system_error>

#include <gtopt/cpu_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

double CPUMonitor::get_system_cpu_usage(double fallback_value) noexcept
{
  static std::atomic<uint64_t> last_idle = 0;
  static std::atomic<uint64_t> last_total = 0;
  static std::atomic<size_t> call_count = 0;

  constexpr std::string_view proc_stat_path = "/proc/stat";
  constexpr size_t min_stats = 4;  // Minimum required CPU stats

  try {
    // Fast existence check
    if (!std::filesystem::exists(proc_stat_path)) [[unlikely]] {
      const auto msg =
          fmt::format("{} does not exist, using fallback CPU value: {:.2f}",
                      proc_stat_path,
                      fallback_value);
      SPDLOG_WARN(msg);
      return fallback_value;
    }

    // Open /proc/stat in text mode (default)
    std::ifstream proc_stat {std::string(proc_stat_path)};
    if (!proc_stat.is_open()) [[unlikely]] {
      const auto msg = fmt::format("Failed to open {}", proc_stat_path);
      SPDLOG_WARN(msg);
      return fallback_value;
    }

    // Read first line efficiently
    std::string line;
    if (!std::getline(proc_stat, line)) [[unlikely]] {
      return fallback_value;
    }

    // Skip "cpu" prefix and any following whitespace
    auto cpu_data = std::string_view(line).substr(3);
    cpu_data = cpu_data.substr(cpu_data.find_first_not_of(' '));

    std::array<uint64_t, 10> times {};

    // Parse numbers directly without stringstream overhead
    auto parse_view = cpu_data | std::views::split(' ')
        | std::views::transform(
                          [](auto&& r)
                          {
                            uint64_t val = 0;
                            std::from_chars(r.begin(), r.end(), val);
                            return val;
                          })
        | std::views::take(times.size());

    // Copy parsed values into times array and get count
    auto [in, out] = std::ranges::copy(parse_view, times.begin());
    const size_t count = std::distance(times.begin(), out);

    if (count < min_stats) [[unlikely]] {
      SPDLOG_WARN(
          fmt::format("Insufficient CPU stats, only {} values read", count));
      return fallback_value;
    }

    const auto idle = times[3];
    const auto total =
        std::accumulate(times.begin(), times.begin() + count, 0ULL);

    // Atomic updates
    const auto idle_delta = idle - last_idle.exchange(idle);
    const auto total_delta = total - last_total.exchange(total);

    if (total_delta == 0) [[unlikely]] {
      return 0.0;
    }

    // Fast floating-point conversion
    const double load = 100.0
        * (1.0
           - static_cast<double>(idle_delta)
               / static_cast<double>(total_delta));

    // Log every 10th call (thread-safe counter)
    if (call_count.fetch_add(1, std::memory_order_relaxed) % 10 == 0) {
      SPDLOG_INFO(
          fmt::format("CPU load: {:.2f}% (idle_delta: {}, total_delta: {})",
                      load,
                      idle_delta,
                      total_delta));
    }

    return load;
  } catch (...) {
    return fallback_value;
  }
}

void CPUMonitor::start()
{
  if (running_.exchange(true)) [[unlikely]] {
    return;  // Already running
  }

  try {
    monitor_thread_ = std::jthread(
        [this](const std::stop_token& stoken)
        {
          while (!stoken.stop_requested()) {
            const double load = get_system_cpu_usage();
            current_load_.store(load, std::memory_order_relaxed);

            // Using C++20's jthread stop token for interruption
            std::this_thread::sleep_for(monitor_interval_);
            if (stoken.stop_requested()) {
              break;
            }
          }
        });

    if (!monitor_thread_.joinable()) [[unlikely]] {
      throw std::runtime_error("Failed to create monitoring thread");
    }

  } catch (...) {
    running_.store(false);
    SPDLOG_ERROR("Exception in CPU monitoring startup");
    throw;
  }
}

void CPUMonitor::stop() noexcept
{
  running_.store(false, std::memory_order_relaxed);
  if (monitor_thread_.joinable()) {
    monitor_thread_.request_stop();
    monitor_thread_.join();
  }
}

}  // namespace gtopt
