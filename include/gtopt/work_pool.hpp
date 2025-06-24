/**
 * @file      work_pool.hpp
 * @brief     Header of
 * @date      Mon Jun 23 23:48:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <generator>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace gtopt
{
enum class [[nodiscard]] TaskStatus : uint8_t
{
  Success,
  Failed,
  Cancelled
};

[[nodiscard]]
constexpr std::string_view to_string(TaskStatus status) noexcept
{
  using enum TaskStatus;
  switch (status) {
    case Success:
      return "Success";
    case Failed:
      return "Failed";
    case Cancelled:
      return "Cancelled";
    default:
      return "Unknown";
  }
}

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

class CPUMonitor
{
public:
  CPUMonitor() = default;
  CPUMonitor(const CPUMonitor&) = delete;
  CPUMonitor& operator=(const CPUMonitor&) = delete;
  CPUMonitor(CPUMonitor&&) = default;
  CPUMonitor& operator=(CPUMonitor&&) = default;
  
  ~CPUMonitor() { stop(); }

  void start()
  {
    running_.store(true, std::memory_order_relaxed);
    monitor_thread_ = std::jthread{[this](const std::stop_token& stoken)
    {
      while (!stoken.stop_requested() && running_.load(std::memory_order_relaxed)) 
      {
        current_load_.store(get_system_cpu_usage(), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }};
  }

  void stop()
  {
    running_.store(false, std::memory_order_relaxed);
    if (monitor_thread_.joinable()) {
      monitor_thread_.request_stop();
      monitor_thread_.join();
    }
  }

  [[nodiscard]] double get_load() const noexcept
  {
    return current_load_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<double> current_load_{0.0};
  std::atomic<bool> running_{false};
  std::jthread monitor_thread_;

  static double get_system_cpu_usage()
  {
    static uint64_t last_idle = 0;
    static uint64_t last_total = 0;

    std::ifstream proc_stat("/proc/stat");
    if (!proc_stat) {
      return 50.0;  // fallback
    }

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
      auto total = std::accumulate(times.begin(), times.end(), 0ULL);

      auto idle_delta = idle - last_idle;
      auto total_delta = total - last_total;

      last_idle = idle;
      last_total = total;

      if (total_delta > 0) {
        return 100.0
            * (1.0
               - static_cast<double>(idle_delta)
                   / static_cast<double>(total_delta));
      }
    }
    return 0.0;
  }

};

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
  Task() = default;
  Task(Task&&) = default;
  Task& operator=(Task&&) = default;
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() = default;

  template<typename F>
    requires(!std::same_as<std::remove_cvref_t<F>, Task>)
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

  bool operator<(const Task& other) const noexcept
  {
    if (requirements_.priority != other.requirements_.priority) {
      return requirements_.priority < other.requirements_.priority;
    }
    return submit_time_ > other.submit_time_;
  }
};


class AdaptiveWorkPool
{
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::priority_queue<Task<void>> task_queue_;
  struct ActiveTask {
    std::future<void> future;
    TaskRequirements requirements;
    std::chrono::steady_clock::time_point start_time;

    [[nodiscard]] bool is_ready() const {
      return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    [[nodiscard]] auto runtime() const noexcept {
      return std::chrono::steady_clock::now() - start_time;
    }
  };

  std::vector<ActiveTask> active_tasks_;
  std::counting_semaphore<> available_threads_{0};

  CPUMonitor cpu_monitor_;
  std::atomic<int> active_threads_{0};
  std::atomic<bool> running_{false};
  std::jthread scheduler_thread_;

  int max_threads_;
  double max_cpu_threshold_;
  double min_cpu_threshold_;
  std::chrono::milliseconds scheduler_interval_;

  std::atomic<size_t> tasks_completed_{0};
  std::atomic<size_t> tasks_submitted_{0};

public:
  struct Config
  {
    int max_threads;
    double max_cpu_threshold;
    double min_cpu_threshold;
    std::chrono::milliseconds scheduler_interval;

    explicit Config(int max_threads_ = 1000,
                    double max_cpu_threshold_ = 85.0,
                    double min_cpu_threshold_ = 60.0,
                    std::chrono::milliseconds scheduler_interval_ =
                        std::chrono::milliseconds(50))
        : max_threads(max_threads_)
        , max_cpu_threshold(max_cpu_threshold_)
        , min_cpu_threshold(min_cpu_threshold_)
        , scheduler_interval(scheduler_interval_)
    {
    }
  };

  AdaptiveWorkPool(const AdaptiveWorkPool&&) = delete;
  AdaptiveWorkPool(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&&) = delete;

  explicit AdaptiveWorkPool(Config config = Config {})
      : available_threads_(config.max_threads)
      , max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , min_cpu_threshold_(config.min_cpu_threshold)
      , scheduler_interval_(config.scheduler_interval)
  {
    SPDLOG_INFO(std::format(
        "AdaptiveWorkPool initialized with {} max threads, CPU thresholds: "
        "{}%-{}%",
        max_threads_,
        min_cpu_threshold_,
        max_cpu_threshold_));
  }

  ~AdaptiveWorkPool() { shutdown(); }

  void start()
  {
    if (running_.exchange(true)) {
      return;
    }

    try {
      cpu_monitor_.start();
      scheduler_thread_ = std::jthread {
          [this](const std::stop_token& stoken)
          {
#ifdef __linux__
            pthread_setname_np(pthread_self(), "WorkPoolScheduler");
#endif
            while (!stoken.stop_requested() && running_) {
              cleanup_completed_tasks();
              if (should_schedule_new_task()) {
                schedule_next_task();
              }
              std::this_thread::sleep_for(scheduler_interval_);
            }
          }};
      SPDLOG_INFO(std::format("AdaptiveWorkPool started with {} max threads",
                              max_threads_));
    } catch (const std::exception& e) {
      running_ = false;

      auto msg = std::format("Failed to start AdaptiveWorkPool: {}", e.what());
      SPDLOG_ERROR(msg);
      throw std::runtime_error(msg);
    }
  }

  void shutdown()
  {
    if (!running_) {
      return;
    }

    running_ = false;
    cv_.notify_all();

    if (scheduler_thread_.joinable()) {
      scheduler_thread_.request_stop();
      scheduler_thread_.join();
    }

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (auto& task : active_tasks_) {
        task.wait();
      }
      active_tasks_.clear();
    }

    cpu_monitor_.stop();
    SPDLOG_INFO("AdaptiveWorkPool shutdown complete");
  }

  template<typename F, typename... Args>
  auto submit(F&& func, TaskRequirements req = {}, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>>
  {
    using ReturnType = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(func),
         ... args = std::forward<Args>(args)]() mutable  // NOLINT
        { return std::invoke(func, args...); });

    auto future = task->get_future();

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      task_queue_.emplace([task]() { (*task)(); }, std::move(req));
      tasks_submitted_++;
    }

    cv_.notify_one();
    return future;
  }

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
    const std::lock_guard<std::mutex> lock(mutex_);
    return Statistics {.tasks_submitted = tasks_submitted_.load(),
                       .tasks_completed = tasks_completed_.load(),
                       .tasks_pending = task_queue_.size(),
                       .tasks_active = active_tasks_.size(),
                       .active_threads = active_threads_.load(),
                       .current_cpu_load = cpu_monitor_.get_load()};
  }

  void print_statistics() const
  {
    auto stats = get_statistics();
    SPDLOG_INFO("=== WorkPool Statistics ===");
    SPDLOG_INFO(
        std::format("Tasks: {} submitted, {} completed, {} pending, {} active",
                    stats.tasks_submitted,
                    stats.tasks_completed,
                    stats.tasks_pending,
                    stats.tasks_active));
    SPDLOG_INFO(std::format(
        "Threads: {} active / {} max", stats.active_threads, max_threads_));
    SPDLOG_INFO(std::format("CPU Load: {}%", stats.current_cpu_load));
  }

private:
  void cleanup_completed_tasks()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto new_end =
        std::ranges::remove_if(active_tasks_,
                               [this](const auto& task)
                               {
                                 if (task.is_ready()) {
                                   active_threads_ -=
                                       task->requirements().estimated_threads;
                                   tasks_completed_++;
                                   return true;
                                 }
                                 return false;
                               })
            .begin();
    active_tasks_.erase(new_end, active_tasks_.end());
  }

  [[nodiscard]] bool should_schedule_new_task() const
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.top();
    const double cpu_load = cpu_monitor_.get_load();
    const auto threads_needed =
        static_cast<unsigned int>(next_task.requirements().estimated_threads);
    const auto current_threads =
        static_cast<unsigned int>(active_threads_.load());

    if (current_threads + threads_needed
        > static_cast<unsigned int>(max_threads_))
    {
      return false;
    }

    double threshold = max_cpu_threshold_;
    switch (next_task.requirements().priority) {
      case Priority::Critical:
        threshold = 95.0;
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
    const std::unique_lock<std::mutex> lock(mutex_);

    if (task_queue_.empty()) {
      return;
    }

    // move out of priority_queue safely for move-only type
    Task<void> task =
        std::move(const_cast<Task<void>&>(task_queue_.top()));  // NOLINT
    task_queue_.pop();

    const auto threads_needed =
        static_cast<int>(task.requirements().estimated_threads);
    active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);

    try {
      auto future = std::async(
          std::launch::async,
          [task = std::move(task), req = task.requirements()]() mutable
          {
            try {
              task.execute();
            } catch (const std::exception& e) {
              SPDLOG_ERROR(std::format("Task execution failed: {}", e.what()));
            } catch (...) {
              SPDLOG_ERROR("Task execution failed with unknown exception");
            }
          });

      const auto req = task.requirements();
      active_tasks_.push_back(ActiveTask{
          std::move(future),
          req,
          std::chrono::steady_clock::now()});

      if (task.requirements().name) {
        SPDLOG_INFO(
            std::format("Scheduled task: '{}' (threads: {}, priority: {})",
                        *task.requirements().name,
                        threads_needed,
                        static_cast<int>(task.requirements().priority)));
      }
    } catch (...) {
      active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
      throw;
    }
  }
};

}  // namespace gtopt
