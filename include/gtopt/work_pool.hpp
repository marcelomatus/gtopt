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
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtopt/cpu_monitor.hpp>
#include <gtopt/hardware_info.hpp>
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
  /// Hard cap on process bytes paged to swap (MB).  When VmSwap exceeds
  /// this, dispatch is blocked to let active tasks drain and release
  /// memory instead of pushing more pages out.  Default 2048 MB — kicks
  /// in before the kernel starts thrashing; set to 0 to disable.
  double max_process_swap_mb {2048.0};
  /// Soft cap on system swap I/O rate (pages/sec, sum of pswpin+pswpout).
  /// When the kernel is thrashing above this rate, dispatch is blocked.
  /// Only evaluated near thread saturation so quiescent paging (e.g.
  /// init-time swap readahead) does not stall the pool.  0 = disabled.
  double max_swap_io_per_sec {0.0};

  explicit WorkPoolConfig(
      int max_threads_ = static_cast<int>(physical_concurrency()),
      double max_cpu_threshold_ = 95.0,
      double min_free_memory_mb_ = 4096.0,
      double max_memory_percent_ = 90.0,
      double max_process_rss_mb_ = 0.0,
      std::chrono::milliseconds scheduler_interval_ =
          std::chrono::milliseconds(50),
      std::string name_ = "WorkPool",
      bool enable_periodic_stats_ = true,
      double max_process_swap_mb_ = 2048.0,
      double max_swap_io_per_sec_ = 0.0) noexcept
      : max_threads(max_threads_)
      , max_cpu_threshold(max_cpu_threshold_)
      , min_free_memory_mb(min_free_memory_mb_)
      , max_memory_percent(max_memory_percent_)
      , max_process_rss_mb(max_process_rss_mb_)
      , scheduler_interval(scheduler_interval_)
      , name(std::move(name_))
      , enable_periodic_stats(enable_periodic_stats_)
      , max_process_swap_mb(max_process_swap_mb_)
      , max_swap_io_per_sec(max_swap_io_per_sec_)
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
  mutable std::mutex active_mutex_;  // Protects per-task accumulators
  std::condition_variable cv_;  // Worker wakeups (submit / completion)
  std::vector<Task<void, Key, KeyCompare>> task_queue_;

  // Stats thread has its own mutex/cv so that `submit()`'s notify_one()
  // never targets the stats thread by accident — that would consume the
  // notification without dispatching the task and leave the queue stuck
  // until the next submit.  Sharing `cv_` between N workers and one
  // stats thread caused exactly this missed-wakeup hang in the SDDP
  // test suite (1/(N+1) per submit).
  mutable std::mutex stats_mutex_;
  std::condition_variable stats_cv_;

  // Persistent worker threads — each runs `worker_loop()`, pulling tasks
  // from `task_queue_` directly.  Replaces the prior design that spawned
  // one fresh `pthread` per task via `std::async(std::launch::async, ...)`,
  // which dominated the load average on long SDDP runs (each task created
  // and destroyed an OS thread, churning glibc/jemalloc per-thread state).
  std::vector<std::jthread> workers_;
  std::jthread stats_thread_;

  gtopt::CPUMonitor cpu_monitor_;
  gtopt::MemoryMonitor memory_monitor_;
  std::atomic<int> active_threads_ {0};
  std::atomic<bool> running_ {false};

  int max_threads_;
  double max_cpu_threshold_;
  double min_free_memory_mb_;
  double max_memory_percent_;
  double max_process_rss_mb_;
  double max_process_swap_mb_;
  double max_swap_io_per_sec_;
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

  // Throttle event counters.  Atomic so `can_dispatch_next()` can bump
  // them without taking the active mutex.  `mutable` because they are
  // incremented from `should_schedule_new_task() const` — they form
  // pure diagnostic state (like a mutex), not logical pool state.
  // Reported in the pool's Final log line so operators see at a glance
  // which gate (if any) held work back.
  mutable std::atomic<size_t> throttled_cpu_ {0};
  mutable std::atomic<size_t> throttled_memory_pct_ {0};
  mutable std::atomic<size_t> throttled_free_memory_ {0};
  mutable std::atomic<size_t> throttled_process_rss_ {0};
  mutable std::atomic<size_t> throttled_process_swap_ {0};
  mutable std::atomic<size_t> throttled_swap_io_ {0};

  // Stall detection: tracked across `log_periodic_stats()` calls to detect
  // when `tasks_completed` stops advancing while work is still queued.
  mutable size_t last_logged_completed_ {0};
  mutable int stall_intervals_ {0};

