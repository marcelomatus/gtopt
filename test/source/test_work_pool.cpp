#include <gtopt/work_pool.hpp>
#include <doctest/doctest.h>

namespace gtopt::test
{
TEST_SUITE("WorkPool") {
    TEST_CASE("basic functionality") {
        AdaptiveWorkPool pool;
        pool.start();

        SUBCASE("Submit and execute simple task") {
            auto future = pool.submit([] { return 42; });
            CHECK(future.get() == 42);
        }

        SUBCASE("Task priority ordering") {
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

            CHECK(execution_order == std::vector{1, 2});
        }

        SUBCASE("CPU monitoring affects scheduling") {
            // Mock CPU monitor
            class MockCPUMonitor : public CPUMonitor {
                double get_load() const override { return 95.0; } // Simulate high load
            };

            AdaptiveWorkPool pool_with_mock;
            pool_with_mock.start();

            bool task_executed = false;
            auto future = pool_with_mock.submit([&] { task_executed = true; });

            // Task shouldn't execute immediately due to high load
            CHECK(future.wait_for(100ms) == std::future_status::timeout);
            CHECK(!task_executed);
        }

        SUBCASE("Batch submission") {
            std::vector<Task<void>> tasks;
            for (int i = 0; i < 10; ++i) {
                tasks.emplace_back([i] { /* no-op */ });
            }

            auto result = pool.submit_batch(tasks);
            CHECK(result.has_value());
            CHECK(pool.get_statistics().tasks_submitted == 10);
        }

        SUBCASE("Pending tasks generator") {
            for (int i = 0; i < 5; ++i) {
                pool.submit([] { std::this_thread::sleep_for(100ms); });
            }

            int count = 0;
            for (const auto& task : pool.pending_tasks()) {
                ++count;
            }
            CHECK(count == 5);
        }

        pool.shutdown();
    }

    TEST_CASE("stress testing") {
        AdaptiveWorkPool pool({.max_threads = 16});
        pool.start();

        SUBCASE("Submit 1000 small tasks") {
            std::vector<std::future<void>> futures;
            futures.reserve(1000);
            for (int i = 0; i < 1000; ++i) {
                futures.push_back(pool.submit([] {}));
            }
            for (auto& f : futures) f.wait();
        }

        SUBCASE("Submit 100 medium tasks") {
            std::vector<std::future<void>> futures;
            futures.reserve(100);
            for (int i = 0; i < 100; ++i) {
                futures.push_back(pool.submit([] { std::this_thread::sleep_for(10ms); }));
            }
            for (auto& f : futures) f.wait();
        }

        pool.shutdown();
    }
}
} // namespace gtopt::test
