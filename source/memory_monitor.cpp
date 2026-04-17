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

namespace
{

/// Parse a plain integer value from a /proc/vmstat-style line like
/// "pswpin 12345".  Returns 0.0 on parse failure.
double parse_proc_count_line(std::string_view line) noexcept
{
  const auto sp = line.find(' ');
  if (sp == std::string_view::npos) {
    return 0.0;
  }
  auto data = line.substr(sp + 1);
  while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) {
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
  return static_cast<double>(val);
}

}  // namespace

MemorySnapshot MemoryMonitor::get_system_memory_snapshot(
    double fallback_total_mb) noexcept
{
  MemorySnapshot snap {
      .total_mb = fallback_total_mb,
      .available_mb = fallback_total_mb * 0.5,
      .process_rss_mb = 0.0,
      .process_swap_mb = 0.0,
      .swap_total_mb = 0.0,
      .swap_used_mb = 0.0,
      .vmstat_pswpin = 0.0,
      .vmstat_pswpout = 0.0,
  };

  try {
    // ── System memory + swap totals from /proc/meminfo ──
    constexpr std::string_view meminfo_path = "/proc/meminfo";
    if (std::filesystem::exists(meminfo_path)) {
      std::ifstream meminfo {std::string(meminfo_path)};
      std::string line;
      bool got_total = false;
      bool got_available = false;
      bool got_swap_total = false;
      double swap_free_mb = 0.0;
      bool got_swap_free = false;

      while (std::getline(meminfo, line)) {
        const auto sv = std::string_view(line);
        if (!got_total && sv.starts_with("MemTotal:")) {
          snap.total_mb = parse_proc_kb_line(sv);
          got_total = true;
        } else if (!got_available && sv.starts_with("MemAvailable:")) {
          snap.available_mb = parse_proc_kb_line(sv);
          got_available = true;
        } else if (!got_swap_total && sv.starts_with("SwapTotal:")) {
          snap.swap_total_mb = parse_proc_kb_line(sv);
          got_swap_total = true;
        } else if (!got_swap_free && sv.starts_with("SwapFree:")) {
          swap_free_mb = parse_proc_kb_line(sv);
          got_swap_free = true;
        }
        if (got_total && got_available && got_swap_total && got_swap_free) {
          break;
        }
      }
      if (got_swap_total && got_swap_free) {
        snap.swap_used_mb = snap.swap_total_mb > swap_free_mb
            ? snap.swap_total_mb - swap_free_mb
            : 0.0;
      }
    }

    // ── Process RSS + VmSwap from /proc/self/status ──
    constexpr std::string_view status_path = "/proc/self/status";
    if (std::filesystem::exists(status_path)) {
      std::ifstream status {std::string(status_path)};
      std::string line;
      bool got_rss = false;
      bool got_swap = false;
      while (std::getline(status, line)) {
        const auto sv = std::string_view(line);
        if (!got_rss && sv.starts_with("VmRSS:")) {
          snap.process_rss_mb = parse_proc_kb_line(sv);
          got_rss = true;
        } else if (!got_swap && sv.starts_with("VmSwap:")) {
          snap.process_swap_mb = parse_proc_kb_line(sv);
          got_swap = true;
        }
        if (got_rss && got_swap) {
          break;
        }
      }
    }

    // ── System-wide swap I/O counters from /proc/vmstat ──
    // pswpin / pswpout are cumulative page counts since boot; the caller
    // derives a per-second rate by diffing two snapshots.
    constexpr std::string_view vmstat_path = "/proc/vmstat";
    if (std::filesystem::exists(vmstat_path)) {
      std::ifstream vmstat {std::string(vmstat_path)};
      std::string line;
      bool got_pin = false;
      bool got_pout = false;
      while (std::getline(vmstat, line)) {
        const auto sv = std::string_view(line);
        if (!got_pin && sv.starts_with("pswpin ")) {
          snap.vmstat_pswpin = parse_proc_count_line(sv);
          got_pin = true;
        } else if (!got_pout && sv.starts_with("pswpout ")) {
          snap.vmstat_pswpout = parse_proc_count_line(sv);
          got_pout = true;
        }
        if (got_pin && got_pout) {
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
    process_swap_mb_.store(snap.process_swap_mb, std::memory_order_relaxed);
    swap_used_mb_.store(snap.swap_used_mb, std::memory_order_relaxed);
    swap_io_rate_.store(0.0, std::memory_order_relaxed);

    monitor_thread_ = std::jthread(
        [this, initial = snap](const std::stop_token& stoken)
        {
          auto prev_pages = initial.vmstat_pswpin + initial.vmstat_pswpout;
          auto prev_time = std::chrono::steady_clock::now();

          while (!stoken.stop_requested()) {
            const auto snap = get_system_memory_snapshot();
            total_mb_.store(snap.total_mb, std::memory_order_relaxed);
            available_mb_.store(snap.available_mb, std::memory_order_relaxed);
            process_rss_mb_.store(snap.process_rss_mb,
                                  std::memory_order_relaxed);
            process_swap_mb_.store(snap.process_swap_mb,
                                   std::memory_order_relaxed);
            swap_used_mb_.store(snap.swap_used_mb, std::memory_order_relaxed);

            // Rolling delta → per-second swap I/O rate.
            const auto now = std::chrono::steady_clock::now();
            const auto dt =
                std::chrono::duration<double>(now - prev_time).count();
            const auto pages = snap.vmstat_pswpin + snap.vmstat_pswpout;
            if (dt > 0.0 && pages >= prev_pages) {
              swap_io_rate_.store((pages - prev_pages) / dt,
                                  std::memory_order_relaxed);
            }
            prev_pages = pages;
            prev_time = now;

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
