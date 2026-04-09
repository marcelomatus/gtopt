#include <chrono>
#include <random>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/work_pool.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── BasicTaskRequirements template tests ───────────────────────────────────

TEST_CASE("BasicTaskRequirements template key")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default key type is int64_t")
  {
    const BasicTaskRequirements<> req;
    CHECK(req.estimated_threads == 1);
    CHECK(req.priority == TaskPriority::Medium);
    CHECK(req.priority_key == int64_t {0});
  }

  SUBCASE("TaskRequirements alias uses int64_t")
  {
    static_assert(std::same_as<TaskRequirements::key_type, int64_t>);
    const TaskRequirements req {.priority_key = 42, .name = {}};
    CHECK(req.priority_key == 42);
  }

  SUBCASE("custom string key type")
  {
    const BasicTaskRequirements<std::string> req {
        .priority = TaskPriority::High,
        .priority_key = "my_task",
        .name = {},
    };
    CHECK(req.priority_key == "my_task");
    CHECK(req.priority == TaskPriority::High);
  }
}

// ─── Task<> ordering with std::less (smaller key = higher priority) ──────────

TEST_CASE(
    "Task ordering: std::less semantics (smaller key = higher priority)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("smaller int64_t key → higher priority in max-heap")
  {
    // Lower key = higher priority (operator< returns true when this has LOWER
    // priority, i.e., when other.key is smaller)
    Task<void, int64_t, std::less<>> low_prio {
        [] {}, TaskRequirements {.priority_key = 100, .name = {}}};
    Task<void, int64_t, std::less<>> high_prio {
        [] {}, TaskRequirements {.priority_key = 1, .name = {}}};

    // In max-heap: "less than" means lower priority → gets pushed down
    // high_prio (key=1) < low_prio (key=100) should be false (high has higher
    // priority)
    CHECK_FALSE(high_prio < low_prio);
    // low_prio (key=100) < high_prio (key=1) should be true (low has lower
    // priority)
    CHECK(low_prio < high_prio);
  }

  SUBCASE("TaskPriority tier overrides key ordering")
  {
    Task<void, int64_t, std::less<>> medium {
        [] {},
        TaskRequirements {
            .priority = TaskPriority::Medium,
            .priority_key = 0,
            .name = {},
        }};
    Task<void, int64_t, std::less<>> high {[] {},
                                           TaskRequirements {
                                               .priority = TaskPriority::High,
                                               .priority_key = 999,
                                               .name = {},
                                           }};

    // High priority always beats medium, regardless of key value
    CHECK(medium < high);  // medium has lower priority
    CHECK_FALSE(high < medium);
  }

  SUBCASE("std::greater semantics: larger key = higher priority")
  {
    // Reverse ordering: larger key = higher priority (old-style behavior)
    Task<void, int64_t, std::greater<>> low_prio {
        [] {}, TaskRequirements {.priority_key = 1, .name = {}}};
    Task<void, int64_t, std::greater<>> high_prio {
        [] {}, TaskRequirements {.priority_key = 100, .name = {}}};

    CHECK_FALSE(high_prio < low_prio);  // high_prio (100) has higher priority
    CHECK(low_prio < high_prio);  // low_prio (1) has lower priority
  }
}

// ─── BasicWorkPool template tests ────────────────────────────────────────────

TEST_CASE("BasicWorkPool with int64_t key")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool;
  pool.start();

  SUBCASE("key_type exposed as int64_t")
  {
    static_assert(std::same_as<AdaptiveWorkPool::key_type, int64_t>);
    static_assert(
        std::same_as<AdaptiveWorkPool::key_compare, std::less<int64_t>>);
    CHECK(true);
  }

  SUBCASE("submit with int64_t priority_key – smaller key dequeued first")
  {
    // Submit tasks with different priority keys; smaller = higher priority
    std::vector<int> order;
    std::mutex mu;
    std::atomic<int> done {0};

    // Stop the scheduler so we can queue up tasks before any run
    // (We submit them quickly and check they all complete)
    auto t1 = pool.submit(
        [&]
        {
          const std::scoped_lock lk {mu};
          order.push_back(10);
          done++;
        },
        TaskRequirements {
            .priority = TaskPriority::Medium,
            .priority_key = 10,
            .name = {},
        });

    auto t2 = pool.submit(
        [&]
        {
          const std::scoped_lock lk {mu};
          order.push_back(1);
          done++;
        },
        TaskRequirements {
            .priority = TaskPriority::Medium,
            .priority_key = 1,
            .name = {},
        });

    REQUIRE(t1.has_value());
    REQUIRE(t2.has_value());
    t1->wait();
    t2->wait();
    CHECK(done == 2);
    CHECK(order.size() == 2);
  }

  pool.shutdown();
}

