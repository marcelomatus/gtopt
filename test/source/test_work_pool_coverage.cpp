// SPDX-License-Identifier: BSD-3-Clause
/// @file test_work_pool_coverage.hpp
/// @brief Additional WorkPool tests for coverage: double start, submit_lambda,
///        format_statistics, string-key pool, shutdown behavior, priority
///        thresholds.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/work_pool.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Double start is idempotent ─────────────────────────────────────────────

TEST_CASE("WorkPool double start is a no-op")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      4,
      90.0,
      10ms,
      "DoubleStartPool",
  });

  pool.start();
  // Second start should be harmless
  CHECK_NOTHROW(pool.start());

  // Submit a task to prove the pool is functional
  auto result = pool.submit([] { return 7; });
  REQUIRE(result.has_value());
  CHECK(result.value().get() == 7);

  pool.shutdown();
}

// ─── Double shutdown is safe ────────────────────────────────────────────────

TEST_CASE("WorkPool double shutdown is safe")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      90.0,
      10ms,
      "DoubleShutdownPool",
  });
  pool.start();

  auto result = pool.submit([] { return 1; });
  REQUIRE(result.has_value());
  result.value().wait();

  pool.shutdown();
  // Second shutdown should be harmless
  CHECK_NOTHROW(pool.shutdown());
}

// ─── submit_lambda convenience method ───────────────────────────────────────

TEST_CASE("WorkPool submit_lambda works")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      4,
      90.0,
      10ms,
      "LambdaPool",
  });
  pool.start();

  auto result = pool.submit_lambda([] { return 42; });
  REQUIRE(result.has_value());
  CHECK(result.value().get() == 42);

  // submit_lambda with custom requirements
  auto result2 = pool.submit_lambda([] { return 99; },
                                    TaskRequirements {
                                        .priority = TaskPriority::High,
                                        .name = "high_lambda",
                                    });
  REQUIRE(result2.has_value());
  CHECK(result2.value().get() == 99);

  pool.shutdown();
}

// ─── format_statistics produces non-empty string ────────────────────────────

TEST_CASE("WorkPool format_statistics returns valid string")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      90.0,
      10ms,
      "StatsPool",
  });
  pool.start();

  auto result = pool.submit([] { return 0; });
  REQUIRE(result.has_value());
  result.value().wait();

  // Allow the scheduler to clean up
  std::this_thread::sleep_for(50ms);

  const auto stats_str = pool.format_statistics();
  CHECK_FALSE(stats_str.empty());
  CHECK(stats_str.find("WorkPool Statistics") != std::string::npos);
  CHECK(stats_str.find("Tasks") != std::string::npos);
  CHECK(stats_str.find("Threads") != std::string::npos);
  CHECK(stats_str.find("CPU Load") != std::string::npos);

  pool.shutdown();
}

// ─── log_statistics does not throw ──────────────────────────────────────────

TEST_CASE("WorkPool log_statistics does not throw")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      90.0,
      std::chrono::milliseconds {10},
      "LogStatsPool",
  });
  CHECK_NOTHROW(pool.log_statistics());
}

// ─── Statistics reflect submitted and completed counts ──────────────────────

TEST_CASE("WorkPool statistics track submission and completion")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      4,
      95.0,
      10ms,
      "TrackStatsPool",
  });
  pool.start();

  constexpr int n_tasks = 5;
  std::vector<std::future<int>> futures;
  futures.reserve(n_tasks);

  for (int i = 0; i < n_tasks; ++i) {
    auto r = pool.submit([i] { return i; });
    REQUIRE(r.has_value());
    futures.push_back(std::move(r.value()));
  }

  // Wait for all
  for (auto& f : futures) {
    f.wait();
  }

  // Allow cleanup cycles
  std::this_thread::sleep_for(100ms);

  const auto stats = pool.get_statistics();
  CHECK(stats.tasks_submitted >= static_cast<size_t>(n_tasks));

  pool.shutdown();
}

// ─── Critical priority tasks ────────────────────────────────────────────────

TEST_CASE("WorkPool Critical priority tasks execute")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      4,
      50.0,  // low threshold to exercise priority-based threshold override
      10ms,
      "CriticalPool",
  });
  pool.start();

  // Critical priority has fixed 95% threshold
  auto result = pool.submit([] { return 123; },
                            TaskRequirements {
                                .priority = TaskPriority::Critical,
                                .name = "critical_task",
                            });
  REQUIRE(result.has_value());
  CHECK(result.value().get() == 123);

  // High priority adds +5% to max_cpu_threshold
  auto result2 = pool.submit([] { return 456; },
                             TaskRequirements {
                                 .priority = TaskPriority::High,
                                 .name = "high_task",
                             });
  REQUIRE(result2.has_value());
  CHECK(result2.value().get() == 456);

  // Low priority uses max_cpu_threshold directly
  auto result3 = pool.submit([] { return 789; },
                             TaskRequirements {
                                 .priority = TaskPriority::Low,
                                 .name = "low_task",
                             });
  REQUIRE(result3.has_value());
  CHECK(result3.value().get() == 789);

  pool.shutdown();
}

// ─── Task ordering with same priority, different keys ───────────────────────

