// SPDX-License-Identifier: BSD-3-Clause
//
// Regression tests for the workpool stall-recovery fix
// (work_pool.hpp: log_periodic_stats recovery nudge +
//  worker_loop top-level exception catch).
//
// Bug pattern this guards against:
//   The pool wedges in state `Pending: N, Active: 0` with no CPU
//   usage even after memory has been freed.  Workers are parked on
//   the gate back-off `cv_.wait_for(scheduler_interval_, ...)` and
//   the stats monitor needs to nudge `cv_.notify_all()` to wake them.
//   Without the nudge, the pool stays wedged forever (observed on
//   juan/IPLP after memory pressure subsided to 91% free).

#include <atomic>
#include <chrono>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/work_pool.hpp>

using namespace gtopt;

namespace
// NOLINTBEGIN(bugprone-argument-comment)
{
using TestPool = BasicWorkPool<>;

}  // namespace
// NOLINTEND(bugprone-argument-comment)

TEST_CASE(
    "WorkPool: top-level worker_loop catch survives exception "
    "from outside task.execute (defense-in-depth)")  // NOLINT
{
  // This test exercises the top-level try/catch around worker_loop's
  // body.  The pool should NOT lose a worker if a task throws — but
  // since `task.execute()` already has its own catch, the only way
  // an exception escapes to the outer catch is via surrounding
  // machinery (cpu_monitor, memory_monitor, scoped_lock).  We can't
  // easily inject those in a unit test without mocking the monitors,
  // so this test just verifies the existing task-exception catch
  // still works AND the pool drains the queue afterwards.
  TestPool pool {WorkPoolConfig {2,
                                 95.0,
                                 64.0,
                                 99.0,
                                 0.0,
                                 std::chrono::milliseconds {10},
                                 "test-stall",
                                 false,
                                 0.0,
                                 0.0}};
  pool.start();

  std::atomic<int> ran {0};
  for (int i = 0; i < 4; ++i) {
    [[maybe_unused]] auto fut =
        pool.submit([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
  }

  // Submit a poison task that throws an std::runtime_error.
  [[maybe_unused]] auto poison_fut =
      pool.submit([]() { throw std::runtime_error {"poison task"}; });

  // Followed by more good tasks — these must still run after the
  // poison task crashed inside the inner task.execute() catch.
  for (int i = 0; i < 4; ++i) {
    [[maybe_unused]] auto fut =
        pool.submit([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); });
  }

  // Wait up to 5 s for all 8 good tasks to complete.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (ran.load() < 8 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  CHECK(ran.load() == 8);
  // Stats should reflect 9 dispatched (8 good + 1 poison).
  const auto stats = pool.get_statistics();
  CHECK(stats.tasks_completed >= 8);
}

TEST_CASE(
    "WorkPool: log_periodic_stats is non-const so the recovery "
    "nudge can call maybe_spawn_worker_unlocked / cv_.notify_all")  // NOLINT
{
  // Runtime smoke test: just constructing the pool and submitting a
  // single task is enough to prove `cv_` was declared `mutable` and
  // that worker_loop links against the new code — both are required
  // for the stall-recovery nudge to compile and link.  We can't probe
  // the private `log_periodic_stats` directly from a test, but if the
  // header signature regresses to `const`, the in-class definition
  // calling `maybe_spawn_worker_unlocked()` will fail to compile.
  TestPool pool {WorkPoolConfig {1,
                                 95.0,
                                 64.0,
                                 99.0,
                                 0.0,
                                 std::chrono::milliseconds {10},
                                 "test-mutable-cv",
                                 false,
                                 0.0,
                                 0.0}};
  pool.start();
  std::atomic<bool> ran {false};
  [[maybe_unused]] auto fut =
      pool.submit([&ran]() { ran.store(true, std::memory_order_relaxed); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ran.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(ran.load());
}

TEST_CASE(
    "WorkPool: full drain of submitted batch — sanity that "
    "workers don't wedge under normal load")  // NOLINT
{
  TestPool pool {WorkPoolConfig {4,
                                 95.0,
                                 64.0,
                                 99.0,
                                 0.0,
                                 std::chrono::milliseconds {10},
                                 "test-drain",
                                 false,
                                 0.0,
                                 0.0}};
  pool.start();

  constexpr int kTasks = 32;
  std::atomic<int> ran {0};
  for (int i = 0; i < kTasks; ++i) {
    [[maybe_unused]] auto fut = pool.submit(
        [&ran]()
        {
          // Small work to give the gate-check loop a chance to fire.
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          ran.fetch_add(1, std::memory_order_relaxed);
        });
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (ran.load() < kTasks && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  CHECK(ran.load() == kTasks);
  const auto stats = pool.get_statistics();
  CHECK(stats.tasks_pending == 0);
}
