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
#include <cmath>
#include <condition_variable>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtopt/cpu_monitor.hpp>
#include <gtopt/hardware_info.hpp>
#include <gtopt/memory_monitor.hpp>
#include <gtopt/resource_governor.hpp>
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
  /// Absolute upper bound the pool may grow `max_threads` to at runtime
  /// once observed per-task RSS deltas show the initial (memory-clamped)
  /// thread count left headroom unused.  0 = no growth (ceiling equals
  /// `max_threads`).  Typically set to the un-clamped CPU budget
  /// (`cpu_factor × cores`) so a conservative bootstrap clamp can recover
  /// to full parallelism when tasks turn out to be cheap.
  int max_threads_ceiling {0};

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
      double max_swap_io_per_sec_ = 0.0,
      int max_threads_ceiling_ = 0) noexcept
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
      , max_threads_ceiling(max_threads_ceiling_)
  {
  }
};

/// Result of the proactive memory-aware thread clamp: the conservative
/// initial thread count and the absolute (CPU-budget) growth ceiling.
struct MemoryClamp
{
  int initial_threads;  ///< Conservative starting `max_threads`
  int ceiling_threads;  ///< Un-clamped CPU budget (growth ceiling)
};

/// Compute the thread count and growth ceiling for a work pool.
///
/// `cpu_factor × physical_concurrency()` is the absolute CPU budget — the
/// ceiling.  There are NO a-priori per-task size estimates anywhere.
///
/// Worker-thread count is DECOUPLED from memory throttling: the pool always
/// runs at the full ceiling so worker threads stay available even while the
/// dispatch gate throttles memory-heavy work.  This is required by the SDDP
/// backward pass's blocking-slot pattern (`release_slot_while_blocking`): a
/// parent cell-task drops its slot and blocks on child futures, and a free
/// worker must be able to run that child.  Clamping the worker count below
/// `cell_task_headroom` deadlocks the pass — observed as a livelock at
/// RSS ≫ limit with the rss-throttle counter spinning and no progress.
///
/// Memory is bounded purely by the live dispatch controller in
/// `BasicWorkPool::can_dispatch_task` (measured idle-floor + marginal
/// per-task cost + one-admit-per-monitor-interval rate limit + always-admit-
/// when-idle progress guarantee).  The rate limit serialises admission to
/// ~one task per monitor interval regardless of worker count, so keeping all
/// workers available can never cause a dispatch burst.
[[nodiscard]] inline MemoryClamp memory_clamp_threads(
    double cpu_factor, double memory_limit_mb, std::string_view pool_label)
{
  const int ceiling = std::max(
      1, static_cast<int>(std::lround(cpu_factor * physical_concurrency())));
  if (memory_limit_mb > 0.0) {
    spdlog::info(
        "{}: memory limit {:.0f} MB — {} worker(s) available; memory bounded "
        "by the rate-limited measured-memory dispatch gate (workers decoupled "
        "from throttling for deadlock-freedom; no fixed per-task estimate).",
        pool_label,
        memory_limit_mb,
        ceiling);
  }
  return {ceiling, ceiling};
}

/// Join every future in @p futures: `.get()` each one, capturing the FIRST
/// exception and rethrowing it only after ALL futures have completed.
/// Invalid (default-constructed / moved-from) entries are skipped.
///
/// This is the mandatory barrier idiom for pool fan-outs whose tasks
/// reference stack locals: a bare `for (auto& f : futures) f.get();`
/// rethrows mid-loop, and the unwinding then destroys locals (build
/// buffers, mutexes, option structs) that still-running sibling tasks
/// are reading — a use-after-free, because the pool's destructor only
/// joins its workers AFTER later-declared locals are gone.  Draining
/// every future first guarantees no task from this fan-out is in
/// flight when the exception propagates.
inline void get_all_futures(std::span<std::future<void>> futures)
{
  std::exception_ptr first_error;
  for (auto& fut : futures) {
    if (!fut.valid()) {
      continue;
    }
    try {
      fut.get();
    } catch (...) {
      if (first_error == nullptr) {
        first_error = std::current_exception();
      }
    }
  }
  if (first_error != nullptr) {
    std::rethrow_exception(first_error);
  }
}

