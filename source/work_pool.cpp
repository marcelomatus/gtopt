#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <utility>  // for std::move
#include <vector>

#ifdef __linux__
#  include <fstream>
#elif defined(_WIN32)
#  include <windows.h>
#endif

namespace work_pool
{

// Strong types for better type safety
enum class [[nodiscard]] TaskStatus : uint8_t
{
  Success,
  Failed,
  Cancelled
};

[[nodiscard]] constexpr auto to_string(TaskStatus status) noexcept
    -> std::string_view
{
  switch (status) {
    case TaskStatus::Success:
      return "Success";
    case TaskStatus::Failed:
      return "Failed";
    case TaskStatus::Cancelled:
      return "Cancelled";
    default:
      return "Unknown";
  }
}  // namespace work_pool

// Task priority and resource requirements
enum class Priority : uint8_t
{
  Low = 0,
  Medium = 1,
  High = 2,
  Critical = 3
};

struct TaskRequirements
{
  int estimated_threads = 1;
  std::chrono::milliseconds estimated_duration {1000};
  Priority priority = Priority::Medium;
  std::optional<std::string> name;
};

// CPU monitoring with platform-specific implementations
class CPUMonitor
{
public:
  CPUMonitor() = default;
  CPUMonitor(const CPUMonitor&) = delete;
  CPUMonitor& operator=(const CPUMonitor&) = delete;
  CPUMonitor(CPUMonitor&&) = delete;
  CPUMonitor& operator=(CPUMonitor&&) = delete;

private:
  std::atomic<double> current_load_ {0.0};
  std::atomic<bool> running_ {false};
  std::thread monitor_thread_;

  static double get_system_cpu_usage()
  {
#ifdef __linux__
    static uint64_t last_idle = 0;
    static uint64_t last_total = 0;

    std::ifstream proc_stat("/proc/stat");
    if (!proc_stat) {
      return 50.0;  // fallback

      std::string line;
      std::getline(proc_stat, line);

      std::istringstream ss(line);
      std::string cpu_name;
      ss >> cpu_name;

      std::vector<uint64_t> times;
      uint64_t time = 0;
      while (ss >> time) {
        times.push_back(time);
      }

      if (times.size() >= 4) {
        auto idle = times[3];
        auto total =
            std::accumulate(times.begin(), times.end(), 0ULL);  // NOLINT

        auto idle_delta = idle - last_idle;
        auto total_delta = total - last_total;

        last_idle = idle;
        last_total = total;

        if (total_delta > 0) {
          return 100.0 * (1.0 - static_cast<double>(idle_delta) / total_delta);
        }
      }
      return 0.0;

#elif defined(_WIN32)
    // Windows implementation using PDH
    PDH_HQUERY cpuQuery;
    PDH_HCOUNTER cpuTotal;
    PDH_FMT_COUNTERVALUE counterVal;

    PdhOpenQuery(nullptr, nullptr, &cpuQuery);
    PdhAddCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", 0, &cpuTotal);
    PdhCollectQueryData(cpuQuery);
    Sleep(100);
    PdhCollectQueryData(cpuQuery);
    PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, nullptr, &counterVal);
    PdhCloseQuery(cpuQuery);

    return counterVal.doubleValue;
#else
    // Fallback for other platforms or online compilers
    return 30.0 + (rand() % 40);  // Simulate varying load
#endif
  }

public:
  ~CPUMonitor() { stop(); }

  void start()
  {
    running_ = true;
    monitor_thread_ = std::thread(
        [this]()
        {
          while (running_) {
            current_load_ = get_system_cpu_usage();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
        });
  }

  void stop()
  {
    running_ = false;
    if (monitor_thread_.joinable()) {
      monitor_thread_.join();
    }
  }

  [[nodiscard]] double get_load() const noexcept
  {
    return current_load_.load();
  }
};

// Task wrapper with metadata
template<typename T = void>
class Task
{
public:
  using result_type = T;

private:
  std::packaged_task<T()> task_;
  TaskRequirements requirements_;
  std::chrono::steady_clock::time_point submit_time_;

public:
  template<typename F>
  explicit Task(F&& func, TaskRequirements req = {})
      : task_(std::forward<F>(func))
      , requirements_(std::move(req))
      , submit_time_(std::chrono::steady_clock::now())
  {
  }

