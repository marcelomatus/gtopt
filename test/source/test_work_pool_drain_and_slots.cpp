// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_work_pool_drain_and_slots.cpp
 * @brief     Tests for get_all_futures (drain-before-rethrow barrier) and
 *            the exact SlotReleaseGuard worker-thread check.
 * @date      2026-07-06
 * @author    claude
 * @copyright BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/work_pool.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(  // NOLINT
    "get_all_futures drains every future before rethrowing the first "
    "exception")
{
  WorkPoolConfig cfg;
  cfg.max_threads = 4;
  cfg.enable_periodic_stats = false;
  BasicWorkPool<> pool {cfg};
  pool.start();

  constexpr int n_slow = 4;
  std::atomic<int> completed {0};

  std::vector<std::future<void>> futures;

  // futures[0]: throws immediately.  The barrier must still drain the
  // slow tasks behind it before letting this exception propagate.
  {
    auto fut = pool.submit(std::function<void()> {
        [] { throw std::runtime_error("first-task-boom"); }});
    REQUIRE(fut.has_value());
    futures.push_back(std::move(*fut));
  }
  for (int i = 0; i < n_slow; ++i) {
    auto fut = pool.submit(std::function<void()> {
        [&completed]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds {50});
          completed.fetch_add(1, std::memory_order_relaxed);
        }});
    REQUIRE(fut.has_value());
    futures.push_back(std::move(*fut));
  }

  CHECK_THROWS_WITH_AS(
      get_all_futures(futures), "first-task-boom", std::runtime_error);
  // The whole point: by the time the exception escapes, EVERY sibling
  // task has completed — nothing is left running against caller locals.
  CHECK(completed.load(std::memory_order_relaxed) == n_slow);
}

TEST_CASE("get_all_futures completes normally when no task throws")  // NOLINT
{
  WorkPoolConfig cfg;
  cfg.max_threads = 2;
  cfg.enable_periodic_stats = false;
  BasicWorkPool<> pool {cfg};
  pool.start();

  std::atomic<int> completed {0};
  std::vector<std::future<void>> futures;
  for (int i = 0; i < 6; ++i) {
    auto fut = pool.submit(std::function<void()> {
        [&completed] { completed.fetch_add(1, std::memory_order_relaxed); }});
    REQUIRE(fut.has_value());
    futures.push_back(std::move(*fut));
  }

  CHECK_NOTHROW(get_all_futures(futures));
  CHECK(completed.load(std::memory_order_relaxed) == 6);
}

TEST_CASE("get_all_futures skips invalid futures")  // NOLINT
{
  // Mirrors the SDDP forward pass's terminal-skip pattern, where the
  // futures vector holds default-constructed (invalid) entries for
  // skipped scenes.
  WorkPoolConfig cfg;
  cfg.max_threads = 2;
  cfg.enable_periodic_stats = false;
  BasicWorkPool<> pool {cfg};
  pool.start();

  std::atomic<int> completed {0};
  std::vector<std::future<void>> futures;
  futures.emplace_back();  // invalid — must be skipped, not throw
  {
    auto fut = pool.submit(std::function<void()> {
        [&completed] { completed.fetch_add(1, std::memory_order_relaxed); }});
    REQUIRE(fut.has_value());
    futures.push_back(std::move(*fut));
  }
  futures.emplace_back();  // invalid

  CHECK_NOTHROW(get_all_futures(futures));
  CHECK(completed.load(std::memory_order_relaxed) == 1);
}

TEST_CASE(  // NOLINT
    "SlotReleaseGuard from a non-worker thread never releases another "
    "task's slot")
{
  WorkPoolConfig cfg;
  cfg.max_threads = 2;
  cfg.enable_periodic_stats = false;
  BasicWorkPool<> pool {cfg};
  pool.start();

  std::atomic<bool> started {false};
  std::atomic<bool> release {false};

  auto fut = pool.submit(std::function<void()> {
      [&]
      {
        started.store(true, std::memory_order_release);
        // Bounded wait so a test failure can't hang the suite.
        for (int i = 0; i < 5000; ++i) {
          if (release.load(std::memory_order_acquire)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
      }});
  REQUIRE(fut.has_value());

  // Wait until the task is running (tasks_active == 1).
  for (int i = 0; i < 5000 && !started.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds {1});
  }
  REQUIRE(started.load(std::memory_order_acquire));
  CHECK(pool.get_statistics().tasks_active == 1);

  {
    // This thread is NOT a pool worker: the guard must be an exact
    // no-op even though another task is active.  The pre-fix
    // `tasks_active_ > 0` heuristic decremented the running task's
    // slot here, over-admitting by one.
    auto guard = pool.release_slot_while_blocking();
    CHECK(pool.get_statistics().tasks_active == 1);
  }
  CHECK(pool.get_statistics().tasks_active == 1);

  release.store(true, std::memory_order_release);
  fut->get();
}

TEST_CASE(  // NOLINT
    "shutdown with the periodic-stats thread enabled is prompt")
{
  // Regression sentinel for the stats-thread missed-wakeup: shutdown()
  // must nudge the stats thread out of its 30 s wait_for under
  // stats_mutex_ (a bare notify can be lost in the predicate-to-wait
  // window and stall shutdown by the full timeout).
  const auto t0 = std::chrono::steady_clock::now();
  {
    WorkPoolConfig cfg;
    cfg.max_threads = 2;
    cfg.enable_periodic_stats = true;
    BasicWorkPool<> pool {cfg};
    pool.start();

    auto fut = pool.submit(std::function<void()> {[] {}});
    REQUIRE(fut.has_value());
    fut->get();

    pool.shutdown();
  }
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  CHECK(elapsed < 10.0);
}
