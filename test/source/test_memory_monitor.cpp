// SPDX-License-Identifier: BSD-3-Clause
#include <chrono>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/memory_monitor.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("MemoryMonitor default construction")  // NOLINT
{
  const MemoryMonitor mon;

  SUBCASE("initial readings are zero or non-negative")
  {
    CHECK(mon.get_total_mb() == doctest::Approx(0.0));
    CHECK(mon.get_available_mb() == doctest::Approx(0.0));
    CHECK(mon.get_process_rss_mb() == doctest::Approx(0.0));
    CHECK(mon.get_process_swap_mb() == doctest::Approx(0.0));
    CHECK(mon.get_swap_used_mb() == doctest::Approx(0.0));
    CHECK(mon.get_swap_io_rate() == doctest::Approx(0.0));
  }

  SUBCASE("default interval is 500ms")
  {
    CHECK(mon.get_interval() == std::chrono::milliseconds(500));
  }
}

TEST_CASE("MemoryMonitor::get_system_memory_snapshot")  // NOLINT
{
  SUBCASE("returns a snapshot with non-negative fields")
  {
    const auto snap = MemoryMonitor::get_system_memory_snapshot();
    CHECK(snap.total_mb >= 0.0);
    CHECK(snap.available_mb >= 0.0);
    CHECK(snap.process_rss_mb >= 0.0);
    CHECK(snap.process_swap_mb >= 0.0);
    CHECK(snap.swap_total_mb >= 0.0);
    CHECK(snap.swap_used_mb >= 0.0);
    CHECK(snap.vmstat_pswpin >= 0.0);
    CHECK(snap.vmstat_pswpout >= 0.0);
  }

  SUBCASE("swap_used_mb is bounded by swap_total_mb")
  {
    // SwapFree ≤ SwapTotal always, so SwapUsed = SwapTotal - SwapFree is
    // in [0, SwapTotal].
    const auto snap = MemoryMonitor::get_system_memory_snapshot();
    CHECK(snap.swap_used_mb <= snap.swap_total_mb + 0.001);
  }

  SUBCASE("pswpin/pswpout counters are monotonic across snapshots")
  {
    const auto snap1 = MemoryMonitor::get_system_memory_snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto snap2 = MemoryMonitor::get_system_memory_snapshot();
    // Cumulative counters from /proc/vmstat never decrease between reads.
    CHECK(snap2.vmstat_pswpin >= snap1.vmstat_pswpin);
    CHECK(snap2.vmstat_pswpout >= snap1.vmstat_pswpout);
  }

  SUBCASE("custom fallback does not break swap fields")
  {
    const auto snap = MemoryMonitor::get_system_memory_snapshot(8192.0);
    CHECK(snap.process_swap_mb >= 0.0);
    CHECK(snap.swap_used_mb >= 0.0);
  }
}

TEST_CASE("MemoryMonitor background thread updates swap readings")  // NOLINT
{
  MemoryMonitor mon;
  mon.set_interval(std::chrono::milliseconds(10));

  SUBCASE("start then stop yields non-negative swap readings")
  {
    mon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    mon.stop();
    CHECK(mon.get_process_swap_mb() >= 0.0);
    CHECK(mon.get_swap_used_mb() >= 0.0);
    CHECK(mon.get_swap_io_rate() >= 0.0);
  }

  SUBCASE("swap_io_rate stays finite on an idle system")
  {
    mon.start();
    // Long enough for at least a couple of sample cycles.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto rate = mon.get_swap_io_rate();
    mon.stop();
    // An idle test process may see 0 pg/s, or some residual system-wide
    // paging — but never a nonsense value like NaN/infinity/negative.
    CHECK(rate >= 0.0);
    CHECK(rate < 1.0e9);
  }

  SUBCASE("double start/stop is safe for swap fields")
  {
    mon.start();
    mon.start();
    mon.stop();
    mon.stop();
    CHECK(mon.get_process_swap_mb() >= 0.0);
    CHECK(mon.get_swap_io_rate() >= 0.0);
  }
}
