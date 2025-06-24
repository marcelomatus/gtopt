/**
 * @file      work_pool.hpp
 * @brief     Adaptive thread pool with CPU monitoring and priority scheduling
 * @date      Mon Jun 23 23:48:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements an adaptive work pool that:
 * - Dynamically adjusts task scheduling based on CPU load
 * - Supports task priorities (Low, Medium, High, Critical)
 * - Provides detailed statistics and monitoring
 * - Uses modern C++23 features including:
 *   - std::jthread for thread management
 *   - std::counting_semaphore for resource control
 *   - std::format for logging
 * - Exception-safe design with proper cleanup
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
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <semaphore>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace gtopt
{
enum class TaskStatus : uint8_t
{
  Success,
  Failed,
  Cancelled
};

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
  CPUMonitor(CPUMonitor&&) = delete;
  CPUMonitor& operator=(CPUMonitor&&) = delete;

  ~CPUMonitor() { stop(); }

  void start()
  {
    running_.store(true, std::memory_order_relaxed);
    monitor_thread_ = std::jthread {
        [this](const std::stop_token& stoken)
        {
          while (!stoken.stop_requested()
                 && running_.load(std::memory_order_relaxed))
          {
            current_load_.store(get_system_cpu_usage(),
                                std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

  [[nodiscard]] constexpr double get_load() const noexcept
  {
    return current_load_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<double> current_load_ {0.0};
  std::atomic<bool> running_ {false};
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
  explicit constexpr Task(F&& func, TaskRequirements req = {})
      : task_(std::forward<F>(func))
      , requirements_(std::move(req))
      , submit_time_(std::chrono::steady_clock::now())
  {
  }

  std::future<T> get_future() { return task_.get_future(); }

  void execute() { task_(); }

  [[nodiscard]] constexpr const TaskRequirements& requirements() const noexcept
  {
    return requirements_;
  }

  [[nodiscard]] constexpr auto age() const noexcept
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
  // Separate mutexes for different concerns
  mutable std::mutex queue_mutex_;  // Protects task queue
  mutable std::mutex active_mutex_;  // Protects active tasks
  std::condition_variable cv_;
  std::priority_queue<Task<void>> task_queue_;
  struct ActiveTask
  {
    std::future<void> future;
    TaskRequirements requirements;
    std::chrono::steady_clock::time_point start_time;

    [[nodiscard]] bool is_ready() const noexcept
    {
      return future.wait_for(std::chrono::seconds(0))
          == std::future_status::ready;
    }

    [[nodiscard]] constexpr auto runtime() const noexcept
    {
      return std::chrono::steady_clock::now() - start_time;
    }
  };

  std::vector<ActiveTask> active_tasks_;
  std::counting_semaphore<> available_threads_ {0};

  CPUMonitor cpu_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};
  std::jthread scheduler_thread_;

  int max_threads_;
  double max_cpu_threshold_;
  std::chrono::milliseconds scheduler_interval_;

  std::atomic<size_t> tasks_completed_ {0};
  std::atomic<size_t> tasks_submitted_ {0};

public:
  struct Statistics
  {
    size_t tasks_submitted;
    size_t tasks_completed;
    size_t tasks_pending;
    size_t tasks_active;
    int active_threads;
    double current_cpu_load;
  };

  struct Config
  {
    int max_threads;
    double max_cpu_threshold;
    std::chrono::milliseconds scheduler_interval;

    explicit constexpr Config(int max_threads_ = static_cast<int>(
                                  std::thread::hardware_concurrency()),
                              double max_cpu_threshold_ = 95.0,
                              std::chrono::milliseconds scheduler_interval_ =
                                  std::chrono::milliseconds(20)) noexcept
        : max_threads(max_threads_)
        , max_cpu_threshold(max_cpu_threshold_)
        , scheduler_interval(scheduler_interval_)
    {
    }
  };

  AdaptiveWorkPool(AdaptiveWorkPool&&) = delete;
  AdaptiveWorkPool(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&&) = delete;

  explicit constexpr AdaptiveWorkPool(Config config = Config {})
      : available_threads_(config.max_threads)
      , max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , scheduler_interval_(config.scheduler_interval)
  {
    SPDLOG_INFO(
        std::format("AdaptiveWorkPool initialized with {} max threads, max CPU "
                    "threshold: {}%",
                    max_threads_,
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
            pthread_setname_np(pthread_self(), "WorkPoolScheduler");
            while (!stoken.stop_requested() && running_) {
              this->cleanup_completed_tasks();
              if (this->should_schedule_new_task()) {
                this->schedule_next_task();
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
      const std::lock_guard<std::mutex> lock(active_mutex_);
      for (auto& task : active_tasks_) {
        task.future.wait();
      }
      active_tasks_.clear();
    }

    cpu_monitor_.stop();
    SPDLOG_INFO("AdaptiveWorkPool shutdown complete");
  }

  template<typename Func, typename... Args>
  [[nodiscard]] auto submit(Func&& func,
                            const TaskRequirements& req = {},
                            Args&&... args)
      -> std::expected<std::future<std::invoke_result_t<Func, Args...>>,
                       std::error_code>
  {
    if constexpr (std::is_same_v<std::decay_t<Func>, std::function<void()>>) {
      if (!func) {
        SPDLOG_WARN("Attempted to submit null std::function");
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
      }
    }

    using ReturnType = std::invoke_result_t<Func, Args...>;

    try {
      auto task = std::make_shared<std::packaged_task<ReturnType()>>(
          [func = std::forward<Func>(func),
           ... args = std::forward<Args>(args)]() mutable
          { return std::invoke(func, args...); });

      auto future = task->get_future();

      {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        try {
          task_queue_.emplace([task]() { (*task)(); }, req);
          tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::bad_alloc&) {
          SPDLOG_ERROR("Failed to allocate memory for task queue");
          return std::unexpected(
              std::make_error_code(std::errc::not_enough_memory));
        } catch (const std::length_error&) {
          SPDLOG_ERROR("Task queue size limit exceeded");
          return std::unexpected(
              std::make_error_code(std::errc::resource_unavailable_try_again));
        }
      }

      cv_.notify_one();
      return future;
    } catch (const std::bad_alloc&) {
      SPDLOG_ERROR("Failed to allocate memory for task");
      return std::unexpected(
          std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& e) {
      SPDLOG_ERROR(std::format("System error submitting task: {}", e.what()));
      return std::unexpected(e.code());
    } catch (const std::exception& e) {
      SPDLOG_ERROR(std::format("Failed to submit task: {}", e.what()));
      return std::unexpected(
          std::make_error_code(std::errc::operation_not_permitted));
    } catch (...) {
      SPDLOG_ERROR("Unknown exception while submitting task");
      return std::unexpected(
          std::make_error_code(std::errc::operation_not_permitted));
    }
  }

  template<typename Func>
  auto submit_lambda(Func&& func, TaskRequirements req = {})
  {
    return submit(std::forward<Func>(func), std::move(req));
  }

  Statistics get_statistics() const noexcept
  {
    std::unique_lock queue_lock(queue_mutex_, std::defer_lock);
    std::unique_lock active_lock(active_mutex_, std::defer_lock);
    std::lock(queue_lock, active_lock);
    return Statistics {.tasks_submitted = tasks_submitted_.load(),
                       .tasks_completed = tasks_completed_.load(),
                       .tasks_pending = task_queue_.size(),
                       .tasks_active = active_tasks_.size(),
                       .active_threads = active_threads_.load(),
                       .current_cpu_load = cpu_monitor_.get_load()};
  }

  [[nodiscard]] std::string format_statistics() const noexcept
  {
    const auto stats = get_statistics();
    return std::format(
        "=== WorkPool Statistics ===\n"
        "Tasks: {:>6} submitted, {:>6} completed, {:>6} pending, {:>6} active\n"
        "Threads: {:>6} active / {:>6} max\n"
        "CPU Load: {:>6.1f}%\n",
        stats.tasks_submitted,
        stats.tasks_completed,
        stats.tasks_pending,
        stats.tasks_active,
        stats.active_threads,
        max_threads_,
        stats.current_cpu_load);
  }

  void info_statistics() const
  {
    SPDLOG_INFO(format_statistics());
  }

  void cleanup_completed_tasks()
  {
    const std::lock_guard<std::mutex> lock(active_mutex_);
    auto new_end =
        std::ranges::remove_if(active_tasks_,
                               [this](const auto& task)
                               {
                                 if (task.is_ready()) {
                                   active_threads_ -=
                                       task.requirements.estimated_threads;
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
    std::unique_lock queue_lock(queue_mutex_, std::defer_lock);
    std::unique_lock active_lock(active_mutex_, std::defer_lock);
    std::lock(queue_lock, active_lock);  // Lock both without deadlock

    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.top();
    const auto cpu_load = cpu_monitor_.get_load();
    const auto threads_needed = (next_task.requirements().estimated_threads);
    const auto current_threads = (active_threads_.load());

    if (current_threads + threads_needed > (max_threads_)) {
      return false;
    }

    auto threshold = max_cpu_threshold_;
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
    const std::unique_lock queue_lock(queue_mutex_);

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
          [task = std::move(task),
           req = std::move(task).requirements()]() mutable
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
      active_tasks_.push_back(
          ActiveTask {.future = std::move(future),
                      .requirements = req,
                      .start_time = std::chrono::steady_clock::now()});

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
