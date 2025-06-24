#include <gtopt/work_pool.hpp>

namespace example
{
using namespace gtopt;

namespace
{
void cpu_intensive_task(const std::string& name, int duration_seconds)
{
  SPDLOG_INFO(std::format("Starting CPU intensive task: ", name));

  auto start = std::chrono::steady_clock::now();
  auto end = start + std::chrono::seconds(duration_seconds);

  volatile int64_t counter = 0;
  while (std::chrono::steady_clock::now() < end) {
    for (int i = 0; i < 1'000'000; ++i) {
      counter += static_cast<int64_t>(i) * i;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  SPDLOG_INFO(std::format("Completed CPU intensive task: {}", name));
}

void multi_threaded_task(const std::string& name,
                         int num_threads,
                         int duration_seconds)
{
  SPDLOG_INFO(std::format("Starting multi-threaded task: {}", name));

  std::vector<std::jthread> threads;
  threads.reserve(num_threads);

  std::atomic<bool> stop_flag {false};

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(
        [&stop_flag](const std::stop_token& stoken)
        {
          volatile int64_t counter = 0;
          while (!stop_flag && !stoken.stop_requested()) {
            for (int j = 0; j < 100'000; ++j) {
              counter += j;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  stop_flag = true;

  SPDLOG_INFO(std::format("Completed multi-threaded task: ", name));
}

void run_example()
{
  const AdaptiveWorkPool::Config config {
      4, 80.0, 50.0, std::chrono::milliseconds(100)};

  AdaptiveWorkPool pool(config);
  pool.start();

  std::vector<std::future<void>> futures;

  futures.push_back(pool.submit(
      [](const std::string& name, int duration)
      { cpu_intensive_task(name, duration); },
      TaskRequirements {
          .estimated_threads = 1,
          .estimated_duration = std::chrono::seconds(2),
          .priority = Priority::Critical,
          .name = std::optional<std::string> {"Critical Processing"}},
      "Critical Task",
      2));

  for (int i = 0; i < 2; ++i) {
    const std::string task_name = "MultiTask-" + std::to_string(i);
    futures.push_back(pool.submit(
        [](const std::string& name, int threads, int duration)
        { multi_threaded_task(name, threads, duration); },
        TaskRequirements {
            .estimated_threads = 2,
            .estimated_duration = std::chrono::seconds(3),
            .priority = Priority::Medium,
            .name =
                std::optional<std::string> {"Multi-Task-" + std::to_string(i)}},
        task_name,
        2,
        3));
  }

  for (int i = 0; i < 3; ++i) {
    futures.push_back(pool.submit_lambda(
        [&i]()
        {
          SPDLOG_INFO(std::format("Light task {} executing", i));
          std::this_thread::sleep_for(std::chrono::seconds(1));
          SPDLOG_INFO(std::format("Light task {} completed", i));
        },
        TaskRequirements {.estimated_threads = 1,
                          .estimated_duration = std::chrono::seconds(1),
                          .priority = Priority::Low,
                          .name = std::optional<std::string> {
                              "Light-Task-" + std::to_string(i)}}));
  }

  auto monitor_future =
      std::async(std::launch::async,
                 [&pool]()
                 {
                   for (int i = 0; i < 15; ++i) {
                     std::this_thread::sleep_for(std::chrono::seconds(1));
                     pool.print_statistics();
                   }
                 });

  for (auto& future : futures) {
    future.wait();
  }

  monitor_future.wait();
  pool.print_statistics();

  SPDLOG_INFO("All tasks completed!");
}

int main()
{
  try {
    example::run_example();
  } catch (const std::exception& e) {
    SPDLOG_ERROR(std::format("Error: {}", e.what()));
    return 1;
  }

  return 0;
}
}  // namespace
}  // namespace example
