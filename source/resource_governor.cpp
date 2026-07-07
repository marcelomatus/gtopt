/**
 * @file      resource_governor.cpp
 * @brief     ResourceGovernor implementation (per-device GPU token bucket)
 * @date      Mon Jul 06 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cstdlib>

#include <gtopt/gpu_monitor.hpp>
#include <gtopt/resource_governor.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// Tokens minted per GPU device.  Default 1: in Default compute mode CUDA
/// contexts time-slice, so concurrent GPU solves add no throughput — only
/// VRAM pressure.  Raise via GTOPT_GPU_CONCURRENCY on MPS-enabled hosts.
[[nodiscard]] int env_gpu_concurrency() noexcept
{
  // getenv: read once under the governor lock.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* env = std::getenv("GTOPT_GPU_CONCURRENCY"); env != nullptr) {
    const int value = std::atoi(env);  // NOLINT(cert-err34-c)
    if (value > 0) {
      return value;
    }
  }
  return 1;
}
}  // namespace

ResourceGovernor& ResourceGovernor::instance() noexcept
{
  static ResourceGovernor governor;
  return governor;
}

void ResourceGovernor::configure(int device_count,
                                 int gpu_concurrency_per_device) noexcept
{
  const std::scoped_lock lock(mutex_);
  configured_ = true;
  per_device_capacity_ = std::max(1, gpu_concurrency_per_device);
  const auto n = static_cast<std::size_t>(std::max(0, device_count));
  free_tokens_.assign(n, per_device_capacity_);
}

void ResourceGovernor::ensure_configured_unlocked() noexcept
{
  if (configured_) {
    return;
  }
  configured_ = true;
  try {
    const int devices = GpuMonitor::probe_device_count();
    per_device_capacity_ = env_gpu_concurrency();
    free_tokens_.assign(static_cast<std::size_t>(std::max(0, devices)),
                        per_device_capacity_);
    if (devices > 0) {
      spdlog::info(
          "ResourceGovernor: GPU admission enabled — {} device(s) x {} "
          "token(s) (GTOPT_GPU_CONCURRENCY)",
          devices,
          per_device_capacity_);
    }
  } catch (...) {
    // Degrade to disabled gating (admit-all): the backend's own
    // serialization mutex still carries correctness.
    free_tokens_.clear();
  }
}

bool ResourceGovernor::gpu_available() noexcept
{
  const std::scoped_lock lock(mutex_);
  ensure_configured_unlocked();
  if (free_tokens_.empty()) {
    return true;  // gating disabled (no NVML / GPU-less) — never throttle
  }
  return std::ranges::any_of(free_tokens_,
                             [](const int tok) { return tok > 0; });
}

ResourceGovernor::Ticket ResourceGovernor::try_admit(ResourceClass cls) noexcept
{
  if (cls == ResourceClass::cpu) {
    // CPU tasks never consume a GPU token — always admitted, no device.
    return Ticket {this, std::nullopt, /*admitted=*/true};
  }

  const std::scoped_lock lock(mutex_);
  ensure_configured_unlocked();
  if (free_tokens_.empty()) {
    // GPU gating not configured (no NVML / GPU-less): admit and rely on the
    // plugin-level serialization mutex.  Behaviour identical to pre-governor.
    return Ticket {this, std::nullopt, /*admitted=*/true};
  }

  // Assign the least-loaded device (most free tokens) that has capacity.
  int best = -1;
  int best_free = 0;
  for (std::size_t i = 0; i < free_tokens_.size(); ++i) {
    if (free_tokens_[i] > best_free) {
      best_free = free_tokens_[i];
      best = static_cast<int>(i);
    }
  }
  if (best < 0) {
    return {};  // all tokens in use — throttle
  }
  --free_tokens_[static_cast<std::size_t>(best)];
  return Ticket {this, best, /*admitted=*/true};
}

void ResourceGovernor::release_device(int device) noexcept
{
  const std::scoped_lock lock(mutex_);
  if (device < 0 || static_cast<std::size_t>(device) >= free_tokens_.size()) {
    return;
  }
  auto& tok = free_tokens_[static_cast<std::size_t>(device)];
  tok = std::min(tok + 1, per_device_capacity_);
}

int ResourceGovernor::gpu_capacity() const noexcept
{
  const std::scoped_lock lock(mutex_);
  return static_cast<int>(free_tokens_.size()) * per_device_capacity_;
}

int ResourceGovernor::gpu_in_use() const noexcept
{
  const std::scoped_lock lock(mutex_);
  int free = 0;
  for (const int t : free_tokens_) {
    free += t;
  }
  return (static_cast<int>(free_tokens_.size()) * per_device_capacity_) - free;
}

int ResourceGovernor::device_count() const noexcept
{
  const std::scoped_lock lock(mutex_);
  return static_cast<int>(free_tokens_.size());
}

void ResourceGovernor::Ticket::release() noexcept
{
  if (gov_ != nullptr && device_.has_value()) {
    gov_->release_device(*device_);
  }
  gov_ = nullptr;
  device_ = std::nullopt;
  admitted_ = false;
}

}  // namespace gtopt
