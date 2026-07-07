/**
 * @file      gpu_monitor.cpp
 * @brief     NVML-backed GpuMonitor implementation (runtime dlopen)
 * @date      Mon Jul 06 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Loads NVML at runtime from ``libnvidia-ml.so.1`` (the versioned SONAME the
 * driver ships — some installs omit the bare ``libnvidia-ml.so`` symlink) and
 * dlsyms just the handful of symbols needed for device count + per-device VRAM
 * / utilisation.  There is NO link-time NVML dependency and NO CUDA-toolkit
 * header dependency, so a GPU-less host or build compiles and runs unchanged:
 * a failed dlopen leaves ``device_count() == 0`` and every query returns
 * nullopt.  We deliberately mirror the discipline in ``solver_registry.cpp``
 * (dlopen the plugin, dlsym optional symbols, degrade gracefully).
 */

#include <dlfcn.h>
#include <gtopt/gpu_monitor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
constexpr double k_bytes_per_mib = 1024.0 * 1024.0;

// ---- Minimal NVML C ABI (we do not include the NVML headers) --------------
// Opaque device handle; NVML defines it as a pointer.
using nvmlDevice_t = void*;

// NVML's nvmlMemory_t uses `unsigned long long` in its C ABI; match it exactly.
// NOLINTNEXTLINE(google-runtime-int)
struct nvmlMemory_t
{
  unsigned long long total;  // NOLINT(google-runtime-int)
  unsigned long long free;  // NOLINT(google-runtime-int)
  unsigned long long used;  // NOLINT(google-runtime-int)
};

struct nvmlUtilization_t
{
  unsigned int gpu;
  unsigned int memory;
};

constexpr int k_nvml_success = 0;

using nvmlInit_fn = int (*)();
using nvmlShutdown_fn = int (*)();
using nvmlDeviceGetCount_fn = int (*)(unsigned int*);
using nvmlDeviceGetHandleByIndex_fn = int (*)(unsigned int, nvmlDevice_t*);
using nvmlDeviceGetMemoryInfo_fn = int (*)(nvmlDevice_t, nvmlMemory_t*);
using nvmlDeviceGetUtilizationRates_fn = int (*)(nvmlDevice_t,
                                                 nvmlUtilization_t*);
}  // namespace

/// dlopen'd NVML function table + cached per-device handles (pimpl).
struct GpuMonitor::Nvml
{
  Nvml() = default;
  Nvml(const Nvml&) = delete;
  Nvml& operator=(const Nvml&) = delete;
  Nvml(Nvml&&) = delete;
  Nvml& operator=(Nvml&&) = delete;

  void* handle {nullptr};
  nvmlShutdown_fn shutdown {nullptr};
  nvmlDeviceGetMemoryInfo_fn mem_info {nullptr};
  nvmlDeviceGetUtilizationRates_fn util_rates {nullptr};
  std::vector<nvmlDevice_t> devices;

  ~Nvml()
  {
    if (shutdown != nullptr) {
      shutdown();
    }
    if (handle != nullptr) {
      ::dlclose(handle);
    }
  }
};

/// dlopen NVML, initialise it and resolve the device handles.  Returns an
/// initialised Nvml table on success, or nullptr when NVML is unavailable.
std::shared_ptr<GpuMonitor::Nvml> GpuMonitor::open_nvml(int& out_count)
{
  out_count = 0;
  void* handle = ::dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return nullptr;  // no driver / GPU-less host — behave as before
  }

  auto sym = [handle](const char* name) { return ::dlsym(handle, name); };

  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* init = reinterpret_cast<nvmlInit_fn>(sym("nvmlInit_v2"));
  auto* get_count =
      reinterpret_cast<nvmlDeviceGetCount_fn>(sym("nvmlDeviceGetCount_v2"));
  auto* get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_fn>(
      sym("nvmlDeviceGetHandleByIndex_v2"));
  auto tbl = std::make_shared<GpuMonitor::Nvml>();
  tbl->handle = handle;
  tbl->shutdown = reinterpret_cast<nvmlShutdown_fn>(sym("nvmlShutdown"));
  tbl->mem_info = reinterpret_cast<nvmlDeviceGetMemoryInfo_fn>(
      sym("nvmlDeviceGetMemoryInfo"));
  tbl->util_rates = reinterpret_cast<nvmlDeviceGetUtilizationRates_fn>(
      sym("nvmlDeviceGetUtilizationRates"));
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

  if (init == nullptr || get_count == nullptr || get_handle == nullptr
      || tbl->mem_info == nullptr)
  {
    return nullptr;  // handle closed by ~Nvml
  }
  if (init() != k_nvml_success) {
    return nullptr;
  }

  unsigned int count = 0;
  if (get_count(&count) != k_nvml_success || count == 0) {
    return nullptr;
  }
  tbl->devices.resize(count, nullptr);
  for (unsigned int i = 0; i < count; ++i) {
    nvmlDevice_t dev = nullptr;
    if (get_handle(i, &dev) == k_nvml_success) {
      tbl->devices[i] = dev;
    }
  }
  out_count = static_cast<int>(count);
  return tbl;
}

