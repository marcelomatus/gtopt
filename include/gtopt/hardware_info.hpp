/**
 * @file      hardware_info.hpp
 * @brief     Physical vs logical CPU core detection
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides physical core counting for accurate thread pool sizing.
 * On Linux, parses /sys/devices/system/cpu topology to distinguish
 * physical cores from hyperthreads.  Falls back to
 * std::thread::hardware_concurrency() on other platforms or parse failure.
 */

#pragma once

#include <thread>

namespace gtopt
{

/// Number of physical CPU cores (excludes hyperthreads / SMT siblings).
/// On Linux, enumerates unique (physical_package_id, core_id) pairs via
/// /sys/devices/system/cpu/cpuN/topology/.
/// Returns hardware_concurrency() on non-Linux or parse failure.
[[nodiscard]] unsigned physical_concurrency() noexcept;

/// Ratio of logical to physical cores (1 without HT, typically 2 with HT).
/// Returns 1 on detection failure.
[[nodiscard]] inline unsigned smt_ratio() noexcept
{
  const auto phys = physical_concurrency();
  const auto logical = std::thread::hardware_concurrency();
  if (phys == 0 || logical == 0) {
    return 1;
  }
  // Round to nearest integer ratio (typically 1 or 2)
  return std::max(1U, (logical + phys / 2) / phys);
}

}  // namespace gtopt
