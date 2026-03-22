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
 * - Supports a generalized template priority key with configurable comparison
 * - Provides detailed statistics and monitoring
 * - Exception-safe design with proper cleanup
 *
 * ## Priority Key Semantics
 *
 * Tasks are ordered by a two-level key:
 *  1. `TaskPriority` enum (Critical > High > Medium > Low) – used as the
 *     primary tier and also controls the CPU load threshold for scheduling.
 *  2. A generic `Key` type with a configurable `KeyCompare` comparator –
 *     used as the secondary sort within the same `TaskPriority` tier.
 *
 * **Default semantics (KeyCompare = `std::less<Key>`)**: if `key1 < key2`
 * then `key1` has **higher** execution priority (is dequeued first).
 * To obtain the reverse ordering (larger key = higher priority), instantiate
 * the pool with `KeyCompare = std::greater<Key>`.
 *
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <functional>
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

#ifdef __linux__
#  include <pthread.h>
#endif

namespace gtopt
{
struct WorkPoolConfig
{
  int max_threads;
  double max_cpu_threshold;
  std::chrono::milliseconds scheduler_interval;
  std::string name;

  explicit WorkPoolConfig(
      int max_threads_ = static_cast<int>(std::thread::hardware_concurrency()),
      double max_cpu_threshold_ = 95.0,
      std::chrono::milliseconds scheduler_interval_ =
          std::chrono::milliseconds(20),
      std::string name_ = "WorkPool") noexcept
      : max_threads(max_threads_)
      , max_cpu_threshold(max_cpu_threshold_)
      , scheduler_interval(scheduler_interval_)
      , name(std::move(name_))
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

/// @brief Task requirements with a generic priority key.
///
/// @tparam Key  The type of the secondary sort key.  Must be default-
///              constructible and equality-comparable.  The default is
///              `int64_t` for backward compatibility.
///
/// When two tasks share the same `TaskPriority`, the pool dequeues them
/// according to `KeyCompare(key1, key2)` (see `BasicWorkPool`): by default
/// the task with the **smaller** key runs first.
template<typename Key = int64_t>
struct BasicTaskRequirements
{
  using key_type = Key;

  int estimated_threads = 1;
  std::chrono::milliseconds estimated_duration {1000};
  TaskPriority priority = TaskPriority::Medium;
  /// Secondary sort key.  With the default `std::less<Key>` comparator on
  /// the pool, the task with the **smaller** key is dequeued first within
  /// the same `TaskPriority` tier.
  Key priority_key = Key {};
  std::optional<std::string> name;
};

/// Backward-compatible alias: `TaskRequirements` is
/// `BasicTaskRequirements<int64_t>`.
using TaskRequirements = BasicTaskRequirements<>;

/// @brief Generic task wrapper with type-erased key type.
///
/// @tparam T         Result type of the task callable (default `void`).
/// @tparam Key       The priority-key type (must match the pool's key type).
/// @tparam KeyCompare  Comparator for the secondary sort.  The default
///                   `std::less<Key>` gives "smaller key = higher priority".
///                   Use `std::greater<Key>` for "larger key = higher
///                   priority" (the old pre-refactor behavior for int64_t).
template<typename T = void,
         typename Key = int64_t,
         typename KeyCompare = std::less<Key>>
class Task
{
public:
  using result_type = T;
  using key_type = Key;
  using key_compare = KeyCompare;

private:
  std::packaged_task<T()> task_;
  BasicTaskRequirements<Key> requirements_;
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
  explicit constexpr Task(F&& func, BasicTaskRequirements<Key> req = {})
      : task_(std::forward<F>(func))
      , requirements_(std::move(req))
      , submit_time_(std::chrono::steady_clock::now())
  {
  }

  std::future<T> get_future() { return task_.get_future(); }

  void execute() { task_(); }

  [[nodiscard]] constexpr const BasicTaskRequirements<Key>& requirements()
      const noexcept
  {
    return requirements_;
  }