int GpuMonitor::probe_device_count() noexcept
{
  try {
    int count = 0;
    const auto tbl = open_nvml(count);  // ~Nvml shuts NVML down again
    return (tbl != nullptr) ? count : 0;
  } catch (...) {
    return 0;
  }
}

void GpuMonitor::start()
{
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    return;  // already started
  }
  int count = 0;
  nvml_ = open_nvml(count);
  device_count_.store(count, std::memory_order_relaxed);
  {
    const std::scoped_lock lock(states_mutex_);
    states_.assign(static_cast<std::size_t>(count), GpuDeviceState {});
    for (int i = 0; i < count; ++i) {
      states_[static_cast<std::size_t>(i)].index = i;
    }
  }
  if (count == 0) {
    running_.store(false, std::memory_order_relaxed);
    return;  // no GPU — no sampling thread
  }
  spdlog::info("GpuMonitor: NVML loaded, {} CUDA device(s) visible", count);
  sample_once();  // seed values before the first admission decision

  monitor_thread_ = std::jthread(
      [this](const std::stop_token& stop)
      {
        while (running_.load(std::memory_order_relaxed)
               && !stop.stop_requested()) {
          std::unique_lock lock(stop_mutex_);
          if (stop_cv_.wait_for(lock,
                                monitor_interval_,
                                [&]
                                {
                                  return !running_.load(
                                             std::memory_order_relaxed)
                                      || stop.stop_requested();
                                }))
          {
            break;
          }
          lock.unlock();
          sample_once();
        }
      });
}

void GpuMonitor::stop() noexcept
{
  request_stop();
  notify_stop();
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  nvml_.reset();
}

void GpuMonitor::sample_once() noexcept
{
  const auto tbl = nvml_;
  if (!tbl) {
    return;
  }
  const int count = device_count_.load(std::memory_order_relaxed);
  for (int i = 0; i < count; ++i) {
    nvmlDevice_t dev = tbl->devices[static_cast<std::size_t>(i)];
    if (dev == nullptr) {
      continue;
    }
    GpuDeviceState st;
    st.index = i;
    nvmlMemory_t mem {};
    if (tbl->mem_info(dev, &mem) == k_nvml_success) {
      st.vram_total_mib = static_cast<double>(mem.total) / k_bytes_per_mib;
      st.vram_free_mib = static_cast<double>(mem.free) / k_bytes_per_mib;
    }
    if (tbl->util_rates != nullptr) {
      nvmlUtilization_t util {};
      if (tbl->util_rates(dev, &util) == k_nvml_success) {
        st.util_pct = static_cast<double>(util.gpu);
      }
    }
    const std::scoped_lock lock(states_mutex_);
    if (static_cast<std::size_t>(i) < states_.size()) {
      states_[static_cast<std::size_t>(i)] = st;
    }
  }
}

std::optional<GpuDeviceState> GpuMonitor::device(int index) const noexcept
{
  const std::scoped_lock lock(states_mutex_);
  if (index < 0 || static_cast<std::size_t>(index) >= states_.size()) {
    return std::nullopt;
  }
  return states_[static_cast<std::size_t>(index)];
}

std::optional<int> GpuMonitor::least_loaded_device() const noexcept
{
  const std::scoped_lock lock(states_mutex_);
  if (states_.empty()) {
    return std::nullopt;
  }
  int best = 0;
  double best_free = -1.0;
  for (std::size_t i = 0; i < states_.size(); ++i) {
    if (states_[i].vram_free_mib > best_free) {
      best_free = states_[i].vram_free_mib;
      best = static_cast<int>(i);
    }
  }
  return best;
}

}  // namespace gtopt
