// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_resource_governor.cpp
 * @brief     Unit tests for ResourceGovernor + GpuMonitor + descriptor ABI
 *
 * Pure-logic tests (no GPU required): the governor is configured explicitly
 * per test and restored to the machine's real state afterwards (it is a
 * process-global singleton shared with any cuOpt solves in this binary).
 */

#include <atomic>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gpu_monitor.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/resource_governor.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/solver_resource.hpp>
#include <gtopt/work_pool.hpp>

using namespace gtopt;

namespace resource_governor_test
{
namespace
{

/// RAII: configure the process-global governor for a test and restore the
/// machine's real (NVML-probed) configuration on scope exit, so later tests
/// (including real cuOpt solves) see the true state.
class GovernorConfigGuard
{
public:
  GovernorConfigGuard(int devices, int per_device)
  {
    ResourceGovernor::instance().configure(devices, per_device);
  }
  GovernorConfigGuard(const GovernorConfigGuard&) = delete;
  GovernorConfigGuard& operator=(const GovernorConfigGuard&) = delete;
  GovernorConfigGuard(GovernorConfigGuard&&) = delete;
  GovernorConfigGuard& operator=(GovernorConfigGuard&&) = delete;
  ~GovernorConfigGuard()
  {
    ResourceGovernor::instance().configure(GpuMonitor::probe_device_count(), 1);
  }
};

}  // namespace
}  // namespace resource_governor_test

TEST_CASE(
    "ResourceGovernor - capacity-1 admits exactly one GPU ticket")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(1, 1);
  auto& gov = ResourceGovernor::instance();

  CHECK(gov.device_count() == 1);
  CHECK(gov.gpu_capacity() == 1);
  CHECK(gov.gpu_in_use() == 0);
  CHECK(gov.gpu_available());

  auto first = gov.try_admit(ResourceClass::gpu);
  REQUIRE(static_cast<bool>(first));
  CHECK(first.device().value_or(-1) == 0);
  CHECK(gov.gpu_in_use() == 1);
  CHECK_FALSE(gov.gpu_available());

  // Second GPU ticket must be refused while the first is held.
  auto second = gov.try_admit(ResourceClass::gpu);
  CHECK_FALSE(static_cast<bool>(second));
  CHECK(gov.gpu_in_use() == 1);

  // Releasing the first frees the token again.
  first = ResourceGovernor::Ticket {};
  CHECK(gov.gpu_in_use() == 0);
  CHECK(gov.gpu_available());
  auto third = gov.try_admit(ResourceClass::gpu);
  CHECK(static_cast<bool>(third));
}

TEST_CASE("ResourceGovernor - CPU tasks never consume GPU tokens")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(1, 1);
  auto& gov = ResourceGovernor::instance();

  std::vector<ResourceGovernor::Ticket> cpu_tickets;
  for (int i = 0; i < 8; ++i) {
    auto t = gov.try_admit(ResourceClass::cpu);
    CHECK(static_cast<bool>(t));
    CHECK_FALSE(t.device().has_value());
    cpu_tickets.push_back(std::move(t));
  }
  CHECK(gov.gpu_in_use() == 0);
  CHECK(gov.gpu_available());
}

TEST_CASE(
    "ResourceGovernor - zero devices disables gating (admit-all)")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(0, 1);
  auto& gov = ResourceGovernor::instance();

  CHECK(gov.device_count() == 0);
  CHECK(gov.gpu_capacity() == 0);
  CHECK(gov.gpu_available());  // gating disabled — never throttle
  auto t1 = gov.try_admit(ResourceClass::gpu);
  auto t2 = gov.try_admit(ResourceClass::gpu);
  CHECK(static_cast<bool>(t1));
  CHECK(static_cast<bool>(t2));
  CHECK_FALSE(t1.device().has_value());  // no device assignment
  CHECK(gov.gpu_in_use() == 0);
}

TEST_CASE("ResourceGovernor - multi-device least-loaded assignment")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(2, 2);
  auto& gov = ResourceGovernor::instance();

  CHECK(gov.gpu_capacity() == 4);
  auto a = gov.try_admit(ResourceClass::gpu);
  REQUIRE(static_cast<bool>(a));
  // Second admit must land on the OTHER device (it now has more free
  // tokens than the first's).
  auto b = gov.try_admit(ResourceClass::gpu);
  REQUIRE(static_cast<bool>(b));
  CHECK(a.device().value_or(-1) != b.device().value_or(-2));

  auto c = gov.try_admit(ResourceClass::gpu);
  auto d = gov.try_admit(ResourceClass::gpu);
  CHECK(static_cast<bool>(c));
  CHECK(static_cast<bool>(d));
  CHECK(gov.gpu_in_use() == 4);
  // Bucket exhausted.
  auto e = gov.try_admit(ResourceClass::gpu);
  CHECK_FALSE(static_cast<bool>(e));
}