  [[nodiscard]] constexpr auto age() const noexcept
  {
    return std::chrono::steady_clock::now() - submit_time_;
  }

  /// Returns true when `this` has **lower** priority than `other` (for use
  /// in a max-heap: the task at the top — the "greatest" — is dequeued
  /// first).
  ///
  /// Ordering:
  ///  1. `TaskPriority` tier: higher enum value = higher priority.
  ///  2. `Key` comparison via `KeyCompare`:
  ///     `KeyCompare(key1, key2) == true` ⟹ key1 has **higher** priority.
  ///     In a max-heap this means `operator<` returns true when `other`
  ///     has higher priority, i.e. `KeyCompare(other.key, this.key)`.
  ///     With the default `std::less<Key>`: smaller key → higher priority.
  ///  3. Tie-break: older submission → higher priority.
  bool operator<(const Task& other) const noexcept
  {
    if (requirements_.priority != other.requirements_.priority) {
      return requirements_.priority < other.requirements_.priority;
    }
    const KeyCompare cmp {};
    if (requirements_.priority_key != other.requirements_.priority_key) {
      // cmp(other.key, this.key): if true, other has higher priority,
      // so this is "lesser" in the heap → return true.
      return cmp(other.requirements_.priority_key, requirements_.priority_key);
    }
    return submit_time_ > other.submit_time_;
  }
};

struct ActiveTask
{
  std::future<void> future;
  int estimated_threads = 1;
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

/// @brief Adaptive thread pool with generic priority key.
///
/// @tparam Key        Secondary sort-key type for task ordering within the
///                    same `TaskPriority` tier.  Default is `int64_t`.
/// @tparam KeyCompare Comparator applied to keys.  Default `std::less<Key>`
///                    gives "smaller key → higher priority".  Pass
///                    `std::greater<Key>` to obtain "larger key → higher
///                    priority" (the original pre-refactor behavior).
///
/// All methods are defined inline here so that any specialization can be
/// instantiated without a separate translation unit.
template<typename Key = int64_t, typename KeyCompare = std::less<Key>>
class BasicWorkPool
{
public:
  using key_type = Key;
  using key_compare = KeyCompare;
  using Requirements = BasicTaskRequirements<Key>;

private:
  // Separate mutexes for different concerns
  mutable std::mutex queue_mutex_;  // Protects task queue
  mutable std::mutex active_mutex_;  // Protects active tasks
  std::condition_variable cv_;
  std::vector<Task<void, Key, KeyCompare>> task_queue_;

  std::vector<ActiveTask> active_tasks_;
  std::counting_semaphore<> available_threads_ {0};

  gtopt::CPUMonitor cpu_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};
  std::jthread scheduler_thread_;

  int max_threads_;
  double max_cpu_threshold_;
  std::chrono::milliseconds scheduler_interval_;
  std::string name_;

  std::atomic<size_t> tasks_completed_ {0};
  std::atomic<size_t> tasks_submitted_ {0};
  std::atomic<size_t> tasks_pending_ {0};
  std::atomic<size_t> tasks_active_ {0};

public:
  BasicWorkPool(BasicWorkPool&&) = delete;
  BasicWorkPool(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&&) = delete;

  explicit BasicWorkPool(WorkPoolConfig config = WorkPoolConfig {})
      : available_threads_(config.max_threads)
      , max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , scheduler_interval_(config.scheduler_interval)
      , name_(std::move(config.name))
  {
    spdlog::info("  {} initialized: {} max threads, {}% CPU threshold",
                 name_,
                 max_threads_,
                 max_cpu_threshold_);
  }

  ~BasicWorkPool() { shutdown(); }