public:
  BasicWorkPool(BasicWorkPool&&) = delete;
  BasicWorkPool(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&) = delete;
  BasicWorkPool& operator=(const BasicWorkPool&&) = delete;

  explicit BasicWorkPool(WorkPoolConfig config = WorkPoolConfig {})
      : max_threads_(config.max_threads)
      , max_cpu_threshold_(config.max_cpu_threshold)
      , min_free_memory_mb_(config.min_free_memory_mb)
      , max_memory_percent_(config.max_memory_percent)
      , max_process_rss_mb_(config.max_process_rss_mb)
      , max_process_swap_mb_(config.max_process_swap_mb)
      , max_swap_io_per_sec_(config.max_swap_io_per_sec)
      , scheduler_interval_(config.scheduler_interval)
      , name_(std::move(config.name))
      , enable_periodic_stats_(config.enable_periodic_stats)
  {
    spdlog::info(
        "  {} initialized: {} max threads, {:.0f}% CPU threshold, "
        "{:.0f} MB min free mem, {:.0f}% max mem{}{}{}",
        name_,
        max_threads_,
        max_cpu_threshold_,
        min_free_memory_mb_,
        max_memory_percent_,
        max_process_rss_mb_ > 0
            ? std::format(", {:.0f} MB max RSS", max_process_rss_mb_)
            : "",
        max_process_swap_mb_ > 0
            ? std::format(", {:.0f} MB max VmSwap", max_process_swap_mb_)
            : "",
        max_swap_io_per_sec_ > 0
            ? std::format(", {:.0f} pg/s max swap I/O", max_swap_io_per_sec_)
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

      // Workers are spawned **lazily** by `submit()` / `submit_batch()`
      // (see `maybe_spawn_worker_unlocked()`).  Reserving up to
      // `max_threads_` slots avoids reallocation as the pool grows;
      // actual `std::jthread` construction defers until the first
      // submission that needs new capacity.  This preserves the
      // persistent-worker invariant (no per-task pthread_create churn,
      // unlike the old `std::async` design) while keeping idle pools
      // free of unused OS threads — important under heavy test
      // parallelism (e.g. `ctest -j20`) where many short-lived pools
      // with `max_threads = physical_concurrency()` would otherwise
      // sum to hundreds of mostly-idle pthreads.
      workers_.reserve(static_cast<std::size_t>(max_threads_));

      // Periodic stats thread: replaces the old scheduler thread's
      // logging duty.  Sleeps on `cv_` with a 30 s timeout so it wakes
      // promptly on shutdown without polling.
      if (enable_periodic_stats_) {
        stats_thread_ = std::jthread {
            [this](const std::stop_token& stoken)
            {
#ifdef __linux__
              pthread_setname_np(pthread_self(), "WPStats");
#endif
              constexpr auto log_interval = std::chrono::seconds(30);
              while (!stoken.stop_requested() && running_) {
                std::unique_lock lock(stats_mutex_);
                stats_cv_.wait_for(
                    lock,
                    log_interval,
                    [&]
                    { return stoken.stop_requested() || !running_.load(); });
                if (stoken.stop_requested() || !running_.load()) {
                  break;
                }
                lock.unlock();
                try {
                  log_periodic_stats();
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                  // log_periodic_stats already swallows internally; this
                  // is belt-and-suspenders so the stats thread never
                  // dies on a transient logging failure.
                }
              }
            }};
      }
    } catch (const std::exception& e) {
      running_ = false;
      auto msg = std::format("Failed to start BasicWorkPool: {}", e.what());
      SPDLOG_ERROR(msg);
      throw std::runtime_error(msg);
    }
  }

  void shutdown()
  {
    // Phase 1: under queue_mutex_, atomically mark the pool not running
    // and stop every spawned worker.  Holding the lock during this step
    // prevents `submit()`/`submit_batch()`/`maybe_spawn_worker_unlocked`
    // from racing with `workers_.clear()` later — a previous design that
    // mutated `running_` outside the lock allowed a submit-spawn to
    // emplace a new `jthread` while the destructor was iterating
    // `workers_` for join, leaving an orphan worker on a freed `this`.
    {
      const std::scoped_lock<std::mutex> lock(queue_mutex_);
      if (!running_.load(std::memory_order_relaxed)) {
        return;
      }
      running_.store(false, std::memory_order_relaxed);
      for (auto& w : workers_) {
        w.request_stop();
      }
    }
    cv_.notify_all();

    // Phase 2: join with the lock released so workers can drain.
    // Workers in `cv_.wait` see `!running_` via the predicate; workers
    // in the gate-failure `cv_.wait_for` see it on the next iteration
    // top after the timeout (≤ scheduler_interval_).
    for (auto& w : workers_) {
      if (w.joinable()) {
        w.join();
      }
    }

    // Phase 3: now that no worker can run, it is safe to clear the
    // vector.  No `submit()` can spawn into it because Phase 1 set
    // `running_ = false` under the same lock that `submit()` takes.
    {
      const std::scoped_lock<std::mutex> lock(queue_mutex_);
      workers_.clear();
    }

    if (stats_thread_.joinable()) {
      stats_thread_.request_stop();
      stats_cv_.notify_all();
      stats_thread_.join();
    }

    tasks_active_.store(0, std::memory_order_relaxed);

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

      {
        const std::scoped_lock<std::mutex> lock(queue_mutex_);
        // Reject post-shutdown submissions before touching the queue.
        // `running_` is read inside the lock so it serialises against
        // the matching write in `shutdown()`'s phase 1.
        if (!running_.load(std::memory_order_relaxed)) {
          return std::unexpected(
              std::make_error_code(std::errc::operation_canceled));
        }
        try {
          // Both emplace_back and push_heap are under queue_mutex_, so
          // no other thread can observe the intermediate state.
          task_queue_.emplace_back([task]() { (*task)(); }, req);
          std::ranges::push_heap(task_queue_, std::less<> {});
          tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
          tasks_pending_.fetch_add(1, std::memory_order_relaxed);
          // Lazy spawn: if all current workers are busy and we haven't
          // hit the cap, add another.  Must run under queue_mutex_ to
          // serialise mutations of `workers_`.
          maybe_spawn_worker_unlocked();
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

      // notify_all (vs notify_one) is intentional: under high system
      // load with many short-lived pools, a single notify_one can race
      // a worker that just finished a task and is between
      // `cv_.notify_all()` (its own end-of-task wake) and re-entering
      // `cv_.wait`.  notify_all guarantees the wakeup reaches every
      // current waiter; idle workers re-park immediately after the
      // predicate check, so the cost is one re-wake per worker per
      // submit — negligible compared to the cost of a missed wakeup.
      cv_.notify_all();
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
      if (!running_.load(std::memory_order_relaxed)) {
        // Reject all batch entries with operation_canceled; the queue
        // is sealed because shutdown() phase 1 ran.
        for ([[maybe_unused]] const auto& _ : tasks) {
          results.push_back(std::unexpected(
              std::make_error_code(std::errc::operation_canceled)));
        }
        return results;
      }
      for (auto& [func, req] : tasks) {
        try {
          auto ptask = std::make_shared<std::packaged_task<ReturnType()>>(
              std::move(func));
          // Build the future locally — only push to `results` after the
          // queue-side operations succeed.  The previous order pushed
          // the future first, so a `length_error` from `emplace_back`
          // or a throwing comparator in `push_heap` would leave the
          // caller with a "successful" `expected<future>` for a task
          // that was never enqueued; the `packaged_task` destructs at
          // scope exit and `.get()` raises `broken_promise` instead of
          // surfacing the queue error.  Building the future locally
          // first keeps `results.size() == tasks.size()` and ensures
          // every entry matches the enqueue outcome.
          auto fut = ptask->get_future();
          task_queue_.emplace_back([ptask]() { (*ptask)(); }, req);
          std::ranges::push_heap(task_queue_, std::less<> {});
          tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
          tasks_pending_.fetch_add(1, std::memory_order_relaxed);
          results.push_back(std::move(fut));
          maybe_spawn_worker_unlocked();
        } catch (const std::exception& e) {
          SPDLOG_ERROR("Failed to enqueue batch task: {}", e.what());
          results.push_back(std::unexpected(
              std::make_error_code(std::errc::operation_not_permitted)));
        }
      }
    }

    cv_.notify_all();
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
    double process_swap_mb;  ///< Process VmSwap in MB
    double swap_used_mb;  ///< System swap used in MB
    double swap_io_rate;  ///< Pages/sec (pswpin + pswpout)
    size_t lp_tasks_dispatched;  ///< Total LP tasks dispatched
    double avg_task_cpu_pct;  ///< Average CPU % per LP task
    double avg_task_rss_delta_mb;  ///< Average RSS delta per LP task
    /// @name Throttle event counters
    /// Count of `can_dispatch_next()` returning false for each reason.
    /// The same scheduling tick may exercise multiple gates; each
    /// failing gate bumps its own counter.  Zero on a well-fed pool;
    /// non-zero indicates the pool was holding back work for that
    /// reason.  Useful for diagnosing "why is my pool only at 50 %
    /// CPU?" without turning on DEBUG logs.
    /// @{
    size_t throttled_cpu;
    size_t throttled_memory_pct;
    size_t throttled_free_memory;
    size_t throttled_process_rss;
    size_t throttled_process_swap;
    size_t throttled_swap_io;
    /// @}
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
        .process_swap_mb = memory_monitor_.get_process_swap_mb(),
        .swap_used_mb = memory_monitor_.get_swap_used_mb(),
        .swap_io_rate = memory_monitor_.get_swap_io_rate(),
        .lp_tasks_dispatched = dispatched,
        .avg_task_cpu_pct = avg_cpu,
        .avg_task_rss_delta_mb = avg_mem,
        .throttled_cpu = throttled_cpu_.load(std::memory_order_relaxed),
        .throttled_memory_pct =
            throttled_memory_pct_.load(std::memory_order_relaxed),
        .throttled_free_memory =
            throttled_free_memory_.load(std::memory_order_relaxed),
        .throttled_process_rss =
            throttled_process_rss_.load(std::memory_order_relaxed),
        .throttled_process_swap =
            throttled_process_swap_.load(std::memory_order_relaxed),
        .throttled_swap_io = throttled_swap_io_.load(std::memory_order_relaxed),
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
          "  Swap: VmSwap {:.0f} MB, system used {:.0f} MB, I/O {:.0f} pg/s\n"
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
          stats.process_swap_mb,
          stats.swap_used_mb,
          stats.swap_io_rate,
          stats.lp_tasks_dispatched,
          stats.avg_task_cpu_pct,
          stats.avg_task_rss_delta_mb);
    } catch (...) {
      return "WorkPool statistics unavailable";
    }
  }

  void log_statistics() const { spdlog::info(format_statistics()); }

