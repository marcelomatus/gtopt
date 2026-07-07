// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_coordinator_pool.cpp
 * @brief     Unit tests for the Tier-1 CoordinatorPool.
 * @copyright BSD-3-Clause
 */
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/coordinator_pool.hpp>

using namespace gtopt;

TEST_CASE("CoordinatorPool basic behavior")  // NOLINT
{
  SUBCASE("run_driver returns the callable's result")
  {
    CoordinatorPool coord {4};
    auto fut = coord.run_driver([] { return 42; });
    CHECK(fut.get() == 42);
  }

  SUBCASE("num_drivers reflects construction")
  {
    CoordinatorPool coord {16};
    CHECK(coord.num_drivers() == 16);
  }

  SUBCASE("drivers run concurrently (not serialized or deferred)")
  {
    // Each of N drivers waits until ALL N are running.  If run_driver were
    // serial or used a deferred launch policy, the first driver would never
    // observe N concurrent peers and would time out (returning false).  Real
    // concurrency makes every driver observe N → all true.  This is the
    // property the forward/backward passes rely on.
    constexpr int N = 8;
    CoordinatorPool coord {N};
    std::atomic<int> running {0};
    std::vector<std::future<bool>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
      futs.push_back(coord.run_driver(
          [&running]
          {
            running.fetch_add(1, std::memory_order_relaxed);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (running.load(std::memory_order_relaxed) < N) {
              if (std::chrono::steady_clock::now() > deadline) {
                return false;
              }
              std::this_thread::yield();
            }
            return true;
          }));
    }
    bool all_concurrent = true;
    for (auto& f : futs) {
      all_concurrent = all_concurrent && f.get();
    }
    CHECK(all_concurrent);
  }

  SUBCASE("results are independent across drivers")
  {
    constexpr int N = 6;
    CoordinatorPool coord {N};
    std::vector<std::future<int>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
      futs.push_back(coord.run_driver([i] { return i * i; }));
    }
    for (int i = 0; i < N; ++i) {
      CHECK(futs[static_cast<std::size_t>(i)].get() == i * i);
    }
  }

  SUBCASE("exception in a driver propagates through the future")
  {
    CoordinatorPool coord {2};
    auto fut =
        coord.run_driver([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(fut.get(), std::runtime_error);
  }
}
