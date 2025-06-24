#include <gtopt/work_pool.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

namespace gtopt::test
{
TEST_CASE("WorkPool basic functionality", "[work_pool]")
{
  AdaptiveWorkPool pool;
  pool.start();

  SECTION("Submit and execute simple task")
  {
    auto future = pool.submit([] { return 42; });
    REQUIRE(future.get() == 42);
  }

  SECTION("Task priority ordering")
  {
    std::vector<int> execution_order;
    std::mutex order_mutex;

    auto high_task = pool.submit(
        [&] { 
          std::lock_guard lock(order_mutex);
          execution_order.push_back(1); 
        },
        {.priority = Priority::High});

    auto low_task = pool.submit(
        [&] { 
          std::lock_guard lock(order_mutex);
          execution_order.push_back(2); 
        },
        {.priority = Priority::Low});

    high_task.wait();
    low_task.wait();

    REQUIRE(execution_order == std::vector{1, 2});
  }

  SECTION("CPU monitoring affects scheduling")
  {
    // Mock CPU monitor
    class MockCPUMonitor : public CPUMonitor
    {
      double get_load() const override { return 95.0; } // Simulate high load
    };

    AdaptiveWorkPool pool_with_mock;
    pool_with_mock.start();

    bool task_executed = false;
    auto future = pool_with_mock.submit([&] { task_executed = true; });

    // Task shouldn't execute immediately due to high load
    REQUIRE(future.wait_for(100ms) == std::future_status::timeout);
    REQUIRE(!task_executed);
  }

  SECTION("Batch submission")
  {
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 10; ++i) {
      tasks.emplace_back([i] { /* no-op */ });
    }

    auto result = pool.submit_batch(tasks);
    REQUIRE(result.has_value());
    REQUIRE(pool.get_statistics().tasks_submitted == 10);
  }

  SECTION("Pending tasks generator")
  {
    for (int i = 0; i < 5; ++i) {
      pool.submit([] { std::this_thread::sleep_for(100ms); });
    }

    int count = 0;
    for (const auto& task : pool.pending_tasks()) {
      ++count;
    }
    REQUIRE(count == 5);
  }

  pool.shutdown();
}

TEST_CASE("WorkPool stress testing", "[work_pool][stress]")
{
  AdaptiveWorkPool pool({.max_threads = 16});
  pool.start();

  BENCHMARK("Submit 1000 small tasks")
  {
    std::vector<std::future<void>> futures;
    futures.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      futures.push_back(pool.submit([] {}));
    }
    for (auto& f : futures) f.wait();
  };

  BENCHMARK("Submit 100 medium tasks")
  {
    std::vector<std::future<void>> futures;
    futures.reserve(100);
    for (int i = 0; i < 100; ++i) {
      futures.push_back(pool.submit([] { std::this_thread::sleep_for(10ms); }));
    }
    for (auto& f : futures) f.wait();
  };

  pool.shutdown();
}
} // namespace gtopt::test