  void start()
  {
    if (running_.exchange(true)) {
      return;
    }

    try {
      cpu_monitor_.set_interval(3 * scheduler_interval_);
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
          },
      };
      spdlog::info("  {} started", name_);
    } catch (const std::exception& e) {
      running_ = false;
      auto msg = std::format("Failed to start BasicWorkPool: {}", e.what());
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
      const std::scoped_lock lock(active_mutex_);
      for (auto& task : active_tasks_) {
        task.future.wait();
      }
      active_tasks_.clear();
      tasks_active_.store(0, std::memory_order_relaxed);
    }

    cpu_monitor_.stop();
    spdlog::info("  WorkPool shutdown complete");
  }

  template<typename Func, typename... Args>
  [[nodiscard]] auto submit(Func&& func,
                            const Requirements& req = Requirements(),
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
        } catch (const std::length_error&) {
          SPDLOG_ERROR("Task queue size limit exceeded");
          return std::unexpected(
              std::make_error_code(std::errc::resource_unavailable_try_again));
        } catch (const std::exception& e) {
          SPDLOG_ERROR("Failed to enqueue task: {}", e.what());
          return std::unexpected(
              std::make_error_code(std::errc::operation_not_permitted));
        }
      }

      cv_.notify_one();
      return future;
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
  auto submit_lambda(Func&& func, Requirements req = {})
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
  void cleanup_completed_tasks()
  {
    const std::scoped_lock<std::mutex> lock(active_mutex_);
    auto new_end = std::ranges::remove_if(active_tasks_,
                                          [this](const auto& task)
                                          {
                                            if (task.is_ready()) {
                                              active_threads_ -=
                                                  task.estimated_threads;
                                              tasks_completed_++;
                                              tasks_active_.fetch_sub(
                                                  1, std::memory_order_relaxed);
                                              return true;
                                            }
                                            return false;
                                          })
                       .begin();
    active_tasks_.erase(new_end, active_tasks_.end());
  }

  bool should_schedule_new_task() const
  {
    std::unique_lock queue_lock(queue_mutex_, std::defer_lock);
    std::unique_lock active_lock(active_mutex_, std::defer_lock);
    std::lock(queue_lock, active_lock);

    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.front();
    const auto cpu_load = cpu_monitor_.get_load();
    const auto threads_needed = next_task.requirements().estimated_threads;
    const auto current_threads = active_threads_.load();

    if (current_threads + threads_needed > max_threads_) {
      return false;
    }

    auto threshold = max_cpu_threshold_;
    switch (next_task.requirements().priority) {
      case TaskPriority::Critical:
        threshold = 95.0;
        break;
      case TaskPriority::High:
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

    // Standard heap-pop pattern: move max to back, restore heap on the rest,
    // then move-construct the task and pop_back to remove it.
    std::ranges::pop_heap(task_queue_, std::less<> {});
    Task<void, Key, KeyCompare> task = std::move(task_queue_.back());
    task_queue_.pop_back();

    tasks_pending_.fetch_sub(1, std::memory_order_relaxed);

    const auto threads_needed = task.requirements().estimated_threads;
    active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);

    try {
      auto future = std::async(
          std::launch::async,
          [ntask = std::move(task)]() mutable noexcept
          {
            try {
              ntask.execute();
            } catch (const std::exception& e) {
              SPDLOG_ERROR("Task execution failed: {}", e.what());
            } catch (...) {
              SPDLOG_ERROR("Task execution failed with unknown exception");
            }
          });

      active_tasks_.push_back(ActiveTask {
          .future = std::move(future),
          .estimated_threads = threads_needed,
          .start_time = std::chrono::steady_clock::now(),
      });
      tasks_active_.fetch_add(1, std::memory_order_relaxed);

    } catch (...) {
      active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
      throw;
    }
  }
};

/// @brief Default work pool using `int64_t` priority key with `std::less`
///        semantics (smaller key → higher priority).
///
/// This is a concrete derived class so that `class AdaptiveWorkPool;`
/// forward declarations in other headers remain valid.
class AdaptiveWorkPool final : public BasicWorkPool<>
{
public:
  using BasicWorkPool::BasicWorkPool;
};

}  // namespace gtopt