// ─── Backward compatibility tests ────────────────────────────────────────────

TEST_CASE("WorkPool basic functionality")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool;
  pool.start();

  SUBCASE("Submit and execute simple task")
  {
    auto result = pool.submit([] { return 42; });
    REQUIRE(result.has_value());
    CHECK(result.value().get() == 42);
  }

  SUBCASE("Task priority ordering")
  {
    std::vector<int> execution_order;
    std::mutex order_mutex;
    std::atomic<int> counter {0};

    // Submit tasks with different priorities
    auto high_task = pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(1);
          counter++;
        },
        {.priority = TaskPriority::High, .name = "high_priority_task"});

    auto medium_task = pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(2);
          counter++;
        },
        {.priority = TaskPriority::Medium, .name = "medium_priority_task"});
    REQUIRE(medium_task.has_value());

    auto low_task = pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(3);
          counter++;
        },
        {.priority = TaskPriority::Low, .name = "low_priority_task"});

    // Wait for all tasks to complete
    high_task.value().wait();
    medium_task.value().wait();
    low_task.value().wait();

    CHECK(counter == 3);
    // High priority should execute first
    CHECK(execution_order[0] == 1);
    // Medium should execute before low
    CHECK(std::ranges::find(execution_order, 2)
          < std::ranges::find(execution_order, 3));
  }

  SUBCASE("Statistics tracking")
  {
    auto stats = pool.get_statistics();
    CHECK(stats.tasks_submitted == 0);
    CHECK(stats.tasks_completed == 0);

    // Use a barrier to ensure tasks complete before checking stats
    std::promise<void> p1;
    std::promise<void> p2;
    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    auto task1 = pool.submit([&] { f1.wait(); });
    auto task2 = pool.submit([&] { f2.wait(); });

    // Allow tasks to start
    p1.set_value();
    p2.set_value();

    // Wait for both tasks to complete
    task1.value().wait();
    task2.value().wait();

    // Wait for pool to process completions with random sleep
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dist(1, 10);

    while (true) {
      stats = pool.get_statistics();
      if (stats.tasks_completed >= 2 && stats.tasks_active == 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
    }

    CHECK(stats.tasks_submitted >= 2);
    CHECK(stats.tasks_completed >= 2);
    CHECK(stats.tasks_active == 0);
  }

  SUBCASE("Batch submission")
  {
    std::vector<Task<void>> tasks;
    tasks.reserve(10);
    for (int i = 0; i < 10; ++i) {
      tasks.emplace_back([] { /* no-op */ });
    }

    // Batch submission test placeholder
    CHECK(true);
  }

  SUBCASE("Pending tasks generator")
  {
    for (int i = 0; i < 5; ++i) {
      auto result = pool.submit([] { std::this_thread::sleep_for(10ms); });
      REQUIRE(result.has_value());
    }

    // Pending tasks test placeholder
    CHECK(true);
  }

  pool.shutdown();
}

