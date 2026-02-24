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
 * - Exception-safe design with proper cleanup
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtopt/cpu_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
struct WorkPoolConfig
{
  int max_threads;
  double max_cpu_threshold;
  std::chrono::milliseconds scheduler_interval;

  explicit constexpr WorkPoolConfig(
      int max_threads_ = static_cast<int>(std::thread::hardware_concurrency()),
      double max_cpu_threshold_ = 95.0,
      std::chrono::milliseconds scheduler_interval_ =
          std::chrono::milliseconds(20)) noexcept
      : max_threads(max_threads_)
      , max_cpu_threshold(max_cpu_threshold_)
      , scheduler_interval(scheduler_interval_)
  {
  }
};

enum class TaskStatus : uint8_t
{
  Success,
  Failed,
  Cancelled,
};

enum class TaskPriority : uint8_t
{
  Low = 0,
  Medium = 1,
  High = 2,
  Critical = 3,
};

struct TaskRequirements
{
  int estimated_threads = 1;
  std::chrono::milliseconds estimated_duration {1000};
  TaskPriority priority = TaskPriority::Medium;
  std::optional<std::string> name;
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

class AdaptiveWorkPool
{
  // Separate mutexes for different concerns
  mutable std::mutex queue_mutex_;  // Protects task queue
  mutable std::mutex active_mutex_;  // Protects active tasks
  std::condition_variable cv_;
  std::vector<Task<void>> task_queue_;

  std::vector<ActiveTask> active_tasks_;
  std::counting_semaphore<> available_threads_ {0};

  gtopt::CPUMonitor cpu_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};
  std::jthread scheduler_thread_;

  int max_threads_;
  double max_cpu_threshold_;
  std::chrono::milliseconds scheduler_interval_;

  std::atomic<size_t> tasks_completed_ {0};
  std::atomic<size_t> tasks_submitted_ {0};
  std::atomic<size_t> tasks_pending_ {0};
  std::atomic<size_t> tasks_active_ {0};

public:
  AdaptiveWorkPool(AdaptiveWorkPool&&) = delete;
  AdaptiveWorkPool(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&) = delete;
  AdaptiveWorkPool& operator=(const AdaptiveWorkPool&&) = delete;

  explicit constexpr AdaptiveWorkPool(WorkPoolConfig config = WorkPoolConfig {})
      : available_threads_(config.max_threads)
      , max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , scheduler_interval_(config.scheduler_interval)
  {
    spdlog::info(
        std::format("  WorkPool initialized with {} max threads, max CPU "
                    "threshold: {}%",
                    max_threads_,
                    max_cpu_threshold_));
  }

  ~AdaptiveWorkPool() { shutdown(); }

  void start();
  void shutdown();

  template<typename Func, typename... Args>
  [[nodiscard]] auto submit(Func&& func,
                            const TaskRequirements& req = TaskRequirements(),
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
        const std::scoped_lock<std::mutex> lock(queue_mutex_);
        try {
          // Both emplace_back and push_heap are under queue_mutex_, so
          // no other thread can observe the intermediate state.
          task_queue_.emplace_back([task]() { (*task)(); }, req);
          std::ranges::push_heap(task_queue_, std::less<> {});
          tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
          tasks_pending_.fetch_add(1, std::memory_order_relaxed);
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
      SPDLOG_ERROR("System error submitting task: {}", e.what());
      return std::unexpected(e.code());
    } catch (const std::exception& e) {
      SPDLOG_ERROR("Failed to submit task: {}", e.what());
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

  struct Statistics
  {
    size_t tasks_submitted;
    size_t tasks_completed;
    size_t tasks_pending;
    size_t tasks_active;
    int active_threads;
    double current_cpu_load;
  };

  Statistics get_statistics() const noexcept
  {
    return Statistics {
        .tasks_submitted = tasks_submitted_.load(),
        .tasks_completed = tasks_completed_.load(),
        .tasks_pending = tasks_pending_.load(),
        .tasks_active = tasks_active_.load(),
        .active_threads = active_threads_.load(),
        .current_cpu_load = cpu_monitor_.get_load(),
    };
  }

  [[nodiscard]] std::string format_statistics() const noexcept
  {
    try {
      const auto stats = get_statistics();
      return std::format(
          "=== WorkPool Statistics ===\n"
          "  Tasks: {:>6} submitted, {:>6} completed, {:>6} pending, {:>6} "
          "  active\n"
          "  Threads: {:>6} active / {:>6} max\n"
          "  CPU Load: {:>6.1f}%\n",
          stats.tasks_submitted,
          stats.tasks_completed,
          stats.tasks_pending,
          stats.tasks_active,
          stats.active_threads,
          max_threads_,
          stats.current_cpu_load);
    } catch (...) {
      return "WorkPool statistics unavailable";
    }
  }

  void log_statistics() const { spdlog::info(format_statistics()); }

private:
  void cleanup_completed_tasks();
  bool should_schedule_new_task() const;
  void schedule_next_task();
};

}  // namespace gtopt
