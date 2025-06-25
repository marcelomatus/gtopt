#include <random>

#include <doctest/doctest.h>
#include <gtopt/work_pool.hpp>

using namespace std::chrono_literals;

namespace gtopt
{

TEST_SUITE("WorkPool")
{
  TEST_CASE("WorkPool basic functionality")
  {
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
            const std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            counter++;
          },
          {.priority = Priority::High, .name = "high_priority_task"});

      auto medium_task = pool.submit(
          [&]
          {
            const std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
            counter++;
          },
          {.priority = Priority::Medium, .name = "medium_priority_task"});
      REQUIRE(medium_task.has_value());

      auto low_task = pool.submit(
          [&]
          {
            const std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
            counter++;
          },
          {.priority = Priority::Low, .name = "low_priority_task"});

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

#ifdef NONE
    SUBCASE("CPU monitoring affects scheduling")
    {
      // Mock CPU monitor
      class MockCPUMonitor : public CPUMonitor
      {
        [[nodiscard]] static constexpr double get_load() noexcept
        {
          return 95.0;
        }  // Simulate high load
      };

      AdaptiveWorkPool pool_with_mock;
      pool_with_mock.start();

      bool task_executed = false;
      auto future = pool_with_mock.submit([&] { task_executed = true; });

      // Task shouldn't execute immediately due to high load
      CHECK(future.wait_for(10ms) == std::future_status::timeout);
      CHECK(!task_executed);
    }
#endif

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
    constexpr int max_threads = 16;
    gtopt::WorkPoolConfig config;
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

    SUBCASE("Constexpr verification")
    {
      constexpr gtopt::WorkPoolConfig cfg {
          4,  // max_threads
          80.0,  // max_cpu_threshold
          std::chrono::milliseconds(10)  // scheduler_interval
      };
      static_assert(cfg.max_threads == 4);
      static_assert(cfg.max_cpu_threshold == 80.0);
      static_assert(cfg.scheduler_interval.count() == 10);
      CHECK(true);
    }

    SUBCASE("Noexcept verification")
    {
      const gtopt::AdaptiveWorkPool pool;
      CHECK(noexcept(pool.get_statistics()));
    }

    pool.shutdown();
  }
}

TEST_SUITE("CPUMonitor")
{
  TEST_CASE("Basic functionality")
  {
    gtopt::CPUMonitor monitor;
    
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
      const auto usage = gtopt::CPUMonitor::get_system_cpu_usage();
      CHECK(usage >= 0.0);
      CHECK(usage <= 100.0);
    }
  }

  TEST_CASE("Edge cases")
  {
    gtopt::CPUMonitor monitor;
    
    SUBCASE("Double start")
    {
      monitor.start();
      CHECK_NOTHROW(monitor.start()); // Should handle gracefully
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
    class MockCPUMonitor : public gtopt::CPUMonitor {
    public:
      void set_test_load(double load) { test_load_ = load; }
      
      [[nodiscard]] double get_load() const noexcept override {
        return test_load_;
      }

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
}

}  // namespace gtopt
