/**
 * @file      solver_resource.hpp
 * @brief     Resource class + descriptor a solver backend declares to the pool
 * @date      Mon Jul 06 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Small, dependency-free header shared by the plugin ABI (@ref
 * solver_backend.hpp), the registry, the work-pool admission layer (@ref
 * resource_governor.hpp) and the SDDP solver tier.  A backend *declares* its
 * dominant resource class + estimated per-solve GPU footprint here; the work
 * pool *enforces* capacity (decoupling "who knows the cost" from "who enforces
 * it").  A backend that exports nothing is treated as ``cpu`` — today's
 * behaviour, zero regression.
 */

#pragma once

#include <cstdint>

namespace gtopt
{

/// Broad execution class of a task's dominant resource.
enum class ResourceClass : std::uint8_t
{
  cpu = 0,  ///< host-CPU solve; full cpu_factor x cores fan-out
  gpu = 1,  ///< device solve; gated by the per-device GPU token bucket
};

/// Descriptor a solver plugin optionally advertises (via the C entry point
/// ``gtopt_solver_resource_descriptor``).  A plain POD so it crosses the C ABI
/// and can grow via ``struct_version`` WITHOUT bumping ``k_solver_abi_version``
/// (older plugins simply don't export the symbol -> treated as ``cpu``).
struct SolverResourceDescriptor
{
  int struct_version {0};  ///< == k_solver_resource_desc_version
  int resource_class {0};  ///< ResourceClass value (0 = cpu, 1 = gpu)
  int device_kind {0};  ///< 0 = none/host, 1 = cuda
  double est_vram_mib_base {0.0};  ///< per-context base VRAM (0 for CPU)
  double est_vram_mib_per_mnnz {0.0};  ///< marginal VRAM per 1e6 matrix nnz
  int recommended_max_concurrency {0};  ///< 1 = single-GPU Default; 0 = unknown
  int spawns_host_threads {0};  ///< solver's own CPU thread fan-out
};

/// Bumped whenever @ref SolverResourceDescriptor gains/reorders a field.
inline constexpr int k_solver_resource_desc_version = 1;

/// Convenience: a default (CPU) descriptor for backends that export nothing.
[[nodiscard]] constexpr SolverResourceDescriptor cpu_resource_descriptor()
{
  return SolverResourceDescriptor {
      .struct_version = k_solver_resource_desc_version,
      .resource_class = static_cast<int>(ResourceClass::cpu),
  };
}

}  // namespace gtopt