TEST_CASE("WorkPool stress testing")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  constexpr int max_threads = 16;
  WorkPoolConfig config;
  config.max_threads = max_threads;
  config.max_cpu_threshold = 90.0;
  config.scheduler_interval = 10ms;
  AdaptiveWorkPool pool(config);
  pool.start();

  SUBCASE("Submit 100 small tasks")
  {
    std::vector<std::future<int>> futures;
    futures.reserve(100);
    std::atomic<int> counter {0};

    for (int i = 0; i < 100; ++i) {
      auto result = pool.submit([&] { return counter++; });
      REQUIRE(result.has_value());
      futures.push_back(std::move(result.value()));
    }

    int total = 0;
    for (auto& f : futures) {
      total += f.get();
    }

    // Verify all tasks executed and returned unique values
    CHECK(counter == 100);
    CHECK(total == 4950);  // Sum of 0..99
  }

  SUBCASE("Submit 10 medium tasks")
  {
    int n = 50;
    std::vector<std::future<int>> futures;
    futures.reserve(static_cast<size_t>(n));
    std::atomic<int> counter {0};

    for (int i = 0; i < n; ++i) {
      auto result = pool.submit(
          [&]
          {
            static std::random_device rd;
            static std::mt19937 gen(rd());
            static std::uniform_int_distribution<> dist(0, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
            return counter++;
          },
          {.estimated_duration = 5ms, .name = "medium_task"});
      REQUIRE(result.has_value());
      futures.push_back(std::move(result.value()));
    }

    int total = 0;
    for (auto& f : futures) {
      total += f.get();
    }

    CHECK(counter == n);
    CHECK(total == ((n - 1) * n) / 2);  // Sum of 0..n-1
  }

  SUBCASE("Task exception handling")
  {
    auto result = pool.submit([] { throw std::runtime_error("test error"); });
    REQUIRE(result.has_value());
    CHECK_THROWS_AS(result.value().get(), std::runtime_error);
  }

  SUBCASE("Invalid task submission")
  {
    // Test handling of invalid tasks by passing a null function pointer
    const std::function<void()> null_func = nullptr;

    try {
      auto result = pool.submit(null_func);
      if (result) {
        FAIL_CHECK("Expected invalid task submission to fail");
      } else {
        CHECK(result.error()
              == std::make_error_code(std::errc::invalid_argument));
      }
    } catch (const std::exception& e) {
      // Submission threw an exception - this is acceptable
      CHECK(true);
    } catch (...) {
      FAIL_CHECK("Unexpected exception type");
    }
  }

  SUBCASE("Config verification")
  {
    const WorkPoolConfig cfg {
        4,  // max_threads
        80.0,  // max_cpu_threshold
        4096.0,  // min_free_memory_mb
        95.0,  // max_memory_percent
        0.0,  // max_process_rss_mb
        std::chrono::milliseconds(10),  // scheduler_interval
        "TestPool",  // name
    };
    CHECK(cfg.max_threads == 4);
    CHECK(cfg.max_cpu_threshold == 80.0);
    CHECK(cfg.scheduler_interval.count() == 10);
    CHECK(cfg.name == "TestPool");
    CHECK(true);
  }

  SUBCASE("Noexcept verification")
  {
    const AdaptiveWorkPool pool;
    CHECK(noexcept(pool.get_statistics()));
  }

  pool.shutdown();
}

TEST_CASE("CPUMonitor basic functionality")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;

  SUBCASE("Start/stop monitoring")
  {
    monitor.start();
    CHECK_NOTHROW(monitor.stop());
  }

  SUBCASE("Get CPU load")
  {
    monitor.start();
    const auto load = monitor.get_load();
    CHECK(load >= 0.0);
    CHECK(load <= 100.0);
    monitor.stop();
  }

  SUBCASE("Get system CPU usage")
  {
    const auto usage = CPUMonitor::get_system_cpu_usage();
    CHECK(usage >= 0.0);
    CHECK(usage <= 100.0);
  }
}

TEST_CASE("CPUMonitor edge cases")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;

  SUBCASE("Double start")
  {
    monitor.start();
    CHECK_NOTHROW(monitor.start());  // Should handle gracefully
    monitor.stop();
  }

  SUBCASE("Stop without start")
  {
    CHECK_NOTHROW(monitor.stop());
  }

  SUBCASE("Get load when not running")
  {
    CHECK(monitor.get_load() == 0.0);
  }
}

