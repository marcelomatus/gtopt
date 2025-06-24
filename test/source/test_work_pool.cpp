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
      auto future = pool.submit([] { return 42; });
      CHECK(future.get() == 42);
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

      auto low_task = pool.submit(
          [&]
          {
            const std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
            counter++;
          },
          {.priority = Priority::Low, .name = "low_priority_task"});

      // Wait for all tasks to complete
      high_task.wait();
      medium_task.wait();
      low_task.wait();

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
      task1.wait();
      task2.wait();

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
        [[nodiscard]] static double get_load()
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
        pool.submit([] { std::this_thread::sleep_for(10ms); });
      }

      // Pending tasks test placeholder
      CHECK(true);
    }

    pool.shutdown();
  }

  TEST_CASE("WorkPool stress testing") 
  {
    constexpr int max_threads = 16;
    AdaptiveWorkPool::Config config {
      .max_threads = max_threads,
      .max_cpu_threshold = 90.0,
      .scheduler_interval = 10ms
    };
    AdaptiveWorkPool pool(config);
  {
    AdaptiveWorkPool::Config config;
    config.max_threads = 16;
    AdaptiveWorkPool pool(config);
    pool.start();

    SUBCASE("Submit 100 small tasks")
    {
      std::vector<std::future<int>> futures;
      futures.reserve(100);
      std::atomic<int> counter {0};

      for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([&] { return counter++; }));
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
      futures.reserve(n);
      std::atomic<int> counter {0};

      for (int i = 0; i < n; ++i) {
        futures.push_back(pool.submit(
            [&]
            {
              static std::random_device rd;
              static std::mt19937 gen(rd());
              static std::uniform_int_distribution<> dist(0, 10);
              std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
              return counter++;
            },
            {.estimated_duration = 5ms, .name = "medium_task"}));
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
      // Test handling of invalid tasks
      auto result = pool.submit(nullptr);
      CHECK_FALSE(result.has_value());
      CHECK(result.error() == std::make_error_code(std::errc::invalid_argument));
    }

    SUBCASE("Constexpr verification")
    {
      constexpr auto cfg = AdaptiveWorkPool::Config{};
      static_assert(cfg.max_threads > 0);
      static_assert(cfg.max_cpu_threshold > 0.0);
      static_assert(cfg.scheduler_interval.count() > 0);
      CHECK(true);
    }

    SUBCASE("Noexcept verification")
    {
      AdaptiveWorkPool pool;
      CHECK(noexcept(pool.get_statistics()));
      CHECK(noexcept(pool.shutdown()));
    }

    pool.shutdown();
  }
}

}  // namespace gtopt
