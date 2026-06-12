// SPDX-License-Identifier: BSD-3-Clause

// Standard library
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Third-party
#include <spdlog/spdlog.h>

// Project headers
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/sddp_pool.hpp>  // SDDPTaskKey — for explicit instantiation
#include <gtopt/work_pool.hpp>

namespace gtopt
{

// ─── BasicWorkPool::log_periodic_stats (out-of-line) ─────────────────────────
//
// Moved out of work_pool.hpp so the logging / format-string code can
// be edited without recompiling every translation unit that includes
// the header.  Three explicit instantiations at the bottom of this
// file cover every concrete pool in use (`int64_t`, `std::string`,
// `SDDPTaskKey`).
template<typename Key, typename KeyCompare>
void BasicWorkPool<Key, KeyCompare>::log_periodic_stats()
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
        max_threads_.load(std::memory_order_relaxed),
        stats.tasks_pending,
        stats.tasks_completed);

    // Stall detection: when `tasks_completed` has not advanced since
    // the previous periodic log AND there is *pending* work that
    // could dispatch but isn't, surface the gate that is blocking
    // progress.  Mirrors the checks in `can_dispatch_next()` so the
    // user sees the same condition the scheduler sees.
    //
    // **Why the criterion requires ``pending > 0``** (not ``active >
    // 0``).  Tasks in this pool are coarse-grained — e.g. a per-scene
    // SDDP simulation pass that walks 51 phases × 14 s each ≈ 12 min
    // per scene.  With pool granularity at the *scene* level,
    // ``tasks_completed`` only advances every 12 min even when each
    // scene is making steady internal progress (visible in trace logs
    // as ``SystemLP::write_out [scene=N phase=M]`` lines).  The old
    // criterion ``has_work = pending > 0 || active > 0`` fired the
    // warning every 30 s × 2 intervals during that healthy 12 min
    // window — false positives on juan/IPLP single-bus runs.
    //
    // Restricting the stall signal to ``pending > 0`` only flags the
    // case where work is *queued* but the scheduler isn't picking it
    // up — i.e. a true dispatch deadlock or a throttle gate that
    // should be visible in the diagnostic ``reason`` block below.
    // Trade-off: the rare case where every active task simultaneously
    // wedges in a futex with nothing pending will no longer trigger
    // the warning.  That scenario is covered by gdb thread dumps when
    // it surfaces; the more common false positive on long
    // monolithic tasks is what users actually see.
    const bool dispatch_blocked = stats.tasks_pending > 0;
    const bool no_progress = stats.tasks_completed == last_logged_completed_;
    if (dispatch_blocked && no_progress) {
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
      } else if (stats.active_threads + 1 >= static_cast<int>(
                     max_threads_.load(std::memory_order_relaxed) * 0.8)
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

      // Deep instrumentation for memory-limit stall debugging.  Logs
      // a per-gate snapshot at stall detection so future repros can
      // be diagnosed without re-running with trace-level.  Each gate
      // value is paired with its threshold so the operator sees
      // exactly which one is blocking (or whether none are, which
      // means workers exited).  Throttle counters since pool start
      // also surface here.
      const auto throttled_cpu = throttled_cpu_.load();
      const auto throttled_mem_pct = throttled_memory_pct_.load();
      const auto throttled_free_mem = throttled_free_memory_.load();
      const auto throttled_rss = throttled_process_rss_.load();
      const auto throttled_swap = throttled_process_swap_.load();
      const auto throttled_swap_io = throttled_swap_io_.load();
      std::size_t workers_alive = 0;
      {
        const std::scoped_lock<std::mutex> qlock(queue_mutex_);
        workers_alive = workers_.size();
      }
      spdlog::warn(
          "[{}] stall diag — workers_alive={} max_threads={} "
          "active_threads={} mem_pct={:.1f}%/{}% free_mb={:.0f}/{:.0f} "
          "rss_mb={:.0f}/{:.0f} swap_mb={:.0f}/{:.0f} swap_io={:.0f}/{:.0f} "
          "cpu_load={:.1f}%/{:.1f}% "
          "throttle_counters[cpu={} mem%={} free={} rss={} swap={} sw_io={}]",
          name_,
          workers_alive,
          max_threads_.load(std::memory_order_relaxed),
          stats.active_threads,
          stats.current_memory_percent,
          max_memory_percent_,
          stats.available_memory_mb,
          min_free_memory_mb_,
          stats.process_rss_mb,
          max_process_rss_mb_,
          stats.process_swap_mb,
          max_process_swap_mb_,
          stats.swap_io_rate,
          max_swap_io_per_sec_,
          stats.current_cpu_load,
          max_cpu_threshold_,
          throttled_cpu,
          throttled_mem_pct,
          throttled_free_mem,
          throttled_rss,
          throttled_swap,
          throttled_swap_io);

      // Recovery nudge — wake any workers parked on the gate
      // back-off (cv_.wait_for at scheduler_interval_ in worker_loop)
      // and replace workers that may have exited.  Required because
      // workers parked on the back-off wait_for only wake on
      // `stop_requested` / `!running_`; they don't re-check memory
      // gates without a notify.  Once memory drops (the stats above
      // confirm it has — e.g. juan/IPLP at 91% free), this nudge
      // re-engages the pool.  Symmetric to the implicit nudge
      // on each task completion at worker_loop's `cv_.notify_all()`,
      // but fires from the monitor thread when no task has completed
      // in the stall window.
      std::size_t workers_before = 0;
      std::size_t workers_after = 0;
      {
        const std::scoped_lock<std::mutex> qlock(queue_mutex_);
        workers_before = workers_.size();
        maybe_spawn_worker_unlocked();
        workers_after = workers_.size();
      }
      cv_.notify_all();
      spdlog::warn(
          "[{}] stall recovery nudge fired — workers {}→{} (spawned={}), "
          "cv_.notify_all() called",
          name_,
          workers_before,
          workers_after,
          workers_after - workers_before);
    }
  } catch (const std::exception& e) {
    SPDLOG_WARN("log_periodic_stats failed: {}", e.what());
  }
}

// ─── BasicWorkPool::log_final_stats (out-of-line) ────────────────────────────
template<typename Key, typename KeyCompare>
void BasicWorkPool<Key, KeyCompare>::log_final_stats() const
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
    const auto total_throttle = stats.throttled_cpu + stats.throttled_memory_pct
        + stats.throttled_free_memory + stats.throttled_process_rss
        + stats.throttled_process_swap + stats.throttled_swap_io;
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

// ─── Explicit instantiations ─────────────────────────────────────────────────
//
// These three are the only concrete `BasicWorkPool` types in the
// codebase.  Adding a new key type means adding a new explicit
// instantiation here (otherwise the out-of-line members above will
// fail to link).
template void
BasicWorkPool<std::int64_t, std::less<std::int64_t>>::log_periodic_stats();
template void
BasicWorkPool<std::int64_t, std::less<std::int64_t>>::log_final_stats() const;

template void
BasicWorkPool<std::string, std::less<std::string>>::log_periodic_stats();
template void
BasicWorkPool<std::string, std::less<std::string>>::log_final_stats() const;

template void
BasicWorkPool<SDDPTaskKey, std::less<SDDPTaskKey>>::log_periodic_stats();
template void
BasicWorkPool<SDDPTaskKey, std::less<SDDPTaskKey>>::log_final_stats() const;

}  // namespace gtopt
