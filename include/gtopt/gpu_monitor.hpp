/**
 * @file      gpu_monitor.hpp
 * @brief     GPU (NVML) resource monitoring — sibling of
 * CPUMonitor/MemoryMonitor
 * @date      Mon Jul 06 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * GPU-side member of the work-pool monitor family.  Mirrors CPUMonitor /
 * MemoryMonitor: a background jthread on the same cadence with cheap cached
 * getters.  The concrete monitor loads NVML at runtime via
 * ``dlopen("libnvidia-ml.so.1")`` — NEVER a link-time dependency — so a
 * GPU-less host or build behaves exactly as before (``has_gpu() == false``,
 * ``device_count() == 0``, no admission tokens minted).
 *
 * The abstract @ref GpuMonitorBase lets tests inject a scripted fake with no
 * NVML and no device (see test_gpu_monitor / test_resource_governor).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace gtopt
{

/// Immutable snapshot of one GPU device's live state (NVML units normalised
/// to MiB / percent).
struct GpuDeviceState
{
  int index {0};  ///< CUDA/NVML ordinal
  double vram_total_mib {0.0};
  double vram_free_mib {0.0};
  double util_pct {0.0};  ///< nvmlDeviceGetUtilizationRates.gpu
  double proc_vram_mib {
      0.0};  ///< this PID's usedGpuMemory (NVML), 0 if unknown
};

/// Query surface shared by the NVML-backed @ref GpuMonitor and test fakes.
/// Kept virtual (not hot — consulted only at task admission) so a
/// FakeGpuMonitor can drive @ref ResourceGovernor without a real GPU.
class GpuMonitorBase
{
public:
  GpuMonitorBase() = default;
  GpuMonitorBase(const GpuMonitorBase&) = delete;
  GpuMonitorBase& operator=(const GpuMonitorBase&) = delete;
  GpuMonitorBase(GpuMonitorBase&&) = delete;
  GpuMonitorBase& operator=(GpuMonitorBase&&) = delete;
  virtual ~GpuMonitorBase() = default;

  /// True only when a GPU backend is usable: NVML loaded AND >=1 device.
  [[nodiscard]] virtual bool has_gpu() const noexcept = 0;

  /// Number of visible CUDA devices (0 when !has_gpu()).
  [[nodiscard]] virtual int device_count() const noexcept = 0;

  /// Live state of device @p index, or nullopt if out of range / no NVML.
  [[nodiscard]] virtual std::optional<GpuDeviceState> device(
      int index) const noexcept = 0;

  /// Ordinal of the device with the most free VRAM (multi-GPU assignment),
  /// or nullopt when no device is visible.
  [[nodiscard]] virtual std::optional<int> least_loaded_device()
      const noexcept = 0;
};

/// NVML-backed GPU monitor.  Loads ``libnvidia-ml.so.1`` lazily; if absent it
/// degrades to a no-GPU monitor (never throws, never a build dep).
class GpuMonitor : public GpuMonitorBase
{
public:
  GpuMonitor() = default;
  ~GpuMonitor() override { stop(); }
  GpuMonitor(const GpuMonitor&) = delete;
  GpuMonitor& operator=(const GpuMonitor&) = delete;
  GpuMonitor(GpuMonitor&&) = delete;
  GpuMonitor& operator=(GpuMonitor&&) = delete;

  /// Initialise NVML (once) and start the background sampling thread.  Safe
  /// to call when NVML is unavailable — leaves the monitor in the no-GPU
  /// state.  Idempotent.
  void start();
  void stop() noexcept;

  /// One-shot device-count probe WITHOUT starting a monitor thread: dlopen
  /// NVML, count devices, shut NVML down again.  0 when NVML/GPU is absent.
  /// Used by ResourceGovernor's lazy auto-configuration to size the GPU
  /// token buckets exactly once per process.
  [[nodiscard]] static int probe_device_count() noexcept;

  /// Signal the sampling thread to wind down without joining (Phase-1 helper
  /// used by the pool's shutdown, mirroring CPUMonitor::request_stop).
  void request_stop() noexcept
  {
    running_.store(false, std::memory_order_relaxed);
    if (monitor_thread_.joinable()) {
      monitor_thread_.request_stop();
    }
  }
  void notify_stop() noexcept
  {
    const std::scoped_lock lock(stop_mutex_);
    stop_cv_.notify_all();
  }

  void set_interval(std::chrono::milliseconds interval) noexcept
  {
    monitor_interval_ = interval;
  }
  [[nodiscard]] auto get_interval() const noexcept { return monitor_interval_; }

  [[nodiscard]] bool has_gpu() const noexcept override
  {
    return device_count_.load(std::memory_order_relaxed) > 0;
  }
  [[nodiscard]] int device_count() const noexcept override
  {
    return device_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::optional<GpuDeviceState> device(
      int index) const noexcept override;
  [[nodiscard]] std::optional<int> least_loaded_device()
      const noexcept override;

private:
  struct Nvml;  ///< opaque dlopen'd NVML function table (pimpl)

  /// dlopen NVML, init it, resolve device handles.  Returns an initialised
  /// table + device count, or {nullptr, 0} when NVML is unavailable.
  static std::shared_ptr<Nvml> open_nvml(int& out_count);
  void sample_once() noexcept;  ///< one NVML poll of every device

  std::atomic<int> device_count_ {0};
  std::atomic<bool> running_ {false};
  std::chrono::milliseconds monitor_interval_ {500};

  mutable std::mutex states_mutex_;
  std::vector<GpuDeviceState> states_;  ///< guarded by states_mutex_

  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  std::jthread monitor_thread_;

  std::shared_ptr<Nvml> nvml_;
};

}  // namespace gtopt