/// Identity of the pool (if any) whose `worker_loop` owns the current
/// thread.  Set once at worker start; never reset (the thread dies with
/// its pool).  Lets `release_slot_for_blocking_` distinguish "called
/// from one of MY workers" (release is correct) from "called from a
/// coordinator / foreign-pool thread" (release would decrement some
/// OTHER task's slot and over-admit) — a `tasks_active_ > 0` check
/// alone cannot tell those apart.
inline thread_local const void* this_thread_worker_pool = nullptr;

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
  /// Sort key.  With the default `std::less<Key>` comparator on the pool,
  /// the task with the **smaller** key is dequeued first.  This key is the
  /// SOLE ordering criterion (see `Task::operator<`): `TaskPriority` no
  /// longer participates in queue ordering — it only controls the dispatch
  /// gate (Critical relaxes the memory/CPU gates).
  Key priority_key = Key {};
  /// Admission flag, fully decoupled from ordering.  When true, the task may
  /// be dispatched even when physical CPU load is at/above the pool's
  /// saturation threshold (it gets the same `max_cpu_threshold_ + 5 %`
  /// relaxation `TaskPriority::High` used to grant).  This splits the
  /// "bypass the CPU gate" intent off `TaskPriority` so a task can order LOW
  /// (by its `priority_key`) yet still bypass the gate — dissolving the
  /// SDDP sim⇄wedge trap where `High` was used only for gate-bypass but also
  /// reordered ahead of every same-iteration training task.
  bool gate_bypass = false;
  /// Dominant resource of the task.  `cpu` (default) keeps today's
  /// behaviour.  `gpu` additionally gates dispatch on the process-global
  /// GPU token bucket (ResourceGovernor) so GPU-backed solves (cuOpt) never
  /// fan out to `cpu_factor x cores` concurrency — in Default compute mode
  /// the device time-slices contexts, so oversubscription buys no
  /// throughput and risks VRAM OOM.  Stamped by `SolverTier` from the
  /// backend's declared `SolverResourceDescriptor`.
  ResourceClass resource_class = ResourceClass::cpu;
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
  /// Ordering is driven by the `priority_key` ALONE — `TaskPriority` does
  /// NOT participate.  Overloading the enum onto both queue ordering and the
  /// CPU-gate-bypass class created a trap: raising a task to `High` to let it
  /// bypass the CPU gate also reordered it ahead of every lower-tier task
  /// regardless of key (and demoting it back reintroduced the gate wedge).
  /// Gate-bypass now lives on the independent `gate_bypass` flag, leaving
  /// `priority_key` as the single source of truth for execution order.
  ///
  /// Ordering:
  ///  1. `Key` comparison via `KeyCompare`:
  ///     `KeyCompare(key1, key2) == true` ⟹ key1 has **higher** priority.
  ///     In a max-heap this means `operator<` returns true when `other`
  ///     has higher priority, i.e. `KeyCompare(other.key, this.key)`.
  ///     With the default `std::less<Key>`: smaller key → higher priority.
  ///  2. Tie-break: older submission → higher priority.
  bool operator<(const Task& other) const noexcept
  {
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
  mutable std::condition_variable
      cv_;  // Worker wakeups (submit / completion / monitor nudge)
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

  // Current dispatch ceiling.  Atomic + mutable at runtime: the
  // projected-RSS gate starts from a conservative memory clamp and
  // `maybe_grow_max_threads_unlocked()` raises this toward
  // `max_threads_ceiling_` once observed per-task deltas reveal unused
  // headroom (grow-only — see that method).
  std::atomic<int> max_threads_;
  /// Absolute upper bound on `max_threads_` growth — the un-clamped CPU
  /// budget (`cpu_factor × cores`).  When 0/unset it equals the initial
  /// `max_threads_`, disabling growth (back-compat default).
  int max_threads_ceiling_;
  double max_cpu_threshold_;
  double min_free_memory_mb_;
  double max_memory_percent_;
  double max_process_rss_mb_;
  double max_process_swap_mb_;
  double max_swap_io_per_sec_;
  std::chrono::milliseconds scheduler_interval_;
  std::string name_;
  bool enable_periodic_stats_;

  // ── Measured memory model (no fixed per-task estimates) ────────────────
  // `idle_floor_mb_` is the process RSS captured whenever the pool goes
  // idle (`active == 0`): the persistent, non-task footprint (heap +
  // resident snapshots + registries + cuts).  `meas_per_task_mb_` is the
  // MEASURED marginal cost of one active task, `(rss - floor) / active`,
  // kept as a slowly-decaying high-water mark so it tracks the true
  // per-task peak of the (roughly uniform) tasks without ever badly
  // underestimating.  Both are updated live from `/proc` via
  // `update_memory_model_()`; nothing here is a hardcoded size.
  mutable std::atomic<double> idle_floor_mb_ {0.0};
  mutable std::atomic<double> meas_per_task_mb_ {0.0};
  // Rate limiter: timestamp of the last admission.  Admitting at most one
  // task per memory-monitor interval guarantees each admission's real RSS
  // impact is observed (the monitor refreshes RSS on that cadence) before
  // the next admit — the structural fix for the "N tasks admitted before
  // RSS reflects any of them" burst that caused multi-x overshoots.
  mutable std::atomic<std::chrono::steady_clock::time_point> last_admit_time_ {
      std::chrono::steady_clock::time_point {}};

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
  mutable std::atomic<size_t> throttled_gpu_ {0};
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
      , max_threads_ceiling_(config.max_threads_ceiling > config.max_threads
                                 ? config.max_threads_ceiling
                                 : config.max_threads)
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
        "  {} initialized: {} max threads{}, {:.0f}% CPU threshold, "
        "{:.0f} MB min free mem, {:.0f}% max mem{}{}{}",
        name_,
        max_threads_.load(std::memory_order_relaxed),
        max_threads_ceiling_ > max_threads_.load(std::memory_order_relaxed)
            ? std::format(" (grows to {})", max_threads_ceiling_)
            : "",
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
    } catch (...) {
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
      // Reserve up to the growth ceiling so `maybe_grow_max_threads_unlocked`
      // never reallocates `workers_` while spawning extra threads.
      workers_.reserve(static_cast<std::size_t>(std::max(
          max_threads_.load(std::memory_order_relaxed), max_threads_ceiling_)));

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
                } catch (...) {
                  // log_periodic_stats already swallows internally; this
                  // is belt-and-suspenders so the stats thread never
                  // dies on a transient logging failure.
                }
              }
            },
        };
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
    using Clock = std::chrono::steady_clock;
    const auto t_shutdown_begin = Clock::now();

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
      // Signal helper threads (stats, cpu_monitor, memory_monitor) to
      // begin their own teardown in parallel with the worker drain
      // below.  Each helper sleeps on its own CV with a `wait_for`
      // capped at the helper's interval (stats=30 s, memory=500 ms,
      // cpu=100 ms); waking them now lets their `wait_for` predicate
      // observe the stop request immediately, so by the time the
      // monitor `.stop()` calls below are reached the helpers are
      // already winding down — `.stop()`'s subsequent `join()` is
      // near-instant rather than blocking up to `monitor_interval_`.
      // Pre-fix, the helpers' shutdown was strictly sequential after
      // the workers' drain, adding the helper interval to total
      // teardown latency on every short run.
      if (stats_thread_.joinable()) {
        stats_thread_.request_stop();
      }
      cpu_monitor_.request_stop();
      memory_monitor_.request_stop();
    }
    cv_.notify_all();
    // `running_` / the stats stop_token were flipped WITHOUT holding
    // `stats_mutex_` (they are guarded by `queue_mutex_`), so the stats
    // thread can be between its predicate check (which read the old
    // values) and its blocking wait — a bare notify in that window is
    // LOST and the thread oversleeps its full 30 s `wait_for` timeout,
    // stalling shutdown by up to that long.  Acquiring `stats_mutex_`
    // here cannot complete until that thread has actually blocked (it
    // holds the mutex through the predicate-to-wait window), so the
    // notify that follows is guaranteed to reach it.  The worker `cv_`
    // above does NOT need this: its predicate state is written under
    // the same `queue_mutex_` its waiters hold.
    {
      const std::scoped_lock<std::mutex> slock(stats_mutex_);
    }
    stats_cv_.notify_all();
    cpu_monitor_.notify_stop();
    memory_monitor_.notify_stop();

    const auto t_phase1 = Clock::now();

    // Phase 2: join with the lock released so workers can drain.
    // Workers in `cv_.wait` see `!running_` via the predicate; workers
    // in the gate-failure `cv_.wait_for` see it on the next iteration
    // top after the timeout (≤ scheduler_interval_).
    for (auto& w : workers_) {
      if (w.joinable()) {
        w.join();
      }
    }
    const auto t_phase2 = Clock::now();

    // Phase 3: now that no worker can run, it is safe to clear the
    // vector.  No `submit()` can spawn into it because Phase 1 set
    // `running_ = false` under the same lock that `submit()` takes.
    {
      const std::scoped_lock<std::mutex> lock(queue_mutex_);
      workers_.clear();
    }

    if (stats_thread_.joinable()) {
      // request_stop + notify already sent in Phase 1 — just join.
      stats_thread_.join();
    }

    tasks_active_.store(0, std::memory_order_relaxed);

    cpu_monitor_.stop();
    memory_monitor_.stop();
    const auto t_helpers = Clock::now();

    // Trace the per-phase timing so a `tail -f` of the trace log
    // makes the eager-wake win visible (and any future regression
    // localisable to a specific phase).  Cost is one trace line at
    // shutdown — emitted on the cold pool-teardown path, never in
    // the inner loop.
    spdlog::trace(
        "[{}] shutdown timing: phase1={:.1f}ms (stop+notify), "
        "phase2={:.1f}ms (worker join), helpers={:.1f}ms "
        "(stats+cpu+mem stop)",
        name_,
        std::chrono::duration<double, std::milli>(t_phase1 - t_shutdown_begin)
            .count(),
        std::chrono::duration<double, std::milli>(t_phase2 - t_phase1).count(),
        std::chrono::duration<double, std::milli>(t_helpers - t_phase2)
            .count());

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
  /// thread — `max_threads_` may grow at runtime up to the ceiling.
  [[nodiscard]] int max_threads() const noexcept
  {
    return max_threads_.load(std::memory_order_relaxed);
  }

  /// Absolute growth ceiling for `max_threads()` (the un-clamped CPU
  /// budget).  Equals `max_threads()` initially when growth is disabled.
  [[nodiscard]] int max_threads_ceiling() const noexcept
  {
    return max_threads_ceiling_;
  }

  /// Process-RSS budget driving the marginal-RSS dispatch gate (0 = the
  /// gate + measured model are disabled).  Exposed for tests / diagnostics.
  [[nodiscard]] double max_process_rss_mb() const noexcept
  {
    return max_process_rss_mb_;
  }

  /// System-wide memory-usage backstop threshold (percent).  Routine
  /// throttling is the marginal-RSS gate; this fires only near OOM.
  [[nodiscard]] double max_memory_percent() const noexcept
  {
    return max_memory_percent_;
  }

  // ── Slot-release guard for in-task blocking ─────────────────────────────
  //
  // Background.  `worker_loop` increments `tasks_active_` and
  // `active_threads_` when a worker pops a task, decrements both after
  // `task.execute()` returns.  Between those two events the task is
  // counted as "consuming a worker slot" by `can_dispatch_top()` even
  // if the user code inside `task.execute()` is parked in a kernel
  // futex waiting for an external event the pool can't dispatch (a
  // future from this same pool, an inter-thread condvar, a network
  // call, …).  When that wait correlates with the gate being closed
  // (memory %, CPU %, …), the pool can wedge: every worker is
  // "active" but none is actually running, no dispatch happens, no
  // `tasks_active_` decrement, no recovery.
  //
  // `release_slot_while_blocking()` returns an RAII guard that
  // temporarily yields the calling task's slot for the duration of an
  // external blocking call, so the pool's gate sees one fewer active
  // task and can dispatch other queued work.  The slot is reacquired
  // when the guard goes out of scope; transient over-saturation is
  // bounded by the number of guards live simultaneously, which is
  // capped by the actual worker count.
  //
  // Usage:
  //
  //     auto fut = some_other_pool.submit(...);
  //     {
  //       auto guard = this_pool.release_slot_while_blocking();
  //       fut.get();   // pool sees one fewer active slot during this wait
  //     }   // slot reacquired here, work continues
  //
  // No-op if called outside one of THIS pool's workers: the guard
  // checks the thread-local worker tag (`this_thread_worker_pool`),
  // so a coordinator thread or a foreign pool's worker can never
  // release a slot that belongs to another running task (which would
  // over-admit by one per guard).  It additionally checks
  // `tasks_active_ > 0` so the count never goes below zero.
  class SlotReleaseGuard
  {
  public:
    explicit SlotReleaseGuard(BasicWorkPool* pool) noexcept
        : m_pool_(pool)
    {
      if (m_pool_ != nullptr) {
        m_released_ = m_pool_->release_slot_for_blocking_();
      }
    }
    ~SlotReleaseGuard() noexcept
    {
      if (m_pool_ != nullptr && m_released_) {
        m_pool_->reacquire_slot_after_blocking_();
      }
    }
    SlotReleaseGuard(const SlotReleaseGuard&) = delete;
    SlotReleaseGuard& operator=(const SlotReleaseGuard&) = delete;
    SlotReleaseGuard(SlotReleaseGuard&& other) noexcept
        : m_pool_(std::exchange(other.m_pool_, nullptr))
        , m_released_(std::exchange(other.m_released_, false))
    {
    }
    SlotReleaseGuard& operator=(SlotReleaseGuard&& other) noexcept
    {
      if (this != &other) {
        if (m_pool_ != nullptr && m_released_) {
          m_pool_->reacquire_slot_after_blocking_();
        }
        m_pool_ = std::exchange(other.m_pool_, nullptr);
        m_released_ = std::exchange(other.m_released_, false);
      }
      return *this;
    }

  private:
    BasicWorkPool* m_pool_;
    bool m_released_ {false};
  };

  /// Acquire a guard that releases this task's worker slot for the
  /// duration of an external blocking wait.  See SlotReleaseGuard for
  /// rationale and usage.
  [[nodiscard]] SlotReleaseGuard release_slot_while_blocking() noexcept
  {
    return SlotReleaseGuard {this};
  }

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
          max_threads_.load(std::memory_order_relaxed),
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
  // ── Slot-release helpers (used by SlotReleaseGuard) ────────────────────
  //
  // These are intentionally narrow: decrement / increment under
  // `queue_mutex_` so dispatch decisions see the fresh count via
  // mutex-acquire happens-before, and notify `cv_` on release so any
  // worker parked on the memory gate wakes up to re-evaluate.  No
  // back-pressure on reacquire: by the time a guard is destroyed the
  // task is about to run again, and over-saturation by 1 per guard is
  // bounded by the worker count.
  /// Returns true iff the slot was actually released (caller is inside
  /// one of THIS pool's workers), so the matching reacquire on guard
  /// destruction knows whether to increment back.  False when called
  /// from any other thread — a coordinator driver, a foreign pool's
  /// worker, or a plain caller.  The thread-local check is what makes
  /// this exact: the previous `tasks_active_ > 0` heuristic released a
  /// slot belonging to some OTHER running task whenever a non-worker
  /// called it while work was in flight, over-admitting by one per
  /// guard.
  bool release_slot_for_blocking_() noexcept
  {
    if (this_thread_worker_pool != this) {
      return false;  // not one of this pool's workers — nothing to release
    }
    bool released = false;
    {
      const std::scoped_lock<std::mutex> qlock(queue_mutex_);
      if (tasks_active_.load(std::memory_order_relaxed) > 0) {
        tasks_active_.fetch_sub(1, std::memory_order_relaxed);
        active_threads_.fetch_sub(1, std::memory_order_relaxed);
        released = true;
      }
    }
    if (released) {
      cv_.notify_all();
    }
    return released;
  }

  void reacquire_slot_after_blocking_() noexcept
  {
    const std::scoped_lock<std::mutex> qlock(queue_mutex_);
    tasks_active_.fetch_add(1, std::memory_order_relaxed);
    active_threads_.fetch_add(1, std::memory_order_relaxed);
  }

  void maybe_spawn_worker_unlocked()
  {
    if (!running_.load(std::memory_order_relaxed)) {
      return;  // pool is shutting down — do not create new workers
    }
    const auto total = workers_.size();
    if (std::cmp_greater_equal(total,
                               max_threads_.load(std::memory_order_relaxed)))
    {
      return;
    }
    const auto active = active_threads_.load(std::memory_order_relaxed);
    const auto idle =
        std::cmp_less(active, total) ? static_cast<int>(total) - active : 0;
    const auto pending = static_cast<int>(task_queue_.size());
    if (pending <= idle) {
      // Existing idle workers can absorb the queue — `notify_all()`
      // from the caller will wake them.
      return;
    }
    workers_.emplace_back(
        [this, i = total](const std::stop_token& stoken)
        {
#ifdef __linux__
          const auto thread_name = std::format("WP-{}-{}", name_, i);
          pthread_setname_np(pthread_self(), thread_name.substr(0, 15).c_str());
#endif
          worker_loop(stoken);
        });
  }

  /// Refresh the measured memory model from a live `/proc` RSS reading.
  /// No locks required (all state is atomic); cheap enough to call on every
  /// dispatch attempt and task completion.
  ///
  ///   * When idle (`active == 0`) the current RSS IS the persistent floor
  ///     (heap + resident snapshots + registries + cuts) — capture it.
  ///   * When busy, the marginal cost of one task is `(rss - floor) / active`.
  ///     We keep `meas_per_task_mb_` as a slowly-decaying high-water mark:
  ///     it jumps up to any new observed marginal immediately (conservative —
  ///     never underestimates a uniform task's peak) and decays gently so a
  ///     transient spike does not throttle the pool forever.
  void update_memory_model_(int active_now) const
  {
    if (max_process_rss_mb_ <= 0.0) {
      return;  // no limit → model unused
    }
    const double rss = memory_monitor_.get_process_rss_mb();
    if (rss <= 0.0) {
      return;
    }
    if (active_now <= 0) {
      idle_floor_mb_.store(rss, std::memory_order_relaxed);
      return;
    }
    const double floor = idle_floor_mb_.load(std::memory_order_relaxed);
    const double sample = (rss - floor) / static_cast<double>(active_now);
    if (!(sample > 0.0)) {
      return;  // floor not yet captured or noise — keep prior estimate
    }
    const double prev = meas_per_task_mb_.load(std::memory_order_relaxed);
    // Jump up instantly, decay ~2% per refresh otherwise.
    const double next = std::max(sample, prev * 0.98);
    meas_per_task_mb_.store(next, std::memory_order_relaxed);
  }

  /// Grow `max_threads_` toward `max_threads_ceiling_` by at most one per
  /// memory-monitor interval, sized to the MEASURED memory headroom.  Caller
  /// holds `queue_mutex_`.
  ///
  /// Concurrency tracks "how many uniform tasks fit": feasible =
  /// `(limit - idle_floor) / measured_per_task`.  Growth is paced (one step
  /// per monitor interval) so each new worker's real RSS impact is observed
  /// before the next step — the same rate limit the dispatch gate uses, so
  /// `max_threads_` and the gate stay consistent and never burst.  Grow-only:
  /// already-spawned jthreads are not reclaimed; the gate handles tightening.
  void maybe_grow_max_threads_unlocked()
  {
    const int ceiling = max_threads_ceiling_;
    const int current = max_threads_.load(std::memory_order_relaxed);
    if (ceiling <= current) {
      return;  // already at ceiling
    }

    // No memory limit → memory is not the constraint; go straight to ceiling.
    if (max_process_rss_mb_ <= 0.0) {
      max_threads_.store(ceiling, std::memory_order_relaxed);
      maybe_spawn_worker_unlocked();
      return;
    }

    // Pace growth to the monitor cadence (reuse the admission rate limiter):
    // one capacity step per interval lets each added worker's footprint
    // register in RSS before we add more.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_admit_time_.load(std::memory_order_relaxed)
        < memory_monitor_.get_interval())
    {
      return;
    }

    const auto active_now =
        std::max(0, active_threads_.load(std::memory_order_relaxed));
    update_memory_model_(active_now);
    const double per_task = meas_per_task_mb_.load(std::memory_order_relaxed);
    const double floor = idle_floor_mb_.load(std::memory_order_relaxed);
    const double rss = memory_monitor_.get_process_rss_mb();

    // No measurement yet (no task has run long enough to move RSS): allow a
    // single probe step so one task can run and reveal its cost.
    int feasible = ceiling;
    constexpr double kPerTaskEpsilonMB = 1.0;
    if (per_task > kPerTaskEpsilonMB) {
      const double headroom = max_process_rss_mb_ - std::max(floor, rss - 0.0);
      const int fits = headroom > 0.0
          ? static_cast<int>(std::floor(headroom / per_task))
          : 0;
      feasible = active_now + std::max(0, fits);
    } else if (rss >= max_process_rss_mb_) {
      feasible = current;  // already at/over limit with no usable estimate
    }

    // Grow by at most one step this interval.
    const int target = std::clamp(feasible, current, ceiling);
    const int new_max = std::min(target, current + 1);
    if (new_max > current) {
      max_threads_.store(new_max, std::memory_order_relaxed);
      SPDLOG_DEBUG(
          "{}: grew max_threads {} → {} (measured per-task {:.0f} MB, floor "
          "{:.0f} MB, rss {:.0f} MB, feasible {}, ceiling {})",
          name_,
          current,
          new_max,
          per_task,
          floor,
          rss,
          feasible,
          ceiling);
      maybe_spawn_worker_unlocked();
    }
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
    // Tag this thread as a worker of THIS pool for the lifetime of the
    // thread (workers never outlive their pool).  Consumed by
    // `release_slot_for_blocking_` to make SlotReleaseGuard exact.
    this_thread_worker_pool = this;
    while (!stoken.stop_requested()) {
      // Defense-in-depth: a top-level catch around each loop iteration
      // so that an exception thrown OUTSIDE the inner `task.execute()`
      // try (e.g. from `cpu_monitor_.get_load()`, `pthread_setname_np`
      // setup, `Task` move-construction, scoped_lock contention, or
      // `std::bad_alloc` from spdlog formatting under memory pressure)
      // does not silently kill the worker thread.  Without this guard
      // a dead worker leaves an entry in `workers_` that
      // `maybe_spawn_worker_unlocked` counts but never replaces, and
      // the pool wedges with `Pending: N, Active: 0`.  Not strictly
      // required for the observed juan/IPLP stall — no exception was
      // logged there — but cheap insurance for future exception modes.
      try {
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

        // Bounded head-of-line peek: pick the highest-priority task in
        // the first PEEK_DEPTH entries whose gates pass.  When the head
        // is throttled but a runnable lower-priority task sits a few
        // positions below, this picks the lower one instead of parking
        // the whole pool.  See `find_dispatchable_index()` for the
        // failure mode this prevents.
        auto idx_opt = find_dispatchable_index();
        if (!idx_opt) {
          // Every candidate in the peek window is gated.  Wait on `cv_`
          // with a timeout AND a predicate that includes shutdown so
          // `shutdown()` can break us out immediately rather than
          // waiting up to scheduler_interval_ for the timeout.  Plain
          // `sleep_for` (or a `wait_for` without predicate) would also
          // miss submit-side notifies during the back-off and starve
          // the queue under sustained back-pressure.
          cv_.wait_for(lock,
                       scheduler_interval_,
                       [&]
                       { return stoken.stop_requested() || !running_.load(); });
          continue;
        }
        const auto idx = *idx_opt;

        Task<void, Key, KeyCompare> task;
        if (idx == 0) {
          // Common-case fast path: head dispatches, standard max-heap pop.
          std::ranges::pop_heap(task_queue_, std::less<> {});
          task = std::move(task_queue_.back());
          task_queue_.pop_back();
        } else {
          // Rare unhappy path: head is gated but a lower-priority task at
          // index `idx` (1..PEEK_DEPTH-1) passes the gates.  Extract it
          // and rebuild the heap — `make_heap` is O(N) but only runs on
          // the rare gate-mismatch ticks, which by construction are
          // dwarfed by the throughput we recover by not parking the pool.
          task = std::move(task_queue_[idx]);
          task_queue_.erase(task_queue_.begin()
                            + static_cast<std::ptrdiff_t>(idx));
          std::ranges::make_heap(task_queue_, std::less<> {});
        }

        const auto threads_needed = task.requirements().estimated_threads;
        tasks_pending_.fetch_sub(1, std::memory_order_relaxed);
        active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);
        tasks_active_.fetch_add(1, std::memory_order_relaxed);

        // GPU token: consume for GPU-class tasks, held (RAII) across
        // execute() and released at end of this iteration.  An empty
        // ticket means a cross-pool race took the last token between the
        // dispatch gate and this acquire — execute anyway: the backend's
        // process-global serialization mutex still guarantees correctness
        // (behaviour never worse than pre-governor).
        ResourceGovernor::Ticket resource_ticket;
        if (task.requirements().resource_class == ResourceClass::gpu) {
          resource_ticket =
              ResourceGovernor::instance().try_admit(ResourceClass::gpu);
        }
        // Stamp the admission time for the measured-memory rate limiter:
        // the dispatch gate and capacity growth both refuse to admit/grow
        // again until one memory-monitor interval has elapsed, so each
        // admission's real RSS impact is observed before the next.
        last_admit_time_.store(std::chrono::steady_clock::now(),
                               std::memory_order_relaxed);

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

        // Now that this task's RSS delta has updated the EMA, see whether
        // the conservative initial clamp can be relaxed toward the CPU
        // ceiling — and spawn workers to fill the new capacity.  Held under
        // `queue_mutex_` (consistent with every other `max_threads_` write
        // and `maybe_spawn_worker_unlocked` call).
        {
          const std::scoped_lock<std::mutex> qlock(queue_mutex_);
          maybe_grow_max_threads_unlocked();
        }

        // Return the GPU token (if held) BEFORE waking waiters so a
        // throttled GPU task can be admitted on the very next gate check
        // instead of one scheduler tick later.
        resource_ticket = ResourceGovernor::Ticket {};

        // Wake any worker that was throttled by `current + threads_needed
        // > max_threads_`: one of those checks may now pass against the
        // freshly-decremented `active_threads_`.
        cv_.notify_all();
      } catch (const std::exception& e) {
        SPDLOG_ERROR("[{}] worker_loop iteration crashed: {} (continuing)",
                     name_,
                     e.what());
      } catch (...) {
        SPDLOG_ERROR(
            "[{}] worker_loop iteration crashed (unknown) (continuing)", name_);
      }
    }
  }

  /// Returns true iff @p next_task may be dispatched right now given
  /// current concurrency, CPU and memory pressure.  Caller must hold
  /// `queue_mutex_`.  Increments throttle counters on each failing
  /// gate so operators can see which gate held work back.
  ///
  /// This is the gate-test factored out from the legacy
  /// `can_dispatch_top()` so the bounded peek in
  /// `find_dispatchable_index()` can test multiple candidates without
  /// duplicating the gate logic.
  bool can_dispatch_task(const Task<void, Key, KeyCompare>& next_task) const
  {
    const auto threads_needed = next_task.requirements().estimated_threads;
    const auto current_threads =
        active_threads_.load(std::memory_order_relaxed);
    const auto max_threads = max_threads_.load(std::memory_order_relaxed);

    if (current_threads + threads_needed > max_threads) {
      return false;
    }

    const auto is_critical =
        next_task.requirements().priority == TaskPriority::Critical;

    // Refresh the measured memory model (captures the idle floor when
    // nothing is running) before any gate decision.
    const auto active_now = active_threads_.load(std::memory_order_relaxed);
    update_memory_model_(active_now);

    // ── Universal progress guarantee ───────────────────────────────────
    // When nothing is running (`active_now == 0`), ADMIT one task
    // regardless of CPU / memory% / free-memory / RSS / swap pressure.
    // Serial execution of a single task is the minimum-resource way to
    // make progress; refusing it on ANY soft resource gate would wedge the
    // pool forever (no running task can relieve the pressure to reopen the
    // gate).  This is the livelock that hung the box on the 2-year
    // simulation/write pass: 18 tasks pending, 0 active, system mem 92% ≥
    // 90% `max_memory_percent` blocking every one, "no progress for 88
    // intervals".  The thread-count gate above already passed (current==0),
    // so admitting here is always safe.
    if (active_now == 0) {
      return true;
    }

    // ── GPU admission gate ─────────────────────────────────────────────
    // GPU-class tasks are additionally gated on the process-global GPU
    // token bucket (one token per device by default: Default compute mode
    // time-slices contexts, so concurrent GPU solves add no throughput).
    // Soft gate like CPU/memory: the universal progress guarantee above
    // still admits when idle, and the post-pop try_admit tolerates
    // cross-pool races (an empty ticket there means "execute anyway" —
    // the backend's own serialization mutex carries correctness).
    if (next_task.requirements().resource_class == ResourceClass::gpu
        && !ResourceGovernor::instance().gpu_available())
    {
      throttled_gpu_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

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
    if (max_threads >= 4
        && current_threads + threads_needed
            >= static_cast<int>(max_threads * 0.8))
    {
      const auto cpu_load = cpu_monitor_.get_physical_load();
      // CPU-gate relaxation is an ADMISSION concern, independent of queue
      // ordering.  `Critical` keeps its fixed 95 % ceiling; any task with
      // `gate_bypass` set (regardless of `TaskPriority`) gets the same
      // `+5 %` headroom `High` used to grant — this is what lets the SDDP
      // sim task bypass the saturation wedge without reordering ahead of
      // still-training peers.
      auto cpu_threshold = max_cpu_threshold_;
      if (is_critical) {
        cpu_threshold = 95.0;
      } else if (next_task.requirements().gate_bypass) {
        cpu_threshold = max_cpu_threshold_ + 5.0;
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

    // ── Measured-memory dispatch gate (no fixed per-task estimates) ──────
    // Reached only when `active_now > 0` (the idle case admitted above).
    // The measured model was already refreshed at the top of this function.
    if (max_process_rss_mb_ > 0.0) {
      const auto rss = memory_monitor_.get_process_rss_mb();
      const auto rss_threshold =
          is_critical ? max_process_rss_mb_ * 1.1 : max_process_rss_mb_;

      const double per_task = meas_per_task_mb_.load(std::memory_order_relaxed);
      const double projected = rss + per_task;

      // Rate limit: admit at most one task per memory-monitor interval so
      // each admission's real RSS impact is observed (the monitor refreshes
      // RSS on that cadence) before we decide again.  This is the
      // structural cure for the "N tasks admitted before RSS reflects any
      // of them" burst — without it, right after an admit the marginal
      // estimate is still ~0 and the gate would wave the whole queue
      // through.  Critical tasks bypass the pacing.
      //
      // BUT the pacing only matters NEAR the budget: a burst admitted with
      // ample headroom cannot overshoot, so pacing every admission far from
      // the budget needlessly serializes pools with many small tasks to
      // ~1-per-monitor-interval even with tens of GB free (observed: the
      // SDDP backward pass crawling at Active:1/96 with RSS 34 GB ≪ 76 GB
      // budget once the gate was enabled).  Gate the pacing on a marginal
      // zone within ~15% of the budget; below it, skip pacing and admit at
      // full concurrency (the hard projection cap below still applies, so a
      // burst can never project past the budget).
      const double pacing_floor = rss_threshold * 0.85;
      if (!is_critical && rss >= pacing_floor) {
        const auto now = std::chrono::steady_clock::now();
        const auto since_admit =
            now - last_admit_time_.load(std::memory_order_relaxed);
        if (since_admit < memory_monitor_.get_interval()) {
          throttled_process_rss_.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
      }

      // Project adding ONE more task at the MEASURED marginal cost.  `rss`
      // already reflects the tasks in flight (their allocation is resident),
      // so we add a single task's measured footprint — peak then settles at
      // ≈ rss + per_task ≤ limit, i.e. overshoot bounded by one task.  This
      // hard cap applies at ALL rss levels (not just the marginal zone).
      if (projected >= rss_threshold) {
        throttled_process_rss_.fetch_add(1, std::memory_order_relaxed);
        SPDLOG_DEBUG(
            "{}: blocked by measured RSS {:.0f}+{:.0f}={:.0f} MB >= {:.0f} MB "
            "(floor {:.0f} MB, {} active)",
            name_,
            rss,
            per_task,
            projected,
            rss_threshold,
            idle_floor_mb_.load(std::memory_order_relaxed),
            active_now);
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
    if (max_swap_io_per_sec_ > 0.0 && max_threads >= 4
        && current_threads + threads_needed
            >= static_cast<int>(max_threads * 0.8))
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

  /// Bounded head-of-line scan: find the highest-priority task in
  /// `task_queue_` (at index 0 .. PEEK_DEPTH-1) whose gates pass.
  ///
  /// Caller must hold `queue_mutex_`.  Returns the heap index of the
  /// first dispatchable task, or `std::nullopt` if every candidate in
  /// the peek window is gated.
  ///
  /// Why peek beyond the heap head?  The legacy `can_dispatch_top()`
  /// checked only `task_queue_.front()` against the dispatch gates.
  /// When the head task was throttled (e.g. CPU gate trips on a
  /// `Medium`-priority master backward task), every worker parked on
  /// `cv_.wait_for(scheduler_interval_)` even though lower-priority
  /// tasks below in the heap could run.  This is a head-of-line
  /// throttle deadlock: a single gated high-priority task at the head
  /// can starve the entire pool for as long as the gate stays closed,
  /// which on a busy host can be indefinite.
  ///
  /// The fix: scan up to `PEEK_DEPTH` (= 8) elements from the front of
  /// the heap.  Eight is empirically deep enough to find a runnable
  /// task on every juan/IPLP wedge witnessed to date while keeping
  /// the per-tick scan O(1) (gates are O(1) calls).  If none of those
  /// K is dispatchable the worker parks, exactly as before.
  ///
  /// See the 2026-05-16 concurrency audit for the failure mode this
  /// prevents (juan/IPLP scene-12 wedge: master backward task at
  /// priority (28,1,lp) gate-throttled, sim tasks at priority
  /// (29,0,lp) underneath it that could run but never dispatched).
  static constexpr std::size_t PEEK_DEPTH = 8;

  [[nodiscard]] std::optional<std::size_t> find_dispatchable_index() const
  {
    if (task_queue_.empty()) {
      return std::nullopt;
    }
    const auto limit = std::min<std::size_t>(task_queue_.size(), PEEK_DEPTH);
    for (std::size_t k = 0; k < limit; ++k) {
      if (can_dispatch_task(task_queue_[k])) {
        return k;
      }
    }
    return std::nullopt;
  }

  /// Legacy single-shot gate test.  Kept for callers (and unit tests)
  /// that only ask "is anything in the queue dispatchable right now?"
  /// — semantically `find_dispatchable_index().has_value()`.
  [[nodiscard]] bool can_dispatch_top() const
  {
    return find_dispatchable_index().has_value();
  }

  /// Periodic stats printer + stall-recovery nudge.  Body is
  /// defined out-of-line in source/work_pool.cpp so the noisy
  /// logging / format-string code can be edited without triggering
  /// a full re-compile of every translation unit that includes this
  /// header (the `BasicWorkPool` template is explicitly instantiated
  /// in work_pool.cpp for `int64_t`, `std::string`, and
  /// `SDDPTaskKey` — the only three key types in use).
  ///
  /// Non-const: may call `maybe_spawn_worker_unlocked()` on stall
  /// detection to recover from the workpool wedge.  Other state
  /// mutation is via `mutable` members (`cv_`, `queue_mutex_`,
  /// stats counters).
  void log_periodic_stats();

  /// Final stats printer at pool shutdown.  Body is out-of-line in
  /// source/work_pool.cpp for the same reason as `log_periodic_stats`.
  void log_final_stats() const;
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