  std::future<T> get_future() { return task_.get_future(); }

  void execute() { task_(); }

  [[nodiscard]] const TaskRequirements& requirements() const noexcept
  {
    return requirements_;
  }

  [[nodiscard]] auto age() const noexcept
  {
    return std::chrono::steady_clock::now() - submit_time_;
  }

  // Priority comparison for priority queue
  bool operator<(const Task& other) const noexcept
  {
    if (requirements_.priority != other.requirements_.priority) {
      return requirements_.priority < other.requirements_.priority;
    }
    return submit_time_
        > other.submit_time_;  // Older tasks have higher priority
  }
};

// Active task tracking
class ActiveTask
{
private:
  std::future<void> future_;
  TaskRequirements requirements_;
  std::chrono::steady_clock::time_point start_time_;

public:
  ActiveTask(std::future<void> fut, TaskRequirements req)
      : future_(std::move(fut))
      , requirements_(std::move(req))
      , start_time_(std::chrono::steady_clock::now())
  {
  }

  [[nodiscard]] bool is_ready() const
  {
    return future_.wait_for(std::chrono::seconds(0))
        == std::future_status::ready;
  }

  void wait() { future_.wait(); }

  [[nodiscard]] const TaskRequirements& requirements() const noexcept
  {
    return requirements_;
  }

  [[nodiscard]] auto runtime() const noexcept
  {
    return std::chrono::steady_clock::now() - start_time_;
  }
};

// Main adaptive work pool
class AdaptiveWorkPool
{
private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::priority_queue<Task<void>> task_queue_;
  std::vector<std::unique_ptr<ActiveTask>> active_tasks_;

  CPUMonitor cpu_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};
  std::thread scheduler_thread_;

  // Configuration
  int max_threads_ {1000};
  double max_cpu_threshold_;
  double min_cpu_threshold_;
  std::chrono::milliseconds scheduler_interval_;

  // Statistics
  std::atomic<size_t> tasks_completed_ {0};
  std::atomic<size_t> tasks_submitted_ {0};

public:
  struct Config
  {
    int max_threads {1000};
    double max_cpu_threshold {85.0};
    double min_cpu_threshold {60.0};
    std::chrono::milliseconds scheduler_interval {50};
  };

  explicit AdaptiveWorkPool(Config config)
      : max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , min_cpu_threshold_(config.min_cpu_threshold)
      , scheduler_interval_(config.scheduler_interval)
  {
    std::cout << "AdaptiveWorkPool initialized with " << max_threads_
              << " max threads, CPU thresholds: " << config.min_cpu_threshold
              << "%-" << config.max_cpu_threshold << "%\n";
  }

  ~AdaptiveWorkPool() { shutdown(); }

  void start()
  {
    if (running_.exchange(true)) {
      return;  // Already running
    }

    try {
      cpu_monitor_.start();
      scheduler_thread_ = std::thread([this] { scheduler_loop(); });
      std::cout << "AdaptiveWorkPool started with " << max_threads_
                << " max threads\n";
    } catch (const std::exception& e) {
      running_ = false;
      throw std::runtime_error(std::string("WorkPool start failed: ")
                               + e.what());
    }
  }

  void shutdown()
  {
    if (!running_)
      return;

    running_ = false;
    cv_.notify_all();

    if (scheduler_thread_.joinable()) {
      scheduler_thread_.join();
    }

    // Wait for active tasks to complete
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& task : active_tasks_) {
        task->wait();
      }
      active_tasks_.clear();
    }