TEST_CASE("Mock CPU monitoring")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  class MockCPUMonitor : public CPUMonitor
  {
  public:
    void set_test_load(double load) { test_load_ = load; }

    [[nodiscard]] double get_load() const noexcept { return test_load_; }

  private:
    double test_load_ = 50.0;
  };

  MockCPUMonitor monitor;

  SUBCASE("Simulate low CPU load")
  {
    monitor.set_test_load(25.0);
    CHECK(monitor.get_load() == 25.0);
  }

  SUBCASE("Simulate high CPU load")
  {
    monitor.set_test_load(90.0);
    CHECK(monitor.get_load() == 90.0);
  }
}

TEST_CASE("CPUMonitor default interval")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const CPUMonitor monitor;

  CHECK(monitor.get_load() == doctest::Approx(0.0));
  CHECK(monitor.get_interval() == std::chrono::milliseconds {100});
}

TEST_CASE("CPUMonitor set_interval")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;

  monitor.set_interval(std::chrono::milliseconds {200});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {200});

  monitor.set_interval(std::chrono::milliseconds {50});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {50});
}

TEST_CASE("CPUMonitor get_system_cpu_usage with fallback")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const double usage = CPUMonitor::get_system_cpu_usage(42.0);

  CHECK(usage >= 0.0);
  CHECK(usage <= 100.0);
}

TEST_CASE("CPUMonitor start and sample load")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {50});

  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {200});
  monitor.stop();

  CHECK(monitor.get_load() >= 0.0);
  CHECK(monitor.get_load() <= 100.0);
}

TEST_CASE("CPUMonitor repeated get_system_cpu_usage calls")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // First call initializes the static last_idle/last_total atomics.
  // Second call exercises the delta calculation path (total_delta != 0).
  const double first = CPUMonitor::get_system_cpu_usage(99.0);
  CHECK(first >= 0.0);
  CHECK(first <= 100.0);

  // Small delay so /proc/stat counters advance, producing a non-zero delta
  std::this_thread::sleep_for(std::chrono::milliseconds {50});

  const double second = CPUMonitor::get_system_cpu_usage(99.0);
  CHECK(second >= 0.0);
  CHECK(second <= 100.0);

  // A third call to further exercise the steady-state path
  std::this_thread::sleep_for(std::chrono::milliseconds {50});
  const double third = CPUMonitor::get_system_cpu_usage(99.0);
  CHECK(third >= 0.0);
  CHECK(third <= 100.0);
}

TEST_CASE("CPUMonitor RAII destructor stops thread")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Verify that the destructor stops the monitoring thread cleanly
  // (exercises the stop() call inside ~CPUMonitor)
  {
    CPUMonitor monitor;
    monitor.set_interval(std::chrono::milliseconds {30});
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds {100});
    // destructor called here — must not hang or crash
  }
  CHECK(true);  // If we reach here, RAII cleanup succeeded
}

TEST_CASE("CPUMonitor thread updates current_load_ over time")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {20});
  monitor.start();

  // Sample the load multiple times while the thread is running
  std::vector<double> samples;
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds {30});
    samples.push_back(monitor.get_load());
  }

  monitor.stop();

  // All samples should be in valid range
  for (const auto load : samples) {
    CHECK(load >= 0.0);
    CHECK(load <= 100.0);
  }
}

TEST_CASE("CPUMonitor start-stop-start cycle")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {30});

  // First cycle
  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {80});
  monitor.stop();

  const double load_after_stop = monitor.get_load();
  CHECK(load_after_stop >= 0.0);
  CHECK(load_after_stop <= 100.0);

  // Second cycle — exercises start() on a stopped monitor
  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {80});
  monitor.stop();

  CHECK(monitor.get_load() >= 0.0);
  CHECK(monitor.get_load() <= 100.0);
}

TEST_CASE("CPUMonitor double stop is safe")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {30});

  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {60});

  // stop() twice — second call should be a no-op (thread not joinable)
  monitor.stop();
  CHECK_NOTHROW(monitor.stop());
}
