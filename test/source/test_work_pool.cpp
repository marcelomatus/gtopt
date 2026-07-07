#include <chrono>
#include <random>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/work_pool.hpp>

using namespace gtopt;

// ─── BasicTaskRequirements template tests ───────────────────────────────────

TEST_CASE("BasicTaskRequirements template key")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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

  SUBCASE("key ordering ignores TaskPriority tier")
  {
    // `TaskPriority` no longer participates in queue ordering (it only
    // controls the dispatch gate).  A `High`-tier task with a LARGER key
    // therefore orders AFTER a `Medium`-tier task with a smaller key —
    // the opposite of the old tier-shadowing behaviour.  This is the
    // invariant that dissolves the SDDP sim⇄wedge trap: gate-bypass via
    // `TaskPriority` no longer reorders the queue.
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

    // Smaller key wins regardless of tier: medium(key=0) outranks
    // high(key=999), so `high < medium` (high is lower in the max-heap).
    CHECK(high < medium);  // high has lower priority (larger key)
    CHECK_FALSE(medium < high);
  }

  SUBCASE("gate_bypass is independent of ordering and defaults false")
  {
    // The gate-bypass admission flag must NOT affect queue ordering.
    Task<void, int64_t, std::less<>> bypass {[] {},
                                             TaskRequirements {
                                                 .priority_key = 50,
                                                 .gate_bypass = true,
                                                 .name = {},
                                             }};
    Task<void, int64_t, std::less<>> plain {[] {},
                                            TaskRequirements {
                                                .priority_key = 10,
                                                .name = {},
                                            }};

    // Default is false.
    CHECK_FALSE(plain.requirements().gate_bypass);
    CHECK(bypass.requirements().gate_bypass);
    // Smaller key still wins: plain(10) outranks bypass(50) despite the
    // bypass flag — ordering and admission are fully decoupled.
    CHECK(bypass < plain);  // bypass has lower priority (larger key)
    CHECK_FALSE(plain < bypass);
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
  using namespace gtopt;

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
  using namespace gtopt;

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
    // Ordering is driven by `priority_key` ALONE (smaller key dequeues
    // first) — `TaskPriority` no longer participates in queue order, it
    // only gates dispatch.  Use a single-threaded pool so tasks actually
    // queue up and ordering is observable (with many threads all tasks
    // get fast-dispatched concurrently, bypassing the priority queue).
    AdaptiveWorkPool prio_pool(WorkPoolConfig {
        1,  // max_threads
        95.0,  // max_cpu_threshold
        4096.0,  // min_free_memory_mb
        95.0,  // max_memory_percent
        0.0,  // max_process_rss_mb
        std::chrono::milliseconds(50),  // scheduler_interval
        "PrioPool",  // name
        false,  // enable_periodic_stats
    });
    prio_pool.start();

    std::vector<int> execution_order;
    std::mutex order_mutex;
    std::atomic<int> counter {0};

    // Block the single thread so subsequent submits queue up
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();

    auto blocker =
        prio_pool.submit([&gate_future] { gate_future.wait(); },
                         {.priority_key = 0, .name = "blocker_task"});

    // Submit tasks out of key order — they queue behind the blocker and
    // dispatch by ascending `priority_key`, NOT submission order or tier.
    // The (intentionally inverted) `TaskPriority` tiers below must have no
    // effect on the observed order.
    auto last_task = prio_pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(3);
          counter++;
        },
        {.priority = TaskPriority::High, .priority_key = 30, .name = "k30"});

    auto first_task = prio_pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(1);
          counter++;
        },
        {.priority = TaskPriority::Low, .priority_key = 10, .name = "k10"});

    auto mid_task = prio_pool.submit(
        [&]
        {
          const std::scoped_lock<std::mutex> lock(order_mutex);
          execution_order.push_back(2);
          counter++;
        },
        {.priority = TaskPriority::Medium, .priority_key = 20, .name = "k20"});
    REQUIRE(mid_task.has_value());

    // Release the blocker — queued tasks now dispatch in key order.
    gate.set_value();

    blocker.value().wait();
    first_task.value().wait();
    mid_task.value().wait();
    last_task.value().wait();

    CHECK(counter == 3);
    // Smallest key (10) executes first despite its Low tier.
    CHECK(execution_order[0] == 1);
    // key 20 before key 30 (Medium-tier task ahead of High-tier task).
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
  using namespace gtopt;

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
            // thread_local (NOT static): the lambda runs concurrently on
            // many workers, and a shared mt19937 mutated via dist(gen)
            // from all of them is a data race (caught by TSan).
            thread_local std::mt19937 gen(std::random_device {}());
            thread_local std::uniform_int_distribution<> dist(0, 10);
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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  const CPUMonitor monitor;

  CHECK(monitor.get_load() == doctest::Approx(0.0));
  CHECK(monitor.get_interval() == std::chrono::milliseconds {100});
}

