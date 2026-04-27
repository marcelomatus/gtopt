/**
 * @file      hardware_info.cpp
 * @brief     Physical core detection via Linux sysfs topology
 */

#include <atomic>
#include <cmath>

#include <gtopt/hardware_info.hpp>

#ifdef __linux__
#  include <charconv>
#  include <filesystem>
#  include <fstream>
#  include <set>
#  include <string>
#  include <utility>

#  include <spdlog/spdlog.h>
#endif

namespace gtopt
{

namespace
{

/// Process-global CPU quota state.  Two atomics so a reader can fetch
/// both without locking.  `g_quota_cores` of 0 means "no clamp" and is
/// the default for backward compatibility.  `g_quota_pct` is kept only
/// for diagnostics / `get_cpu_quota_pct()` reporting.
///
/// These are necessarily mutable global state: the CLI sets the quota
/// once at startup and every subsequent call to `physical_concurrency()`
/// must observe the same clamp.  Encapsulating them in an instance
/// would require threading a handle through every pool factory and the
/// dozens of call sites that already rely on the free-function form.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned> g_quota_cores {0};
std::atomic<double> g_quota_pct {0.0};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Probe the kernel topology files once.  Cached as a function-local
/// static so repeated calls are free.  Result is independent of any
/// CPU quota.
[[nodiscard]] unsigned probe_physical_concurrency() noexcept
{
#ifdef __linux__
  try {
    namespace fs = std::filesystem;

    // Count unique (package_id, core_id) pairs across all online CPUs.
    std::set<std::pair<int, int>> physical_cores;

    constexpr std::string_view cpu_base = "/sys/devices/system/cpu";

    for (const auto& entry : fs::directory_iterator(cpu_base)) {
      const auto name = entry.path().filename().string();

      // Match cpu0, cpu1, ... (skip cpufreq, cpuidle, etc.)
      if (name.size() < 4 || name[0] != 'c' || name[1] != 'p' || name[2] != 'u'
          || name[3] < '0' || name[3] > '9')
      {
        continue;
      }

      const auto topo_dir = entry.path() / "topology";
      const auto pkg_path = topo_dir / "physical_package_id";
      const auto core_path = topo_dir / "core_id";

      if (!fs::exists(pkg_path) || !fs::exists(core_path)) {
        continue;
      }

      auto read_int = [](const fs::path& path) -> int
      {
        std::ifstream ifs(path);
        std::string buf;
        if (!std::getline(ifs, buf) || buf.empty()) {
          return -1;
        }
        int val = -1;
        // `std::from_chars` requires raw pointer bounds; pointer
        // arithmetic is inherent to its contract.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::from_chars(buf.data(), buf.data() + buf.size(), val);
        return val;
      };

      const int pkg_id = read_int(pkg_path);
      const int core_id = read_int(core_path);

      if (pkg_id >= 0 && core_id >= 0) {
        physical_cores.emplace(pkg_id, core_id);
      }
    }

    if (!physical_cores.empty()) {
      const auto count = static_cast<unsigned>(physical_cores.size());
      SPDLOG_DEBUG(
          "Detected {} physical cores, {} logical cores (SMT ratio {})",
          count,
          std::thread::hardware_concurrency(),
          std::thread::hardware_concurrency() / count);
      return count;
    }
  } catch (const std::exception& ex) {
    // Any sysfs read / parse failure falls back to
    // `std::thread::hardware_concurrency()` below.  Trace the reason so
    // operators investigating unexpected core counts can spot the cause
    // without needing to reproduce the probe failure.
    SPDLOG_TRACE("Physical-core probe failed: {}", ex.what());
  }
#endif

  return std::thread::hardware_concurrency();
}

}  // namespace

[[nodiscard]] unsigned detected_physical_concurrency() noexcept
{
  // Cache the topology probe — it never changes for the life of the
  // process and the sysfs walk is comparatively expensive.
  static const unsigned cached = probe_physical_concurrency();
  return cached;
}

[[nodiscard]] unsigned physical_concurrency() noexcept
{
  if (const auto q = g_quota_cores.load(std::memory_order_relaxed); q != 0) {
    return q;
  }
  return detected_physical_concurrency();
}

void set_cpu_quota_pct(double pct) noexcept
{
  // Reject non-finite, non-positive, and "no-op" (>= 100) values.
  // Everything outside the open interval (0, 100) disables clamping.
  if (!std::isfinite(pct) || pct <= 0.0 || pct >= 100.0) {
    g_quota_cores.store(0, std::memory_order_relaxed);
    g_quota_pct.store(0.0, std::memory_order_relaxed);
    return;
  }
  const auto detected = detected_physical_concurrency();
  if (detected == 0) {
    // No detection — clamp would be meaningless; leave disabled.
    g_quota_cores.store(0, std::memory_order_relaxed);
    g_quota_pct.store(0.0, std::memory_order_relaxed);
    return;
  }
  const auto clamped =
      std::max(1U,
               static_cast<unsigned>(
                   std::ceil(static_cast<double>(detected) * pct / 100.0)));
  g_quota_cores.store(clamped, std::memory_order_relaxed);
  g_quota_pct.store(pct, std::memory_order_relaxed);
}

[[nodiscard]] double get_cpu_quota_pct() noexcept
{
  return g_quota_pct.load(std::memory_order_relaxed);
}

}  // namespace gtopt