    cpu_monitor_.stop();
    std::cout << "AdaptiveWorkPool shutdown complete\n";
  }

  template<typename F, typename... Args>
  auto submit(F&& func, TaskRequirements req = {}, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>>
  {
    using ReturnType = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(func), args...]() mutable
        { return std::invoke(func, args...); });

    auto future = task->get_future();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if constexpr (std::is_void_v<ReturnType>) {
        task_queue_.emplace([task]() { (*task)(); }, std::move(req));
      } else {
        task_queue_.emplace([task]() { (*task)(); }, std::move(req));
      }
      tasks_submitted_++;
    }

    cv_.notify_one();
    return future;
  }

  // Convenience method for lambda submission
  template<typename F>
  auto submit_lambda(F&& func, TaskRequirements req = {})
  {
    return submit(std::forward<F>(func), std::move(req));
  }

  struct Statistics
  {
    size_t tasks_submitted;
    size_t tasks_completed;
    size_t tasks_pending;
    size_t tasks_active;
    int active_threads;
    double current_cpu_load;
  };

  Statistics get_statistics() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return Statistics {tasks_submitted_.load(),
                       tasks_completed_.load(),
                       task_queue_.size(),
                       active_tasks_.size(),
                       active_threads_.load(),
                       cpu_monitor_.get_load()};
  }

  void print_statistics() const
  {
    auto stats = get_statistics();
    std::cout << "=== WorkPool Statistics ===\n";
    std::cout << "Tasks: " << stats.tasks_submitted << " submitted, "
              << stats.tasks_completed << " completed, " << stats.tasks_pending
              << " pending, " << stats.tasks_active << " active\n";
    std::cout << "Threads: " << stats.active_threads << " active / "
              << max_threads_ << " max\n";
    std::cout << "CPU Load: " << stats.current_cpu_load << "%\n\n";
  }

private:
  void scheduler_loop()
  {
    while (running_) {
      cleanup_completed_tasks();

      if (should_schedule_new_task()) {
        schedule_next_task();
      }

      std::this_thread::sleep_for(scheduler_interval_);
    }
  }

  void cleanup_completed_tasks()
  {
    std::lock_guard<std::mutex> lock(mutex_);

    active_tasks_.erase(
        std::remove_if(active_tasks_.begin(),
                       active_tasks_.end(),
                       [this](const auto& task)
                       {
                         if (task->is_ready()) {
                           active_threads_ -=
                               task->requirements().estimated_threads;
                           tasks_completed_++;
                           return true;
                         }
                         return false;
                       }),
        active_tasks_.end());
  }

  [[nodiscard]] bool should_schedule_new_task() const
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.top();
    const double cpu_load = cpu_monitor_.get_load();
    const unsigned int threads_needed =
        static_cast<unsigned int>(next_task.requirements().estimated_threads);
    const unsigned int current_threads =
        static_cast<unsigned int>(active_threads_.load());

    // Check thread capacity
    if (current_threads + threads_needed
        > static_cast<unsigned int>(max_threads_))
    {
      return false;
    }

    // Adaptive CPU scheduling based on priority
    double threshold = max_cpu_threshold_;
    switch (next_task.requirements().priority) {
      case Priority::Critical:
        threshold = 95.0;  // Allow critical tasks even at high CPU
        break;
      case Priority::High:
        threshold = max_cpu_threshold_ + 5.0;
        break;
      default:
        break;
    }

    return cpu_load < threshold;
  }

  void schedule_next_task()
  {
    std::unique_lock<std::mutex> lock(mutex_);

    if (task_queue_.empty()) {
      return;
    }

    auto task = std::move(const_cast<Task<void>&>(task_queue_.top()));
    task_queue_.pop();

    const auto threads_needed =
        static_cast<unsigned int>(task.requirements().estimated_threads);
    active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);

    // Launch task in separate thread
    try {
      auto future = std::async(
          std::launch::async,
          [task = std::move(task)]() mutable
          {
            try {
              task.execute();
            } catch (const std::exception& e) {
              std::cerr << "Task execution failed: " << e.what() << '\n';
            } catch (...) {
              std::cerr << "Task execution failed with unknown exception\n";
            }
          });

      active_tasks_.push_back(
          std::make_unique<ActiveTask>(std::move(future), task.requirements()));

      if (task.requirements().name) {
        std::cout << "Scheduled task: '" << *task.requirements().name
                  << "' (threads: " << threads_needed << ", priority: "
                  << static_cast<int>(task.requirements().priority) << ")\n";
      }
    } catch (...) {
      active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
      throw;
    }
  }
};

}  // namespace work_pool