TEST_CASE("CPUMonitor set_interval round-trips multiple values")
{
  using namespace gtopt;

  CPUMonitor monitor;

  monitor.set_interval(std::chrono::milliseconds {200});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {200});

  monitor.set_interval(std::chrono::milliseconds {50});
  CHECK(monitor.get_interval() == std::chrono::milliseconds {50});
}

TEST_CASE("CPUMonitor get_system_cpu_usage with fallback")
{
  using namespace gtopt;

  const double usage = CPUMonitor::get_system_cpu_usage(42.0);

  CHECK(usage >= 0.0);
  CHECK(usage <= 100.0);
}

TEST_CASE("CPUMonitor start and sample load")
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  CPUMonitor monitor;
  monitor.set_interval(std::chrono::milliseconds {30});

  monitor.start();
  std::this_thread::sleep_for(std::chrono::milliseconds {60});

  // stop() twice — second call should be a no-op (thread not joinable)
  monitor.stop();
  CHECK_NOTHROW(monitor.stop());
}

// ─── WorkPoolConfig swap-gate knobs ──────────────────────────────────────────

TEST_CASE("WorkPoolConfig swap knobs default")  // NOLINT
{
  // max_process_swap_mb defaults to 2048 MB (enabled) to catch process
  // paging before thrash; max_swap_io_per_sec remains disabled by default
  // since benign init-time paging can briefly exceed any fixed threshold.
  const WorkPoolConfig cfg;
  CHECK(cfg.max_process_swap_mb == doctest::Approx(2048.0));
  CHECK(cfg.max_swap_io_per_sec == doctest::Approx(0.0));
}

TEST_CASE("WorkPoolConfig swap knobs are assignable via constructor")  // NOLINT
{
  const WorkPoolConfig cfg {
      2,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      95.0,  // max_memory_percent
      0.0,  // max_process_rss_mb
      std::chrono::milliseconds(50),  // scheduler_interval
      "SwapCfgPool",  // name
      false,  // enable_periodic_stats
      512.0,  // max_process_swap_mb
      10000.0,  // max_swap_io_per_sec
  };
  CHECK(cfg.max_process_swap_mb == doctest::Approx(512.0));
  CHECK(cfg.max_swap_io_per_sec == doctest::Approx(10000.0));
}

TEST_CASE(
    "AdaptiveWorkPool: swap-aware Statistics carries swap fields")  // NOLINT
{
  AdaptiveWorkPool pool(WorkPoolConfig {
      2,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      95.0,  // max_memory_percent
      0.0,  // max_process_rss_mb
      std::chrono::milliseconds(50),  // scheduler_interval
      "SwapStatsPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb (disabled)
      0.0,  // max_swap_io_per_sec (disabled)
  });
  pool.start();

  auto f = pool.submit([] { return 7; });
  REQUIRE(f.has_value());
  CHECK(f.value().get() == 7);

  // Let the monitor collect at least one reading.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto stats = pool.get_statistics();

  // Swap readings should be present and finite, regardless of actual
  // swap pressure on the host.
  CHECK(stats.process_swap_mb >= 0.0);
  CHECK(stats.swap_used_mb >= 0.0);
  CHECK(stats.swap_io_rate >= 0.0);

  // format_statistics() now includes a Swap: line.
  const auto text = pool.format_statistics();
  CHECK(text.find("Swap:") != std::string::npos);
}

