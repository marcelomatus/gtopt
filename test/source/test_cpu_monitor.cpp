#include <doctest/doctest.h>
#include <gtopt/cpu_monitor.hpp>

#include <chrono>
#include <thread>

using namespace gtopt;

TEST_CASE("CPUMonitor default construction")
{
  CPUMonitor monitor;

  CHECK(monitor.get_load() == doctest::Approx(0.0));
  CHECK(monitor.get_interval() == std::chrono::milliseconds {100});
}

TEST_CASE("CPUMonitor set_interval")
{
  CPUMonitor monitor;

  monitor.set_interval(std::chrono::milliseconds {200});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {200});

  monitor.set_interval(std::chrono::milliseconds {50});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {50});
}

TEST_CASE("CPUMonitor get_system_cpu_usage with fallback")
{
  // This should return a valid value on Linux or the fallback on other systems
  const double usage = CPUMonitor::get_system_cpu_usage(42.0);

  // On systems without /proc/stat, it should return fallback
  // On Linux, it should return 0-100 (or a reasonable value on first call)
  CHECK(usage >= 0.0);
  CHECK(usage <= 100.0);
}

TEST_CASE("CPUMonitor start and stop")
{
  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {50});

  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {200});
  monitor.stop();

  // After running for a bit, load should have been sampled
  // The value may be 0.0 on first read or a valid CPU percentage
  CHECK(monitor.get_load() >= 0.0);
  CHECK(monitor.get_load() <= 100.0);
}

TEST_CASE("CPUMonitor double start is safe")
{
  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {50});

  monitor.start();
  CHECK_NOTHROW(monitor.start());  // Second start should be a no-op

  monitor.stop();
}

TEST_CASE("CPUMonitor stop without start is safe")
{
  CPUMonitor monitor;
  CHECK_NOTHROW(monitor.stop());
}
