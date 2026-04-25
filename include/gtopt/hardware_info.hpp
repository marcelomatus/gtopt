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
 *
 * ## CPU quota
 *
 * The process-global `set_cpu_quota_pct()` lets the user shrink the unit
 * of "1 CPU" reported by `physical_concurrency()` once at startup.  When
 * a quota is set, every work-pool factory that multiplies the result by
 * its own `cpu_factor` automatically gets a smaller slice of the host:
 *
 *   set_cpu_quota_pct(30) on a 9-physical-core box →
 *     physical_concurrency() = ceil(9 × 0.30) = 3
 *
 * Pools with `cpu_factor=2.0` then size to 6 threads instead of 18,
 * pools with `cpu_factor=4.0` size to 12 instead of 36, etc.
 *
 * The detected hardware count is preserved via
 * `detected_physical_concurrency()` so logs and SMT ratios remain
 * meaningful.
 */

#pragma once

#include <thread>

namespace gtopt
{

/// True hardware physical-core count (Linux topology probe), never
/// affected by `set_cpu_quota_pct()`.  Use this for diagnostics, log
/// messages, and the SMT-ratio computation.  Returns
/// `std::thread::hardware_concurrency()` on non-Linux or parse failure.
[[nodiscard]] unsigned detected_physical_concurrency() noexcept;

/// Effective physical CPU core count.  Equal to
/// `detected_physical_concurrency()` unless `set_cpu_quota_pct()` has
/// been called with a value in (0, 100), in which case the result is
/// `ceil(detected × pct / 100)` (always at least 1).
///
/// All work-pool factories should call this — never
/// `std::thread::hardware_concurrency()` directly — so the CPU quota
/// cascades through every `cpu_factor` multiplication.
[[nodiscard]] unsigned physical_concurrency() noexcept;

/// Set the process-global CPU quota as a percentage of physical cores.
/// Values in (0, 100) clamp `physical_concurrency()` to
/// `ceil(detected × pct / 100)` (minimum 1).  Values outside that
/// range — including 0, negative, NaN, and ≥100 — disable the clamp.
///
/// Intended to be called once during CLI parsing, before any pool is
/// constructed.  Thread-safe: the underlying state is an atomic load.
void set_cpu_quota_pct(double pct) noexcept;

/// Currently configured CPU quota in percent, or 0 when no clamp is
/// active.  Useful for logging and tests.
[[nodiscard]] double get_cpu_quota_pct() noexcept;

/// Ratio of logical to physical cores (1 without HT, typically 2 with HT).
/// Returns 1 on detection failure.  Always uses
/// `detected_physical_concurrency()` so a CPU quota does not skew the
/// ratio.
[[nodiscard]] inline unsigned smt_ratio() noexcept
{
  const auto phys = detected_physical_concurrency();
  const auto logical = std::thread::hardware_concurrency();
  if (phys == 0 || logical == 0) {
    return 1;
  }
  // Round to nearest integer ratio (typically 1 or 2)
  return std::max(1U, (logical + (phys / 2)) / phys);
}

}  // namespace gtopt