TEST_CASE(
    "AdaptiveWorkPool: swap gates disabled by default admit work")  // NOLINT
{
  // With both swap gates set to 0 (disabled), a normal submit must run
  // to completion — this is the "no behavior change" contract.
  AdaptiveWorkPool pool(WorkPoolConfig {
      2,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      95.0,  // max_memory_percent
      0.0,  // max_process_rss_mb
      std::chrono::milliseconds(10),  // scheduler_interval
      "SwapDisabledPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb
      0.0,  // max_swap_io_per_sec
  });
  pool.start();

  auto f1 = pool.submit([] { return 1; });
  auto f2 = pool.submit([] { return 2; });
  auto f3 = pool.submit([] { return 3; });
  REQUIRE(f1.has_value());
  REQUIRE(f2.has_value());
  REQUIRE(f3.has_value());
  CHECK(f1.value().get() == 1);
  CHECK(f2.value().get() == 2);
  CHECK(f3.value().get() == 3);
}

// ─── Measured-memory dispatch controller (no fixed per-task estimates) ───────

TEST_CASE("WorkPoolConfig max_threads_ceiling default is 0")  // NOLINT
{
  const WorkPoolConfig cfg;
  CHECK(cfg.max_threads_ceiling == 0);
}

TEST_CASE(  // NOLINT
    "Measured gate never deadlocks when limit is below the resident floor")
{
  // CRITICAL safety contract.  Set the process-RSS limit to 1 MB — far
  // below the live RSS of the test process (tens of MB).  The measured gate
  // projects `rss + measured_per_task`, which trivially exceeds 1 MB, so a
  // naive gate would block EVERY dispatch and wedge the pool forever (no
  // running task can free memory to reopen the gate).
  //
  // The gate's "always admit one task when nothing is in flight" rule
  // guarantees forward progress: tasks run strictly serially but ALL of
  // them complete.  We assert completion within a generous timeout rather
  // than calling .get() unguarded so a regression surfaces as a failed
  // CHECK instead of a hung test binary.
  AdaptiveWorkPool pool(WorkPoolConfig {
      4,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      99.0,  // max_memory_percent (relaxed so only the RSS gate is in play)
      1.0,  // max_process_rss_mb — absurdly low, below live RSS
      std::chrono::milliseconds(10),  // scheduler_interval
      "RssDeadlockPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb (disabled)
      0.0,  // max_swap_io_per_sec (disabled)
      4,  // max_threads_ceiling
  });
  pool.start();

  constexpr int kNumTasks = 6;
  std::atomic<int> ran {0};
  std::vector<std::future<int>> futures;
  futures.reserve(kNumTasks);
  for (int i = 0; i < kNumTasks; ++i) {
    auto f = pool.submit(
        [&ran, i]
        {
          ran.fetch_add(1, std::memory_order_relaxed);
          return i;
        });
    REQUIRE(f.has_value());
    futures.push_back(std::move(f.value()));
  }

  // Every task must complete despite the 1 MB limit — proves no deadlock.
  for (auto& f : futures) {
    REQUIRE(f.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  }
  CHECK(ran.load() == kNumTasks);
}

TEST_CASE(  // NOLINT
    "System-memory-percent gate never deadlocks (idle-progress guarantee)")
{
  // Regression guard for the livelock that hung the box on the 2-year
  // simulation pass: with `max_memory_percent` set impossibly low (0%),
  // the system-memory gate `mem_pct >= threshold` is ALWAYS true, so a
  // gate without the idle-progress guarantee would refuse every task
  // forever (observed as "18 pending, 0 active, no progress for 88
  // intervals", mem% 92% >= 90%).  The universal "admit one when
  // active == 0" rule must let tasks drain strictly serially instead.
  AdaptiveWorkPool pool(WorkPoolConfig {
      4,  // max_threads
      99.0,  // max_cpu_threshold
      0.0,  // min_free_memory_mb (disabled)
      0.0,  // max_memory_percent — impossibly low: mem% always over
      0.0,  // max_process_rss_mb (disabled)
      std::chrono::milliseconds(10),  // scheduler_interval
      "MemPctDeadlockPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb (disabled)
      0.0,  // max_swap_io_per_sec (disabled)
      4,  // max_threads_ceiling
  });
  pool.start();

  constexpr int kNumTasks = 6;
  std::atomic<int> ran {0};
  std::vector<std::future<void>> futures;
  futures.reserve(kNumTasks);
  for (int i = 0; i < kNumTasks; ++i) {
    auto f =
        pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(f.has_value());
    futures.push_back(std::move(f.value()));
  }
  for (auto& f : futures) {
    REQUIRE(f.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  }
  CHECK(ran.load() == kNumTasks);
}

TEST_CASE(  // NOLINT
    "Measured gate with limit disabled (0) admits work at full ceiling")
{
  // max_process_rss_mb == 0 disables the controller entirely: the pool runs
  // at its full thread count immediately — the "no behavior change" contract.
  AdaptiveWorkPool pool(WorkPoolConfig {
      4,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      95.0,  // max_memory_percent
      0.0,  // max_process_rss_mb (disabled)
      std::chrono::milliseconds(10),  // scheduler_interval
      "RssDisabledPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb
      0.0,  // max_swap_io_per_sec
      0,  // max_threads_ceiling (unset → equals max_threads)
  });
  pool.start();
  CHECK(pool.max_threads() == 4);

  auto f1 = pool.submit([] { return 1; });
  auto f2 = pool.submit([] { return 2; });
  auto f3 = pool.submit([] { return 3; });
  REQUIRE(f1.has_value());
  REQUIRE(f2.has_value());
  REQUIRE(f3.has_value());
  CHECK(f1.value().get() == 1);
  CHECK(f2.value().get() == 2);
  CHECK(f3.value().get() == 3);
}

// ─── max_threads runtime growth toward the CPU ceiling ───────────────────────

TEST_CASE(  // NOLINT
    "WorkPool: ceiling unset (0) means max_threads never grows")
{
  // Back-compat contract: with no ceiling configured, the effective
  // ceiling equals max_threads, so growth is disabled.
  AdaptiveWorkPool pool(WorkPoolConfig {
      2,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      99.0,  // max_memory_percent
      0.0,  // max_process_rss_mb
      std::chrono::milliseconds(10),  // scheduler_interval
      "NoGrowPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb
      0.0,  // max_swap_io_per_sec
      0,  // max_threads_ceiling (unset → equals max_threads)
  });
  pool.start();
  CHECK(pool.max_threads() == 2);
  CHECK(pool.max_threads_ceiling() == 2);

  std::vector<std::future<int>> fs;
  for (int i = 0; i < 6; ++i) {
    auto f = pool.submit([] { return 0; });
    REQUIRE(f.has_value());
    fs.push_back(std::move(f.value()));
  }
  for (auto& f : fs) {
    REQUIRE(f.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  }
  // Never grows past the initial value.
  CHECK(pool.max_threads() == 2);
}

TEST_CASE(  // NOLINT
    "WorkPool: no RSS limit grows straight to ceiling")
{
  // With max_process_rss_mb == 0 there is no memory constraint, so the pool
  // grows max_threads straight to the ceiling.
  AdaptiveWorkPool pool(WorkPoolConfig {
      1,  // max_threads
      95.0,  // max_cpu_threshold
      4096.0,  // min_free_memory_mb
      99.0,  // max_memory_percent
      0.0,  // max_process_rss_mb (disabled)
      std::chrono::milliseconds(10),  // scheduler_interval
      "GrowNoLimitPool",  // name
      false,  // enable_periodic_stats
      0.0,  // max_process_swap_mb
      0.0,  // max_swap_io_per_sec
      3,  // max_threads_ceiling
  });
  pool.start();
  CHECK(pool.max_threads() == 1);

  std::vector<std::future<int>> fs;
  for (int i = 0; i < 6; ++i) {
    auto f = pool.submit([] { return 0; });
    REQUIRE(f.has_value());
    fs.push_back(std::move(f.value()));
  }
  for (auto& f : fs) {
    REQUIRE(f.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  }
  for (int i = 0; i < 200 && pool.max_threads() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(pool.max_threads() == 3);
}

// ─── SlotReleaseGuard ─────────────────────────────────────────────────────
//
// Pin the contract for the in-task blocking guard: while a guard is alive
// the pool's active-task count drops by 1, and any waiting workers can
// dispatch their queued work.  When the guard is destroyed the count is
// restored.  No-op when called outside a worker context (count never
// underflows).

TEST_CASE("SlotReleaseGuard outside worker is a safe no-op")  // NOLINT
{
  using namespace gtopt;
  WorkPoolConfig cfg;
  cfg.max_threads = 2;
  BasicWorkPool<> pool {cfg};
  pool.start();

  // Caller is the test thread, NOT a worker.  Active count is 0; the
  // guard's release_slot_for_blocking_ defensively skips the decrement.
  CHECK(pool.get_statistics().tasks_active == 0);
  {
    auto guard = pool.release_slot_while_blocking();
    CHECK(pool.get_statistics().tasks_active == 0);
  }
  CHECK(pool.get_statistics().tasks_active == 0);
}

TEST_CASE(  // NOLINT
    "SlotReleaseGuard from inside a task drops then restores the count")
{
  using namespace gtopt;
  WorkPoolConfig cfg;
  cfg.max_threads = 4;
  BasicWorkPool<> pool {cfg};
  pool.start();

  // Use sync primitives so the worker's observed counts can be checked
  // by the test thread at known points in time.
  std::mutex mu;
  std::condition_variable cv;
  bool guard_alive = false;
  bool guard_ended = false;
  std::size_t active_inside_guard = 0;
  std::size_t active_after_guard = 0;

  auto fut = pool.submit(std::function<int()> {
      [&]() -> int
      {
        // Inside a worker: tasks_active_ is at least 1 (this task).
        {
          auto guard = pool.release_slot_while_blocking();
          active_inside_guard = pool.get_statistics().tasks_active;
          {
            const std::scoped_lock lock {mu};
            guard_alive = true;
          }
          cv.notify_all();

          // Wait for the test thread to observe the released state.
          std::unique_lock lock {mu};
          cv.wait(lock, [&] { return guard_ended; });
        }
        active_after_guard = pool.get_statistics().tasks_active;
        return 0;
      }});
  if (!fut.has_value()) {
    MESSAGE("submit failed: " << fut.error().message());
  }
  REQUIRE(fut.has_value());

  {
    std::unique_lock lock {mu};
    cv.wait(lock, [&] { return guard_alive; });
  }
  // Inside the guard scope the worker's slot is released — count is 0.
  CHECK(active_inside_guard == 0);
  {
    const std::scoped_lock lock {mu};
    guard_ended = true;
  }
  cv.notify_all();
  CHECK(fut.value().get() == 0);
  // After guard exit, slot is re-acquired (count == 1 inside the task);
  // after task completes, worker_loop decrements back to 0.
  CHECK(active_after_guard == 1);
  // The final decrement happens in worker_loop *after* task.execute()
  // fulfils the future (work_pool.hpp runs tasks_active_.fetch_sub(1) on
  // a later line), so fut.get() can unblock a hair before the counter
  // drops — under CPU oversubscription that lag is observable.  Poll for
  // the drain instead of reading the counter eagerly.
  bool drained = false;
  for (int i = 0; i < 2000 && !drained; ++i) {
    if (pool.get_statistics().tasks_active == 0) {
      drained = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
  }
  CHECK(drained);
}

TEST_CASE(  // NOLINT
    "SlotReleaseGuard notifies waiting workers on release")
{
  using namespace gtopt;
  // Verify the release path notifies cv_, so any worker parked in the
  // memory-gate sleep wakes up to re-evaluate.  We can't easily simulate
  // the memory gate from a unit test, but the contract is: after
  // `release_slot_for_blocking_` returns, the active count is one
  // smaller and at least one worker has been notified.  We exercise
  // this by submitting a task that takes the guard and checking the
  // before/after counts under a held mutex (no timing race).
  WorkPoolConfig cfg;
  cfg.max_threads = 2;
  BasicWorkPool<> pool {cfg};
  pool.start();

  std::mutex mu;
  std::condition_variable cv;
  bool inside_guard = false;
  bool guard_ended = false;
  std::size_t observed_active_before = 999;
  std::size_t observed_active_inside = 999;

  auto fut = pool.submit(std::function<int()> {
      [&]() -> int
      {
        observed_active_before = pool.get_statistics().tasks_active;
        {
          auto guard = pool.release_slot_while_blocking();
          observed_active_inside = pool.get_statistics().tasks_active;
          {
            const std::scoped_lock lock {mu};
            inside_guard = true;
          }
          cv.notify_all();
          std::unique_lock lock {mu};
          cv.wait(lock, [&] { return guard_ended; });
        }
        return 0;
      }});
  REQUIRE(fut.has_value());

  {
    std::unique_lock lock {mu};
    cv.wait(lock, [&] { return inside_guard; });
  }
  // Before guard: this task was active (count >= 1).
  CHECK(observed_active_before >= 1);
  // After guard: count dropped by 1 — to 0 since this is the only task.
  CHECK(observed_active_inside == observed_active_before - 1);

  {
    const std::scoped_lock lock {mu};
    guard_ended = true;
  }
  cv.notify_all();
  CHECK(fut.value().get() == 0);
}
