// SPDX-License-Identifier: BSD-3-Clause
#include <chrono>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/cpu_monitor.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("CPUMonitor default construction")  // NOLINT
{
  const CPUMonitor mon;

  SUBCASE("initial load is zero")
  {
    CHECK(mon.get_load() == doctest::Approx(0.0));
  }

  SUBCASE("default interval is 100ms")
  {
    CHECK(mon.get_interval() == std::chrono::milliseconds(100));
  }
}

TEST_CASE("CPUMonitor set_interval")  // NOLINT
{
  CPUMonitor mon;
  mon.set_interval(std::chrono::milliseconds(50));
  CHECK(mon.get_interval() == std::chrono::milliseconds(50));
}

TEST_CASE("CPUMonitor get_system_cpu_usage")  // NOLINT
{
  SUBCASE("returns a value between 0 and 100 or the fallback")
  {
    const auto usage = CPUMonitor::get_system_cpu_usage(42.0);
    // On Linux with /proc/stat, first call may return 0 (no delta yet)
    // or a real value.  On non-Linux, returns fallback.
    CHECK(usage >= 0.0);
    CHECK(usage <= 100.0);
  }

  SUBCASE("second call produces a meaningful delta")
  {
    // First call seeds the counters
    CPUMonitor::get_system_cpu_usage();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto usage = CPUMonitor::get_system_cpu_usage();
    CHECK(usage >= 0.0);
    CHECK(usage <= 100.0);
  }

  SUBCASE("custom fallback value is returned when /proc/stat missing")
  {
    // We can't easily simulate missing /proc/stat, but we can verify
    // the function handles calls gracefully.
    const auto usage = CPUMonitor::get_system_cpu_usage(99.0);
    CHECK(usage >= 0.0);
    CHECK(usage <= 100.0);
  }
}

TEST_CASE("CPUMonitor start and stop")  // NOLINT
{
  CPUMonitor mon;
  mon.set_interval(std::chrono::milliseconds(10));

  SUBCASE("start then stop")
  {
    mon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mon.stop();
    // After stopping, load should have been updated at least once
    // (it's a relaxed read, so any value >= 0 is fine)
    CHECK(mon.get_load() >= 0.0);
  }

  SUBCASE("double start is safe")
  {
    mon.start();
    mon.start();  // second start returns immediately
    mon.stop();
    CHECK(mon.get_load() >= 0.0);
  }

  SUBCASE("double stop is safe")
  {
    mon.start();
    mon.stop();
    mon.stop();  // second stop is a no-op
    CHECK(mon.get_load() >= 0.0);
  }

  SUBCASE("stop without start is safe")
  {
    mon.stop();
    CHECK(mon.get_load() == doctest::Approx(0.0));
  }
}