TEST_CASE(
    "ResourceGovernor - ticket move semantics release exactly once")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(1, 1);
  auto& gov = ResourceGovernor::instance();

  auto a = gov.try_admit(ResourceClass::gpu);
  REQUIRE(static_cast<bool>(a));
  CHECK(gov.gpu_in_use() == 1);

  // Move: source becomes empty, token still held exactly once.
  auto b = std::move(a);
  CHECK(static_cast<bool>(b));
  CHECK_FALSE(static_cast<bool>(
      a));  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
  CHECK(gov.gpu_in_use() == 1);

  // Move-assign over an empty ticket, then destroy: single release.
  {
    ResourceGovernor::Ticket c;
    c = std::move(b);
    CHECK(static_cast<bool>(c));
    CHECK(gov.gpu_in_use() == 1);
  }
  CHECK(gov.gpu_in_use() == 0);
}

TEST_CASE("BasicTaskRequirements - default resource class is cpu")  // NOLINT
{
  const BasicTaskRequirements<> req {};
  CHECK(req.resource_class == ResourceClass::cpu);
}

TEST_CASE(
    "GpuMonitor - probe and lifecycle are safe with or without a GPU")  // NOLINT
{
  // Never asserts GPU presence (must pass on GPU-less CI): only internal
  // consistency between the probe, has_gpu() and device_count().
  const int probed = GpuMonitor::probe_device_count();
  CHECK(probed >= 0);

  GpuMonitor mon;
  mon.start();
  CHECK(mon.device_count() == probed);
  CHECK(mon.has_gpu() == (probed > 0));
  CHECK_FALSE(mon.device(-1).has_value());
  CHECK_FALSE(mon.device(probed).has_value());
  if (probed > 0) {
    const auto st = mon.device(0);
    REQUIRE(st.has_value());
    CHECK(st->index == 0);
    CHECK(st->vram_total_mib > 0.0);
    CHECK(mon.least_loaded_device().has_value());
  } else {
    CHECK_FALSE(mon.least_loaded_device().has_value());
  }
  mon.stop();
  mon.stop();  // idempotent
}

TEST_CASE("SolverRegistry - resource descriptor plumbing")  // NOLINT
{
  auto& reg = SolverRegistry::instance();

  SUBCASE("CPU solvers export no descriptor and read as cpu")
  {
    if (reg.has_solver("clp")) {
      CHECK_FALSE(reg.resource_descriptor("clp").has_value());
      CHECK_FALSE(reg.is_gpu_backed("clp"));
    }
  }

  SUBCASE("cuopt declares gpu class with serial concurrency")
  {
    if (!reg.has_solver("cuopt")) {
      return;  // plugin not built/loadable on this host — skip
    }
    const auto desc = reg.resource_descriptor("cuopt");
    REQUIRE(desc.has_value());
    CHECK(desc->struct_version == k_solver_resource_desc_version);
    CHECK(desc->resource_class == static_cast<int>(ResourceClass::gpu));
    CHECK(desc->recommended_max_concurrency == 1);
    CHECK(desc->est_vram_mib_base > 0.0);
    CHECK(reg.is_gpu_backed("cuopt"));
  }
}

TEST_CASE("LinearInterface - resource_class reflects the backend")  // NOLINT
{
  auto& reg = SolverRegistry::instance();

  SUBCASE("CPU backend reads cpu")
  {
    if (reg.has_solver("clp")) {
      const LinearInterface li {"clp"};
      CHECK(li.resource_class() == ResourceClass::cpu);
    }
  }

  SUBCASE("cuopt backend reads gpu")
  {
    if (!reg.has_solver("cuopt")) {
      return;
    }
    const LinearInterface li {"cuopt"};
    CHECK(li.resource_class() == ResourceClass::gpu);
  }
}

TEST_CASE(
    "AdaptiveWorkPool - GPU-class tasks serialize on the token bucket")  // NOLINT
{
  using namespace resource_governor_test;
  const GovernorConfigGuard guard(1, 1);  // one device, one token

  AdaptiveWorkPool pool(WorkPoolConfig {
      4,  // max_threads
      95.0,  // max_cpu_threshold
      64.0,  // min_free_memory_mb
      99.0,  // max_memory_percent
      0.0,  // max_process_rss_mb (off)
      std::chrono::milliseconds(5),  // scheduler_interval
      "test-gpu-gate",
      false,  // enable_periodic_stats
  });
  pool.start();

  std::atomic<int> concurrent {0};
  std::atomic<int> peak {0};
  constexpr int k_tasks = 6;

  std::vector<std::future<void>> futs;
  for (int i = 0; i < k_tasks; ++i) {
    TaskRequirements req;
    req.resource_class = ResourceClass::gpu;
    auto fut = pool.submit(
        [&concurrent, &peak]
        {
          const int now =
              concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
          int prev = peak.load(std::memory_order_relaxed);
          while (prev < now
                 && !peak.compare_exchange_weak(
                     prev, now, std::memory_order_relaxed))
          {}
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          concurrent.fetch_sub(1, std::memory_order_acq_rel);
        },
        req);
    REQUIRE(fut.has_value());
    futs.push_back(std::move(*fut));
  }
  for (auto& f : futs) {
    f.get();
  }
  pool.shutdown();

  // One token ⇒ at most one GPU task in flight at any instant.  (The
  // universal idle-admit and the post-pop race fallback can never exceed
  // the token count within a single pool.)
  CHECK(peak.load() == 1);
}
