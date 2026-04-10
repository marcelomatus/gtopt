/**
 * @file      memory_monitor.cpp
 * @brief     Memory monitoring implementation via /proc filesystem
 */

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtopt/memory_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Parse a numeric value in kB from a /proc line like "MemTotal:  12345 kB".
/// Skips the label, then any whitespace, then parses the first integer.
/// Returns 0.0 on parse failure.
double parse_proc_kb_line(std::string_view line) noexcept
{
  // Skip label (everything up to and including ':')
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) {
    return 0.0;
  }
  auto data = line.substr(colon + 1);

  // Skip any leading whitespace — /proc/meminfo uses spaces,
  // /proc/self/status uses tabs, other /proc files may mix both.
  while (!data.empty()
         && (data.front() == ' ' || data.front() == '\t' || data.front() == '\n'
             || data.front() == '\r'))
  {
    data.remove_prefix(1);
  }
  if (data.empty()) {
    return 0.0;
  }

  uint64_t val = 0;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] =
      std::from_chars(data.data(), data.data() + data.size(), val);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (ec != std::errc {}) {
    return 0.0;
  }
  return static_cast<double>(val) / 1024.0;  // kB → MB
}

}  // namespace

MemorySnapshot MemoryMonitor::get_system_memory_snapshot(
    double fallback_total_mb) noexcept
{
  MemorySnapshot snap {
      .total_mb = fallback_total_mb,
      .available_mb = fallback_total_mb * 0.5,
      .process_rss_mb = 0.0,
  };

  try {
    // ── System memory from /proc/meminfo ──
    constexpr std::string_view meminfo_path = "/proc/meminfo";
    if (std::filesystem::exists(meminfo_path)) {
      std::ifstream meminfo {std::string(meminfo_path)};
      std::string line;
      bool got_total = false;
      bool got_available = false;

      while (std::getline(meminfo, line)) {
        const auto sv = std::string_view(line);
        if (!got_total && sv.starts_with("MemTotal:")) {
          snap.total_mb = parse_proc_kb_line(sv);
          got_total = true;
        } else if (!got_available && sv.starts_with("MemAvailable:")) {
          snap.available_mb = parse_proc_kb_line(sv);
          got_available = true;
        }
        if (got_total && got_available) {
          break;
        }
      }
    }

    // ── Process RSS from /proc/self/status ──
    constexpr std::string_view status_path = "/proc/self/status";
    if (std::filesystem::exists(status_path)) {
      std::ifstream status {std::string(status_path)};
      std::string line;
      while (std::getline(status, line)) {
        const auto sv = std::string_view(line);
        if (sv.starts_with("VmRSS:")) {
          snap.process_rss_mb = parse_proc_kb_line(sv);
          break;
        }
      }
    }
  } catch (const std::exception& e) {
    SPDLOG_WARN("MemoryMonitor: /proc parse error: {}", e.what());
  }

  return snap;
}

void MemoryMonitor::start()
{
  if (running_.exchange(true)) [[unlikely]] {
    return;
  }

  try {
    // Seed initial values
    const auto snap = get_system_memory_snapshot();
    total_mb_.store(snap.total_mb, std::memory_order_relaxed);
    available_mb_.store(snap.available_mb, std::memory_order_relaxed);
    process_rss_mb_.store(snap.process_rss_mb, std::memory_order_relaxed);

    monitor_thread_ = std::jthread(
        [this](const std::stop_token& stoken)
        {
          while (!stoken.stop_requested()) {
            const auto snap = get_system_memory_snapshot();
            total_mb_.store(snap.total_mb, std::memory_order_relaxed);
            available_mb_.store(snap.available_mb, std::memory_order_relaxed);
            process_rss_mb_.store(snap.process_rss_mb,
                                  std::memory_order_relaxed);

            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait_for(lock,
                              monitor_interval_,
                              [&] { return stoken.stop_requested(); });
          }
        });

    if (!monitor_thread_.joinable()) [[unlikely]] {
      throw std::runtime_error("Failed to create memory monitoring thread");
    }
  } catch (...) {
    running_.store(false);
    SPDLOG_ERROR("Exception in memory monitoring startup");
    throw;
  }
}

void MemoryMonitor::stop() noexcept
{
  running_.store(false, std::memory_order_relaxed);
  if (monitor_thread_.joinable()) {
    monitor_thread_.request_stop();
    stop_cv_.notify_all();
    monitor_thread_.join();
  }
}

}  // namespace gtopt
