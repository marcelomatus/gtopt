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
#include <gtopt/memory_monitor.hpp>
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
  double min_free_memory_mb;  ///< Block dispatch if system free < this (MB)
  double max_memory_percent;  ///< Block dispatch if system usage > this (%)
  double max_process_rss_mb;  ///< Block dispatch if process RSS > this (0=off)
  std::chrono::milliseconds scheduler_interval;
  std::string name;
  bool enable_periodic_stats {true};  ///< Log periodic CPU/MEM stats

  explicit WorkPoolConfig(
      int max_threads_ = static_cast<int>(std::thread::hardware_concurrency()),
      double max_cpu_threshold_ = 95.0,
      double min_free_memory_mb_ = 2048.0,
      double max_memory_percent_ = 95.0,
      double max_process_rss_mb_ = 0.0,
      std::chrono::milliseconds scheduler_interval_ =
          std::chrono::milliseconds(50),
      std::string name_ = "WorkPool",
      bool enable_periodic_stats_ = true) noexcept
      : max_threads(max_threads_)
      , max_cpu_threshold(max_cpu_threshold_)
      , min_free_memory_mb(min_free_memory_mb_)
      , max_memory_percent(max_memory_percent_)
      , max_process_rss_mb(max_process_rss_mb_)
      , scheduler_interval(scheduler_interval_)
      , name(std::move(name_))
      , enable_periodic_stats(enable_periodic_stats_)
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

/// Per-task resource usage sampled before/after execution.
struct TaskResourceStats
{
  double cpu_load_before {};  ///< System CPU % at task start
  double cpu_load_after {};  ///< System CPU % at task end
  double rss_mb_before {};  ///< Process RSS (MB) at task start
  double rss_mb_after {};  ///< Process RSS (MB) at task end
  double duration_s {};  ///< Wall-clock seconds
};

struct ActiveTask
{
  std::future<void> future;
  int estimated_threads = 1;
  std::chrono::steady_clock::time_point start_time;
  std::shared_ptr<TaskResourceStats> resource_stats {};

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
  gtopt::MemoryMonitor memory_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};
  std::jthread scheduler_thread_;

  int max_threads_;
  double max_cpu_threshold_;
  double min_free_memory_mb_;
  double max_memory_percent_;
  double max_process_rss_mb_;
  std::chrono::milliseconds scheduler_interval_;
  std::string name_;
  bool enable_periodic_stats_;

  std::atomic<size_t> tasks_completed_ {0};
  std::atomic<size_t> tasks_submitted_ {0};
  std::atomic<size_t> tasks_pending_ {0};
  std::atomic<size_t> tasks_active_ {0};

  // Per-task resource accumulation (protected by active_mutex_)
  size_t lp_tasks_dispatched_ {0};
  double total_task_cpu_pct_ {0.0};
  double total_task_rss_delta_mb_ {0.0};
  std::chrono::steady_clock::time_point pool_start_time_ {};

