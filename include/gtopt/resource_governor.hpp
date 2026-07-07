/**
 * @file      resource_governor.hpp
 * @brief     Process-global GPU admission (per-device token bucket)
 * @date      Mon Jul 06 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The work pool sizes CPU concurrency from cores; the governor adds the *GPU*
 * side.  It holds a per-device token bucket (a counting semaphore) sized from
 * the NVML device count x a per-device concurrency (default 1, because in
 * Default compute mode CUDA contexts time-slice — N concurrent GPU solves get
 * no throughput, only VRAM-OOM risk).  A task's @ref ResourceClass decides
 * whether it needs a GPU token; CPU tasks are a no-op and keep the full
 * cpu_factor x cores fan-out.
 *
 * It is a **process-global singleton** because GPU capacity is a process-wide
 * resource shared across every pool (SDDP sddp/aux/build pools, PlanningLP
 * build pool, ...).  A per-pool governor would let two pools each admit "one"
 * GPU solve = two concurrent = the original oversubscription bug.
 *
 * Admission is **non-blocking** (like the pool's ``can_dispatch_task`` gate):
 * @ref try_admit returns an empty ticket when no GPU token is free, and the
 * caller throttles.  A GPU-less host (``configure(0, ...)`` or never
 * configured) admits every task — behaviour identical to today.
 */

#pragma once

#include <mutex>
#include <optional>
#include <vector>

#include <gtopt/solver_resource.hpp>

namespace gtopt
{

class ResourceGovernor
{
public:
  /// Process-global instance shared across every work pool.
  [[nodiscard]] static ResourceGovernor& instance() noexcept;

  ResourceGovernor(const ResourceGovernor&) = delete;
  ResourceGovernor& operator=(const ResourceGovernor&) = delete;
  ResourceGovernor(ResourceGovernor&&) = delete;
  ResourceGovernor& operator=(ResourceGovernor&&) = delete;
  ~ResourceGovernor() = default;

  /// (Re)size the GPU token buckets: ``device_count`` devices, each with
  /// ``gpu_concurrency_per_device`` tokens (clamped to >= 0 / >= 1).
  /// ``device_count == 0`` disables GPU gating (all tasks admit).  Safe to
  /// call once at startup; any in-flight tickets keep their prior device
  /// index valid (release is index-checked).
  void configure(int device_count, int gpu_concurrency_per_device = 1) noexcept;

  /// RAII admission ticket.  Truthy when the task may run now.  For a GPU
  /// task it also carries the assigned device; its destructor returns the
  /// token.  Move-only.
  class Ticket
  {
  public:
    Ticket() noexcept = default;  ///< empty / not-admitted
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    Ticket(Ticket&& other) noexcept
        : gov_(other.gov_)
        , device_(other.device_)
        , admitted_(other.admitted_)
    {
      other.gov_ = nullptr;
      other.device_ = std::nullopt;
      other.admitted_ = false;
    }
    Ticket& operator=(Ticket&& other) noexcept
    {
      if (this != &other) {
        release();
        gov_ = other.gov_;
        device_ = other.device_;
        admitted_ = other.admitted_;
        other.gov_ = nullptr;
        other.device_ = std::nullopt;
        other.admitted_ = false;
      }
      return *this;
    }
    ~Ticket() { release(); }

    /// True when the task was admitted (CPU always; GPU iff a token was free).
    [[nodiscard]] explicit operator bool() const noexcept { return admitted_; }

    /// Assigned GPU device (nullopt for CPU tasks / not admitted).
    [[nodiscard]] std::optional<int> device() const noexcept { return device_; }

  private:
    friend class ResourceGovernor;
    Ticket(ResourceGovernor* gov,
           std::optional<int> device,
           bool admitted) noexcept
        : gov_(gov)
        , device_(device)
        , admitted_(admitted)
    {
    }
    void release() noexcept;

    ResourceGovernor* gov_ {nullptr};
    std::optional<int> device_ {std::nullopt};
    bool admitted_ {false};
  };

  /// Try to admit one task of class @p cls.  CPU tasks always admit (no
  /// token).  GPU tasks admit iff some device has a free token (P1 will also
  /// project VRAM); the returned ticket carries the least-loaded such device.
  ///
  /// Lazily auto-configures on the first GPU query: probes the NVML device
  /// count once (GpuMonitor::probe_device_count) and mints
  /// `GTOPT_GPU_CONCURRENCY` tokens per device (env, default 1 — Default
  /// compute mode time-slices contexts, so >1 buys nothing without MPS).
  [[nodiscard]] Ticket try_admit(ResourceClass cls) noexcept;

  /// Non-consuming dispatch-gate check: true when a GPU token is free OR
  /// GPU gating is disabled (no devices / never configured).  The pool's
  /// `can_dispatch_task` uses this to throttle GPU-class tasks; the real
  /// acquisition happens post-pop via try_admit (race-tolerant: an empty
  /// ticket there means "execute anyway" — the backend's own serialization
  /// mutex still carries correctness).
  [[nodiscard]] bool gpu_available() noexcept;

  /// Total GPU tokens across all devices (0 when GPU gating is disabled).
  [[nodiscard]] int gpu_capacity() const noexcept;
  /// Currently-held GPU tokens (for tests / diagnostics).
  [[nodiscard]] int gpu_in_use() const noexcept;
  /// Number of configured GPU devices.
  [[nodiscard]] int device_count() const noexcept;

private:
  ResourceGovernor() = default;
  void release_device(int device) noexcept;
  /// First-GPU-query auto-configuration (NVML probe + env knob).  Caller
  /// must hold `mutex_`.
  void ensure_configured_unlocked() noexcept;

  mutable std::mutex mutex_;
  std::vector<int> free_tokens_;  ///< per device; guarded by mutex_
  int per_device_capacity_ {1};
  bool configured_ {false};  ///< set by configure() or the lazy probe
};

}  // namespace gtopt