private:
  /// Lazy worker spawn.  Called from `submit()` / `submit_batch()`
  /// with `queue_mutex_` held, after the new task has been pushed.
  ///
  /// Spawn decision is `pending > idle` rather than `active >= total`.
  /// The earlier `active < total` heuristic had a race: when N tasks
  /// were submitted in burst to a fresh pool, the first submit spawned
  /// W1 but W1 had not yet incremented `active_threads_`, so the
  /// second-through-Nth submits all observed `active(0) < total(1)`
  /// and skipped spawning even though every additional task needed
  /// its own worker.  The pool degenerated to one serial worker.
  ///
  /// `pending` is read directly from `task_queue_.size()` (we hold
  /// `queue_mutex_`).  `idle` is bounded above by `total - active`;
  /// using that upper bound is conservative — we may over-spawn by
  /// one when an idle worker is about to wake, but the extra worker
  /// just re-parks on `cv_.wait`.  The race that actually matters
  /// (under-spawning) is gone.
  void maybe_spawn_worker_unlocked()
  {
    if (!running_.load(std::memory_order_relaxed)) {
      return;  // pool is shutting down — do not create new workers
    }
    const auto total = workers_.size();
    if (total >= static_cast<std::size_t>(max_threads_)) {
      return;
    }
    const auto active = active_threads_.load(std::memory_order_relaxed);
    const auto idle =
        active < static_cast<int>(total) ? static_cast<int>(total) - active : 0;
    const auto pending = static_cast<int>(task_queue_.size());
    if (pending <= idle) {
      // Existing idle workers can absorb the queue — `notify_all()`
      // from the caller will wake them.
      return;
    }
    workers_.emplace_back(
        [this, i = total](std::stop_token stoken)
        {
#ifdef __linux__
          const auto thread_name = std::format("WP-{}-{}", name_, i);
          pthread_setname_np(pthread_self(), thread_name.substr(0, 15).c_str());
#endif
          worker_loop(stoken);
        });
  }

  /// Worker loop: each persistent thread runs this until shutdown.
  ///
  /// Pulls the highest-priority dispatchable task from `task_queue_`,
  /// executes it inline, then loops.  Replaces the prior scheduler /
  /// `std::async` pattern that spawned a fresh OS thread per task.
  ///
  /// Throttle gates (CPU, memory %, free memory, RSS, swap, swap I/O)
  /// are checked against the head of the queue with `queue_mutex_`
  /// held; on a gate failure the worker releases the lock and sleeps
  /// for `scheduler_interval_` before re-checking, giving other tasks
  /// a chance to drain.
  void worker_loop(const std::stop_token& stoken)
  {
    while (!stoken.stop_requested()) {
      std::unique_lock<std::mutex> lock(queue_mutex_);

      cv_.wait(lock,
               [&]
               {
                 return stoken.stop_requested() || !running_.load()
                     || !task_queue_.empty();
               });

      if (task_queue_.empty()) {
        if (stoken.stop_requested() || !running_.load()) {
          return;
        }
        continue;  // spurious wakeup
      }

      if (!can_dispatch_top()) {
        // Gate blocked dispatch.  Wait on `cv_` with a timeout AND a
        // predicate that includes shutdown so `shutdown()` can break
        // us out immediately rather than waiting up to
        // scheduler_interval_ for the timeout.  Plain `sleep_for` (or
        // a `wait_for` without predicate) would also miss
        // submit-side notifies during the back-off and starve the
        // queue under sustained back-pressure.
        cv_.wait_for(lock,
                     scheduler_interval_,
                     [&]
                     { return stoken.stop_requested() || !running_.load(); });
        continue;
      }

      // Pop the top task (max-heap pop pattern).
      std::ranges::pop_heap(task_queue_, std::less<> {});
      Task<void, Key, KeyCompare> task = std::move(task_queue_.back());
      task_queue_.pop_back();

      const auto threads_needed = task.requirements().estimated_threads;
      tasks_pending_.fetch_sub(1, std::memory_order_relaxed);
      active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);
      tasks_active_.fetch_add(1, std::memory_order_relaxed);

      lock.unlock();

      // Sample resources before execution
      TaskResourceStats rs {};
      rs.cpu_load_before = cpu_monitor_.get_load();
      rs.rss_mb_before = memory_monitor_.get_process_rss_mb();
      const auto t_start = std::chrono::steady_clock::now();

      try {
        task.execute();
      } catch (const std::exception& e) {
        SPDLOG_ERROR("Task execution failed: {}", e.what());
      } catch (...) {
        SPDLOG_ERROR("Task execution failed with unknown exception");
      }

      rs.cpu_load_after = cpu_monitor_.get_load();
      rs.rss_mb_after = memory_monitor_.get_process_rss_mb();
      rs.duration_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t_start)
                          .count();

      // Account.  The dispatch-relevant counters (`active_threads_`,
      // `tasks_active_`, `tasks_completed_`) are decremented under
      // `queue_mutex_` so that any subsequent `can_dispatch_top()` /
      // `maybe_spawn_worker_unlocked()` reading them sees the fresh
      // value via mutex-acquire happens-before.  Previously the
      // decrement was under `active_mutex_` only, leaving a small
      // window where a dispatch decision could be stale by one task —
      // self-correcting within one tick, but not auditable.
      // `active_mutex_` now protects only the per-task accumulators.
      {
        const std::scoped_lock<std::mutex> qlock(queue_mutex_);
        active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
        tasks_active_.fetch_sub(1, std::memory_order_relaxed);
        tasks_completed_.fetch_add(1, std::memory_order_relaxed);
      }
      {
        const std::scoped_lock alock(active_mutex_);
        ++lp_tasks_dispatched_;
        total_task_cpu_pct_ += (rs.cpu_load_before + rs.cpu_load_after) / 2.0;
        total_task_rss_delta_mb_ += (rs.rss_mb_after - rs.rss_mb_before);
      }

      // Wake any worker that was throttled by `current + threads_needed
      // > max_threads_`: one of those checks may now pass against the
      // freshly-decremented `active_threads_`.
      cv_.notify_all();
    }
  }

  /// Returns true iff the head of `task_queue_` may be dispatched right
  /// now given current concurrency, CPU and memory pressure.  Caller
  /// must hold `queue_mutex_`.  Increments throttle counters on each
  /// failing gate so operators can see which gate held work back.
  bool can_dispatch_top() const
  {
    if (task_queue_.empty()) {
      return false;
    }

    const auto& next_task = task_queue_.front();
    const auto threads_needed = next_task.requirements().estimated_threads;
    const auto current_threads =
        active_threads_.load(std::memory_order_relaxed);

    if (current_threads + threads_needed > max_threads_) {
      return false;
    }

    const auto is_critical =
        next_task.requirements().priority == TaskPriority::Critical;

    // CPU check — only apply when threads are near saturation.
    // Thread count is the primary concurrency limiter; CPU load is a
    // secondary guard that only matters when cores are already busy.
    //
    // For very small pools (max_threads < 4), `max_threads * 0.8`
    // truncates to 0 or 1, making the saturation check a permanent
    // true.  Combined with the per-pool-size threshold formula
    // (e.g. 50 % for max_threads = 1) this would starve dispatch on
    // any background-loaded host (`ctest -j20` consistently sits at
    // 50 %+ system CPU).  Skip the gate entirely for small pools —
    // they cannot meaningfully oversubscribe.
    if (max_threads_ >= 4
        && current_threads + threads_needed
            >= static_cast<int>(max_threads_ * 0.8))
    {
      const auto cpu_load = cpu_monitor_.get_physical_load();
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
        throttled_cpu_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }

    // Memory checks (Critical tasks get relaxed thresholds)
    const auto mem_pct = memory_monitor_.get_memory_percent();
    const auto mem_threshold = is_critical ? 98.0 : max_memory_percent_;
    if (mem_pct >= mem_threshold) {
      throttled_memory_pct_.fetch_add(1, std::memory_order_relaxed);
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
      throttled_free_memory_.fetch_add(1, std::memory_order_relaxed);
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
        throttled_process_rss_.fetch_add(1, std::memory_order_relaxed);
        SPDLOG_DEBUG("{}: blocked by process RSS {:.0f} MB >= {:.0f} MB",
                     name_,
                     rss,
                     rss_threshold);
        return false;
      }
    }

    // Swap-pressure gates: once pages are going to/from swap, adding work
    // tends to deepen the thrash.  Critical tasks get 10% headroom to
    // avoid deadlocking progress when the pool is already paging.
    if (max_process_swap_mb_ > 0.0) {
      const auto vmswap = memory_monitor_.get_process_swap_mb();
      const auto swap_threshold =
          is_critical ? max_process_swap_mb_ * 1.1 : max_process_swap_mb_;
      if (vmswap >= swap_threshold) {
        throttled_process_swap_.fetch_add(1, std::memory_order_relaxed);
        SPDLOG_DEBUG("{}: blocked by VmSwap {:.0f} MB >= {:.0f} MB",
                     name_,
                     vmswap,
                     swap_threshold);
        return false;
      }
    }

    // Only enforce the swap I/O rate gate once threads are near saturation —
    // low-concurrency activity can benignly trigger a few pages/sec of
    // swap-in as fresh code is faulted in.  Same small-pool guard as
    // the CPU gate: with max_threads < 4 the saturation check would
    // be a permanent true and starve dispatch.
    if (max_swap_io_per_sec_ > 0.0 && max_threads_ >= 4
        && current_threads + threads_needed
            >= static_cast<int>(max_threads_ * 0.8))
    {
      const auto rate = memory_monitor_.get_swap_io_rate();
      const auto rate_threshold =
          is_critical ? max_swap_io_per_sec_ * 2.0 : max_swap_io_per_sec_;
      if (rate >= rate_threshold) {
        throttled_swap_io_.fetch_add(1, std::memory_order_relaxed);
        SPDLOG_DEBUG("{}: blocked by swap I/O rate {:.0f} pg/s >= {:.0f} pg/s",
                     name_,
                     rate,
                     rate_threshold);
        return false;
      }
    }

    // Load-average gating was previously here; removed because
    // `getloadavg(3)` is system-wide (sums over all processes) and
    // 60-s EWMA-smoothed — wrong scope and wrong horizon for a
    // per-pool dispatch decision.  On shared hosts (CI, multi-tenant
    // solver boxes) every pool saw the same aggregated load and
    // throttled simultaneously, which is collective deadlock rather
    // than back-pressure.  Operators that need fairness on a shared
    // host should use kernel-enforced limits (cgroup `cpu.max`,
    // `cpuset`, `taskset`, `systemd-run --slice`).

    return true;
  }

  void log_periodic_stats() const
  {
    try {
      const auto stats = get_statistics();
      // Only include swap fields in the line when they have signal — keeps
      // the common case compact while making thrash visible when it starts.
      const auto swap_tail =
          (stats.process_swap_mb > 0.0 || stats.swap_io_rate > 0.0)
          ? std::format("  Swap: {:.0f} MB ({:.0f} pg/s)",
                        stats.process_swap_mb,
                        stats.swap_io_rate)
          : std::string {};
      spdlog::info(
          "[{}] CPU: {:.1f}%  MEM: {:.0f} MB free ({:.1f}%)  "
          "RSS: {:.0f} MB{}  Active: {}/{}  Pending: {}  Done: {}",
          name_,
          stats.current_cpu_load,
          stats.available_memory_mb,
          stats.current_memory_percent,
          stats.process_rss_mb,
          swap_tail,
          stats.active_threads,
          max_threads_,
          stats.tasks_pending,
          stats.tasks_completed);

      // Stall detection: when `tasks_completed` has not advanced since the
      // previous periodic log yet work is still queued or running, surface
      // the dispatch gate that is blocking progress.  Mirrors the checks in
      // `can_dispatch_next()` so the user sees the same condition the
      // scheduler sees.
      const bool has_work = stats.tasks_pending > 0 || stats.tasks_active > 0;
      const bool no_progress = stats.tasks_completed == last_logged_completed_;
      if (has_work && no_progress) {
        ++stall_intervals_;
      } else {
        stall_intervals_ = 0;
      }
      last_logged_completed_ = stats.tasks_completed;

      if (stall_intervals_ >= 2) {
        std::string reason;
        if (stats.current_memory_percent >= max_memory_percent_) {
          reason = std::format("memory usage {:.1f}% >= {:.1f}%",
                               stats.current_memory_percent,
                               max_memory_percent_);
        } else if (stats.available_memory_mb < min_free_memory_mb_
                   && stats.available_memory_mb > 0.0)
        {
          reason = std::format("free memory {:.0f} MB < {:.0f} MB",
                               stats.available_memory_mb,
                               min_free_memory_mb_);
        } else if (max_process_rss_mb_ > 0.0
                   && stats.process_rss_mb >= max_process_rss_mb_)
        {
          reason = std::format("process RSS {:.0f} MB >= {:.0f} MB",
                               stats.process_rss_mb,
                               max_process_rss_mb_);
        } else if (max_process_swap_mb_ > 0.0
                   && stats.process_swap_mb >= max_process_swap_mb_)
        {
          reason = std::format("process VmSwap {:.0f} MB >= {:.0f} MB",
                               stats.process_swap_mb,
                               max_process_swap_mb_);
        } else if (max_swap_io_per_sec_ > 0.0
                   && stats.swap_io_rate >= max_swap_io_per_sec_)
        {
          reason = std::format("swap thrashing {:.0f} pg/s >= {:.0f} pg/s",
                               stats.swap_io_rate,
                               max_swap_io_per_sec_);
        } else if (stats.active_threads + 1
                       >= static_cast<int>(max_threads_ * 0.8)
                   && stats.current_cpu_load >= max_cpu_threshold_)
        {
          reason = std::format("CPU load {:.1f}% >= {:.1f}%",
                               stats.current_cpu_load,
                               max_cpu_threshold_);
        } else if (stats.swap_io_rate > 0.0) {
          reason = std::format(
              "active task(s) not completing (kernel paging "
              "{:.0f} pg/s — likely thrash)",
              stats.swap_io_rate);
        } else {
          reason =
              "active task(s) not completing (external block or blocked I/O)";
        }
        SPDLOG_WARN(
            "[{}] no progress for {} intervals ({} pending, {} active): {}",
            name_,
            stall_intervals_,
            stats.tasks_pending,
            stats.tasks_active,
            reason);
      }
    } catch (const std::exception& e) {
      SPDLOG_WARN("log_periodic_stats failed: {}", e.what());
    }
  }

  void log_final_stats() const
  {
    try {
      const auto stats = get_statistics();
      // Skip the noisy "Final: 0 tasks dispatched, 0 completed ..." that
      // otherwise emits whenever a pool is constructed for a code path
      // (hot-start cut load, monitoring init, ...) that ends up not
      // dispatching any work.
      if (stats.lp_tasks_dispatched == 0 && stats.tasks_completed == 0) {
        return;
      }
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

      // Throttle summary — only emit when at least one gate fired so a
      // healthy pool stays silent.  Operators use this to diagnose
      // "why is my pool only at 50% CPU?" without re-running with
      // DEBUG logs.  Each counter is the number of schedule ticks on
      // which that gate blocked dispatch.
      const auto total_throttle = stats.throttled_cpu
          + stats.throttled_memory_pct + stats.throttled_free_memory
          + stats.throttled_process_rss + stats.throttled_process_swap
          + stats.throttled_swap_io;
      if (total_throttle > 0) {
        // Compact form: only emit gates that actually fired.  The juan
        // run had every counter at 0 except cpu, producing the noisy
        // ``cpu=35876 mem%=0 free_mem=0 rss=0 swap=0 swap_io=0`` line —
        // an operator only needs to see that it was the CPU gate.  When
        // multiple gates fire we still surface all of them so a swap-IO
        // bottleneck is still distinguishable from a free-memory one.
        std::string parts;
        const auto add = [&](std::string_view label, std::size_t v)
        {
          if (v > 0) {
            if (!parts.empty()) {
              parts.append(" ");
            }
            parts.append(std::format("{}={}", label, v));
          }
        };
        add("cpu", stats.throttled_cpu);
        add("mem%", stats.throttled_memory_pct);
        add("free_mem", stats.throttled_free_memory);
        add("rss", stats.throttled_process_rss);
        add("swap", stats.throttled_process_swap);
        add("swap_io", stats.throttled_swap_io);
        spdlog::info(
            "[{}]   throttle: {} (total={})", name_, parts, total_throttle);
      }
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