// Example usage and testing
namespace example
{
using namespace work_pool;

static void cpu_intensive_task(const std::string& name, int duration_seconds)
{
  std::cout << "Starting CPU intensive task: " << name << "\n";

  auto start = std::chrono::steady_clock::now();
  auto end = start + std::chrono::seconds(duration_seconds);

  // Simulate CPU-intensive work
  volatile int64_t counter = 0;
  while (std::chrono::steady_clock::now() < end) {
    for (int i = 0; i < 1000000; ++i) {
      counter += static_cast<int64_t>(i) * i;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "Completed CPU intensive task: " << name << "\n";
}

static void multi_threaded_task(const std::string& name,
                                int num_threads,
                                int duration_seconds)
{
  std::cout << "Starting multi-threaded task: " << name << " with "
            << num_threads << " threads\n";

  std::vector<std::thread> threads;
  std::atomic<bool> stop_flag {false};

  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(
        [&stop_flag, i, name]()
        {
          volatile int64_t counter = 0;
          while (!stop_flag) {
            for (int j = 0; j < 100000; ++j) {
              counter += j;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  stop_flag = true;

  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }

    std::cout << "Completed multi-threaded task: " << name << "\n";
  }
}

static void run_example()
{
  const AdaptiveWorkPool::Config config {
      4,  // max_threads
      80.0,  // max_cpu_threshold
      50.0,  // min_cpu_threshold
      std::chrono::milliseconds(100)  // scheduler_interval
  };

  AdaptiveWorkPool pool(config);
  pool.start();

  // Submit various types of tasks
  std::vector<std::future<void>> futures;

  // High priority task
  futures.push_back(
      pool.submit(
          [](const std::string& name, int duration) {
            cpu_intensive_task(name, duration);
          },
          "Critical Task", 
          2,
          TaskRequirements {
                      1,  // estimated_threads
                      std::chrono::seconds(2),  // estimated_duration
                      Priority::Critical,  // priority
                      "Critical Processing"  // name
                  }));

  // Multi-threaded tasks
  for (int i = 0; i < 2; ++i) {
    std::string task_name = "MultiTask-" + std::to_string(i);
    futures.push_back(
        pool.submit(multi_threaded_task,
                    task_name,
                    2,
                    3,
                    TaskRequirements {
                        2,  // estimated_threads
                        std::chrono::seconds(3),  // estimated_duration
                        Priority::Medium,  // priority
                        "Multi-Task-" + std::to_string(i)  // name
                    }));
  }

  // Light tasks
  for (int i = 0; i < 3; ++i) {
    futures.push_back(pool.submit_lambda(
        [i]()
        {
          std::cout << "Light task " << i << " executing\n";
          std::this_thread::sleep_for(std::chrono::seconds(1));
          std::cout << "Light task " << i << " completed\n";
        },
        TaskRequirements {
            1,  // estimated_threads
            std::chrono::seconds(1),  // estimated_duration
            Priority::Low,  // priority
            "Light-Task-" + std::to_string(i)  // name
        }));
  }

  // Monitor progress
  auto monitor_future =
      std::async(std::launch::async,
                 [&pool]()
                 {
                   for (int i = 0; i < 15; ++i) {
                     std::this_thread::sleep_for(std::chrono::seconds(1));
                     pool.print_statistics();
                   }
                 });

  // Wait for all tasks to complete
  for (auto& future : futures) {
    future.wait();
  }

  monitor_future.wait();
  pool.print_statistics();

  std::cout << "All tasks completed!\n";
}


int main()
{
  try {
    example::run_example();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
}  // namespace example