public:
  BasicWorkPool(BasicWorkPool&&) = delete;
  BasicWorkPool(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&&) = delete;

  explicit BasicWorkPool(WorkPoolConfig config = WorkPoolConfig {})
      : available_threads_(config.max_threads)
      , max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , min_free_memory_mb_(config.min_free_memory_mb)
      , max_memory_percent_(config.max_memory_percent)
      , max_process_rss_mb_(config.max_process_rss_mb)
      , scheduler_interval_(config.scheduler_interval)
      , name_(std::move(config.name))
      , enable_periodic_stats_(config.enable_periodic_stats)
  {
    spdlog::info(
        "  {} initialized: {} max threads, {:.0f}% CPU threshold, "
        "{:.0f} MB min free mem, {:.0f}% max mem{}",
        name_,
        max_threads_,
        max_cpu_threshold_,
        min_free_memory_mb_,
        max_memory_percent_,
        max_process_rss_mb_ > 0
            ? std::format(", {:.0f} MB max RSS", max_process_rss_mb_)
            : "");
  }

  ~BasicWorkPool() noexcept
  {
    // Destructor must not throw.  `shutdown()` calls spdlog / std::format
    // which can in principle throw `std::format_error`; swallow any such
    // exception rather than terminating the program during teardown.
    try {
      shutdown();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // best-effort cleanup; deliberately swallowed
    }
  }

  void start()
  {
    if (running_.exchange(true)) {
      return;
    }

    pool_start_time_ = std::chrono::steady_clock::now();

    try {
      cpu_monitor_.set_interval(3 * scheduler_interval_);
      cpu_monitor_.start();
      memory_monitor_.set_interval(3 * scheduler_interval_);
      memory_monitor_.start();
      scheduler_thread_ = std::jthread {
          [this](const std::stop_token& stoken)
          {
#ifdef __linux__
            pthread_setname_np(pthread_self(), "WorkPoolScheduler");
#endif
            auto last_log = std::chrono::steady_clock::now();
            constexpr auto log_interval = std::chrono::seconds(30);

            while (!stoken.stop_requested() && running_) {
              cleanup_completed_tasks();

              // Dispatch as many ready tasks as possible in one pass
              while (should_schedule_new_task()) {
                schedule_next_task();
              }

              // Periodic stats logging
              const auto now = std::chrono::steady_clock::now();
              if (enable_periodic_stats_ && now - last_log >= log_interval) {
                log_periodic_stats();
                last_log = now;
              }

              // Wait on cv_ — wakes immediately on submit(), task
              // completion, or shutdown instead of sleeping the full
              // interval.
              std::unique_lock lock(queue_mutex_);
              cv_.wait_for(lock,
                           scheduler_interval_,
                           [&]
                           {
                             return stoken.stop_requested() || !running_.load()
                                 || !task_queue_.empty();
                           });
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
    memory_monitor_.stop();

    // Log final summary
    log_final_stats();
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

      // Fast path: if the queue is empty and we have thread capacity,
      // launch directly without enqueuing — avoids scheduler round-trip.
      if (try_launch_direct(task, req)) {
        tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        return future;
      }

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

  /// Submit multiple callables under a single lock acquisition.
  /// Returns a vector of futures, one per callable.
  template<typename Func>
  [[nodiscard]] auto submit_batch(
      std::vector<std::pair<Func, Requirements>>& tasks)
      -> std::vector<std::expected<std::future<std::invoke_result_t<Func>>,
                                   std::error_code>>
  {
    using ReturnType = std::invoke_result_t<Func>;
    using ResultVec =
        std::vector<std::expected<std::future<ReturnType>, std::error_code>>;

    ResultVec results;
    results.reserve(tasks.size());

    {
      const std::scoped_lock<std::mutex> lock(queue_mutex_);
      for (auto& [func, req] : tasks) {
        try {
          auto ptask = std::make_shared<std::packaged_task<ReturnType()>>(
              std::move(func));
          results.push_back(ptask->get_future());
          task_queue_.emplace_back([ptask]() { (*ptask)(); }, req);
          std::ranges::push_heap(task_queue_, std::less<> {});
          tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
          tasks_pending_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
          SPDLOG_ERROR("Failed to enqueue batch task: {}", e.what());
          results.push_back(std::unexpected(
              std::make_error_code(std::errc::operation_not_permitted)));
        }
      }
    }

    cv_.notify_one();
    return results;
  }

  /// Thread cap configured for this pool.  Needed by tests that
  /// verify `make_solver_work_pool(cpu_factor)` clamps its thread
  /// count correctly (e.g. tiny `cpu_factor` must floor to 1 thread
  /// to give a genuine serial baseline).  Safe to call from any
  /// thread — `max_threads_` is set once in the constructor.
  [[nodiscard]] int max_threads() const noexcept { return max_threads_; }

  struct Statistics
  {
    size_t tasks_submitted;
    size_t tasks_completed;
    size_t tasks_pending;
    size_t tasks_active;
    int active_threads;
    double current_cpu_load;
    double current_memory_percent;  ///< System memory usage %
    double available_memory_mb;  ///< System available memory MB
    double process_rss_mb;  ///< Process RSS in MB
    size_t lp_tasks_dispatched;  ///< Total LP tasks dispatched
    double avg_task_cpu_pct;  ///< Average CPU % per LP task
    double avg_task_rss_delta_mb;  ///< Average RSS delta per LP task
  };

  Statistics get_statistics() const noexcept
  {
    double avg_cpu = 0.0;
    double avg_mem = 0.0;
    size_t dispatched = 0;
    {
      const std::scoped_lock lock(active_mutex_);
      dispatched = lp_tasks_dispatched_;
      if (dispatched > 0) {
        avg_cpu = total_task_cpu_pct_ / static_cast<double>(dispatched);
        avg_mem = total_task_rss_delta_mb_ / static_cast<double>(dispatched);
      }
    }
    return Statistics {
        .tasks_submitted = tasks_submitted_.load(),
        .tasks_completed = tasks_completed_.load(),
        .tasks_pending = tasks_pending_.load(),
        .tasks_active = tasks_active_.load(),
        .active_threads = active_threads_.load(),
        .current_cpu_load = cpu_monitor_.get_load(),
        .current_memory_percent = memory_monitor_.get_memory_percent(),
        .available_memory_mb = memory_monitor_.get_available_mb(),
        .process_rss_mb = memory_monitor_.get_process_rss_mb(),
        .lp_tasks_dispatched = dispatched,
        .avg_task_cpu_pct = avg_cpu,
        .avg_task_rss_delta_mb = avg_mem,
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
          "  CPU Load: {:>6.1f}%\n"
          "  Memory: {:.1f}% used, {:.0f} MB free, RSS {:.0f} MB\n"
          "  LP tasks: {} dispatched, avg CPU {:.1f}%, avg mem delta "
          "{:.1f} MB\n",
          stats.tasks_submitted,
          stats.tasks_completed,
          stats.tasks_pending,
          stats.tasks_active,
          stats.active_threads,
          max_threads_,
          stats.current_cpu_load,
          stats.current_memory_percent,
          stats.available_memory_mb,
          stats.process_rss_mb,
          stats.lp_tasks_dispatched,
          stats.avg_task_cpu_pct,
          stats.avg_task_rss_delta_mb);
    } catch (...) {
      return "WorkPool statistics unavailable";
    }
  }

  void log_statistics() const { spdlog::info(format_statistics()); }

private:
  /// Fast-path: launch a task directly from submit() when the queue is
  /// empty and thread capacity is available.  Returns true on success.
  template<typename ReturnType>
  bool try_launch_direct(
      const std::shared_ptr<std::packaged_task<ReturnType()>>& task,
      const Requirements& req)
  {
    const auto threads_needed = req.estimated_threads;
    const auto current_threads =
        active_threads_.load(std::memory_order_relaxed);

    // Only use fast path when queue is empty and threads available
    if (current_threads + threads_needed > max_threads_) {
      return false;
    }

    // Try to claim the queue lock; if contended, fall back to enqueue
    std::unique_lock queue_lock(queue_mutex_, std::try_to_lock);
    if (!queue_lock.owns_lock() || !task_queue_.empty()) {
      return false;
    }
    queue_lock.unlock();

    active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);

    auto stats = std::make_shared<TaskResourceStats>();
    stats->cpu_load_before = cpu_monitor_.get_load();
    stats->rss_mb_before = memory_monitor_.get_process_rss_mb();

    try {
      auto future = std::async(
          std::launch::async,
          [task, stats, this]() mutable noexcept
          {
            try {
              (*task)();
            } catch (const std::exception& e) {
              SPDLOG_ERROR("Task execution failed: {}", e.what());
            } catch (...) {
              SPDLOG_ERROR("Task execution failed with unknown exception");
            }
            stats->cpu_load_after = cpu_monitor_.get_load();
            stats->rss_mb_after = memory_monitor_.get_process_rss_mb();
            cv_.notify_one();
          });

      {
        const std::scoped_lock active_lock(active_mutex_);
        active_tasks_.push_back(ActiveTask {
            .future = std::move(future),
            .estimated_threads = threads_needed,
            .start_time = std::chrono::steady_clock::now(),
            .resource_stats = std::move(stats),
        });
      }
      tasks_active_.fetch_add(1, std::memory_order_relaxed);
      return true;

    } catch (...) {
      active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
      return false;
    }
  }

  void cleanup_completed_tasks()
  {
    const std::scoped_lock<std::mutex> lock(active_mutex_);
    auto new_end =
        std::ranges::remove_if(
            active_tasks_,
            [this](const auto& task)
            {
              if (task.is_ready()) {
                active_threads_ -= task.estimated_threads;
                tasks_completed_++;
                tasks_active_.fetch_sub(1, std::memory_order_relaxed);

                // Accumulate per-task resource stats
                if (task.resource_stats) {
                  const auto& rs = *task.resource_stats;
                  ++lp_tasks_dispatched_;
                  total_task_cpu_pct_ +=
                      (rs.cpu_load_before + rs.cpu_load_after) / 2.0;
                  const auto delta = rs.rss_mb_after - rs.rss_mb_before;
                  total_task_rss_delta_mb_ += delta;
                }
                return true;
              }
              return false;
            })
            .begin();
    active_tasks_.erase(new_end, active_tasks_.end());
  }

  bool should_schedule_new_task() const
  {
    // Lock-free fast-reject: check atomics before acquiring any mutex.
    if (tasks_pending_.load(std::memory_order_relaxed) == 0) {
      return false;
    }
    if (active_threads_.load(std::memory_order_relaxed) >= max_threads_) {
      return false;
    }

    // Need the queue lock to inspect the top task's requirements
    const std::unique_lock queue_lock(queue_mutex_);

    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.front();
    const auto threads_needed = next_task.requirements().estimated_threads;
    const auto current_threads = active_threads_.load();

    if (current_threads + threads_needed > max_threads_) {
      return false;
    }

    const auto is_critical =
        next_task.requirements().priority == TaskPriority::Critical;

    // CPU check — only apply when threads are near saturation.
    // Thread count is the primary concurrency limiter; CPU load is a
    // secondary guard that only matters when cores are already busy.
    if (current_threads + threads_needed
        >= static_cast<int>(max_threads_ * 0.8))
    {
      const auto cpu_load = cpu_monitor_.get_load();
      auto cpu_threshold = max_cpu_threshold_;
      switch (next_task.requirements().priority) {
        case TaskPriority::Critical:
          cpu_threshold = 95.0;
          break;
        case TaskPriority::High:
          cpu_threshold = max_cpu_threshold_ + 5.0;
          break;
        default:
          break;
      }
      if (cpu_load >= cpu_threshold) {
        return false;
      }
    }

    // Memory checks (Critical tasks get relaxed thresholds)
    const auto mem_pct = memory_monitor_.get_memory_percent();
    const auto mem_threshold = is_critical ? 98.0 : max_memory_percent_;
    if (mem_pct >= mem_threshold) {
      SPDLOG_DEBUG("{}: blocked by memory usage {:.1f}% >= {:.1f}%",
                   name_,
                   mem_pct,
                   mem_threshold);
      return false;
    }

    const auto free_mb = memory_monitor_.get_available_mb();
    const auto free_threshold =
        is_critical ? min_free_memory_mb_ * 0.5 : min_free_memory_mb_;
    if (free_mb < free_threshold && free_mb > 0.0) {
      SPDLOG_DEBUG("{}: blocked by low free memory {:.0f} MB < {:.0f} MB",
                   name_,
                   free_mb,
                   free_threshold);
      return false;
    }

    if (max_process_rss_mb_ > 0.0) {
      const auto rss = memory_monitor_.get_process_rss_mb();
      const auto rss_threshold =
          is_critical ? max_process_rss_mb_ * 1.1 : max_process_rss_mb_;
      if (rss >= rss_threshold) {
        SPDLOG_DEBUG("{}: blocked by process RSS {:.0f} MB >= {:.0f} MB",
                     name_,
                     rss,
                     rss_threshold);
        return false;
      }
    }

    return true;
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

    // Sample resource state before execution
    auto stats = std::make_shared<TaskResourceStats>();
    stats->cpu_load_before = cpu_monitor_.get_load();
    stats->rss_mb_before = memory_monitor_.get_process_rss_mb();

    try {
      auto future = std::async(
          std::launch::async,
          [ntask = std::move(task), stats, this]() mutable noexcept
          {
            try {
              ntask.execute();
            } catch (const std::exception& e) {
              SPDLOG_ERROR("Task execution failed: {}", e.what());
            } catch (...) {
              SPDLOG_ERROR("Task execution failed with unknown exception");
            }
            // Sample after execution
            stats->cpu_load_after = cpu_monitor_.get_load();
            stats->rss_mb_after = memory_monitor_.get_process_rss_mb();

            // Wake scheduler so it can immediately dispatch pending work
            cv_.notify_one();
          });

      active_tasks_.push_back(ActiveTask {
          .future = std::move(future),
          .estimated_threads = threads_needed,
          .start_time = std::chrono::steady_clock::now(),
          .resource_stats = std::move(stats),
      });
      tasks_active_.fetch_add(1, std::memory_order_relaxed);

    } catch (...) {
      active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
      throw;
    }
  }

  void log_periodic_stats() const
  {
    try {
      const auto stats = get_statistics();
      spdlog::info(
          "[{}] CPU: {:.1f}%  MEM: {:.0f} MB free ({:.1f}%)  "
          "RSS: {:.0f} MB  Active: {}/{}  Pending: {}  Done: {}",
          name_,
          stats.current_cpu_load,
          stats.available_memory_mb,
          stats.current_memory_percent,
          stats.process_rss_mb,
          stats.active_threads,
          max_threads_,
          stats.tasks_pending,
          stats.tasks_completed);
    } catch (const std::exception& e) {
      SPDLOG_WARN("log_periodic_stats failed: {}", e.what());
    }
  }

  void log_final_stats() const
  {
    try {
      const auto stats = get_statistics();
      const auto elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - pool_start_time_)
              .count();
      spdlog::info(
          "[{}] Final: {} tasks dispatched, {} completed, "
          "avg CPU {:.1f}%, avg mem delta {:.1f} MB, wall {:.1f}s",
          name_,
          stats.lp_tasks_dispatched,
          stats.tasks_completed,
          stats.avg_task_cpu_pct,
          stats.avg_task_rss_delta_mb,
          elapsed);
    } catch (const std::exception& e) {
      SPDLOG_WARN("log_final_stats failed: {}", e.what());
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