TEST_CASE("Task age-based tie-breaking within same priority and key")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // When priority and key are identical, older tasks have higher priority
  Task<void, int64_t, std::less<>> first {
      [] {},
      TaskRequirements {
          .priority = TaskPriority::Medium,
          .priority_key = 5,
          .name = {},
      },
  };

  // Small delay so submit_time differs
  std::this_thread::sleep_for(std::chrono::milliseconds {1});

  Task<void, int64_t, std::less<>> second {
      [] {},
      TaskRequirements {
          .priority = TaskPriority::Medium,
          .priority_key = 5,
          .name = {},
      },
  };

  // first was submitted earlier => higher priority => first < second is false
  // (in max-heap terms, first is "greater" = dequeued first)
  CHECK_FALSE(first < second);
  CHECK(second < first);
}

// ─── Task age accessor ─────────────────────────────────────────────────────

TEST_CASE("Task age increases over time")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Task<void, int64_t, std::less<>> task {
      [] {},
      TaskRequirements {
          .priority = TaskPriority::Medium,
          .name = {},
      },
  };

  const auto age1 = task.age();
  std::this_thread::sleep_for(std::chrono::milliseconds {5});
  const auto age2 = task.age();

  CHECK(age2 > age1);
}

// ─── Task requirements accessor ─────────────────────────────────────────────

TEST_CASE("Task requirements accessor returns correct values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TaskRequirements req {
      .estimated_threads = 3,
      .estimated_duration = std::chrono::milliseconds {500},
      .priority = TaskPriority::High,
      .priority_key = 42,
      .name = "test_req",
  };

  Task<void, int64_t, std::less<>> task {[] {}, req};

  CHECK(task.requirements().estimated_threads == 3);
  CHECK(task.requirements().estimated_duration
        == std::chrono::milliseconds {500});
  CHECK(task.requirements().priority == TaskPriority::High);
  CHECK(task.requirements().priority_key == 42);
  REQUIRE(
      (task.requirements().name && *task.requirements().name == "test_req"));
}

// ─── ActiveTask is_ready and runtime ────────────────────────────────────────

TEST_CASE("ActiveTask is_ready and runtime")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  std::promise<void> prom;
  auto fut = prom.get_future();

  ActiveTask atask {
      .future = std::move(fut),
      .estimated_threads = 2,
      .start_time = std::chrono::steady_clock::now(),
  };

  // Not ready yet
  CHECK_FALSE(atask.is_ready());
  CHECK(atask.estimated_threads == 2);

  // Runtime should be non-negative
  CHECK(atask.runtime() >= std::chrono::nanoseconds {0});

  // Complete the task
  prom.set_value();
  CHECK(atask.is_ready());
}

// ─── BasicWorkPool with string key ──────────────────────────────────────────

TEST_CASE("BasicWorkPool with string key type")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  using StringPool = BasicWorkPool<std::string>;

  StringPool pool(WorkPoolConfig {
      2,
      95.0,
      10ms,
      "StringKeyPool",
  });
  pool.start();

  static_assert(std::same_as<StringPool::key_type, std::string>);

  auto result = pool.submit([] { return 77; },
                            BasicTaskRequirements<std::string> {
                                .priority = TaskPriority::Medium,
                                .priority_key = "alpha",
                                .name = "string_key_task",
                            });
  REQUIRE(result.has_value());
  CHECK(result.value().get() == 77);

  pool.shutdown();
}

// ─── WorkPoolConfig default values ──────────────────────────────────────────

TEST_CASE("WorkPoolConfig default values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const WorkPoolConfig config;

  CHECK(config.max_threads
        == static_cast<int>(std::thread::hardware_concurrency()));
  CHECK(config.max_cpu_threshold == doctest::Approx(95.0));
  CHECK(config.scheduler_interval == std::chrono::milliseconds {20});
  CHECK(config.name == "WorkPool");
}

// ─── TaskStatus enum values ─────────────────────────────────────────────────

TEST_CASE("TaskStatus enum values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(static_cast<uint8_t>(TaskStatus::Success) == 0);
  CHECK(static_cast<uint8_t>(TaskStatus::Failed) == 1);
  CHECK(static_cast<uint8_t>(TaskStatus::Cancelled) == 2);
}

// ─── TaskPriority enum ordering ─────────────────────────────────────────────

TEST_CASE("TaskPriority enum ordering")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(TaskPriority::Low < TaskPriority::Medium);
  CHECK(TaskPriority::Medium < TaskPriority::High);
  CHECK(TaskPriority::High < TaskPriority::Critical);
}

// ─── Pool with exception-throwing task ──────────────────────────────────────

TEST_CASE("WorkPool task exception propagation via future")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      95.0,
      10ms,
      "ExceptionPool",
  });
  pool.start();

  auto result = pool.submit(
      []() -> int { throw std::logic_error("intentional test error"); });
  REQUIRE(result.has_value());

  CHECK_THROWS_AS(result.value().get(), std::logic_error);

  pool.shutdown();
}

// ─── Pool with void-returning tasks ─────────────────────────────────────────

TEST_CASE("WorkPool void tasks complete successfully")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      95.0,
      10ms,
      "VoidPool",
  });
  pool.start();

  std::atomic<int> counter {0};
  auto r1 = pool.submit([&] { counter++; });
  auto r2 = pool.submit([&] { counter++; });

  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());

  r1.value().wait();
  r2.value().wait();

  CHECK(counter == 2);

  pool.shutdown();
}
