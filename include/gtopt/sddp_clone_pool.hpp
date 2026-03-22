/**
 * @file      sddp_clone_pool.hpp
 * @brief     Cached LP clone pool for SDDP aperture reuse
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Manages a flat pool of optional LinearInterface clones indexed by
 * (scene, phase).  Clones are created lazily on first access and reset
 * from the source LP on subsequent accesses.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>

namespace gtopt
{

class SystemLP;
class PlanningLP;
struct PhaseStateInfo;

/// Cached LP clone pool for SDDP aperture reuse.
///
/// Stores one optional LinearInterface per (scene, phase) slot.
/// Clones are created lazily from the source LP on first access;
/// subsequent accesses reset the clone's bounds and delete cut rows.
class SDDPClonePool
{
public:
  SDDPClonePool() = default;

  /// Allocate the pool for the given number of scenes and phases.
  /// All slots start empty (nullopt).
  void allocate(Index num_scenes, Index num_phases)
  {
    m_num_phases_ = num_phases;
    m_pool_.resize(static_cast<std::size_t>(num_scenes)
                   * static_cast<std::size_t>(num_phases));
  }

  /// Whether the pool has been allocated (non-empty).
  [[nodiscard]] bool is_allocated() const noexcept { return !m_pool_.empty(); }

  /// Get or create a cached clone for the given (scene, phase).
  ///
  /// On first call for a slot, clones the source LP.  On subsequent
  /// calls, resets column bounds from the source LP and deletes rows
  /// beyond base_nrows.
  ///
  /// @param scene      Scene index
  /// @param phase      Phase index
  /// @param planning   PlanningLP reference (for source LP access)
  /// @param base_nrows Number of structural rows (cuts above this are reset)
  /// @return Reference to the cached clone
  [[nodiscard]] LinearInterface& get_or_create(SceneIndex scene,
                                               PhaseIndex phase,
                                               PlanningLP& planning,
                                               std::size_t base_nrows);

  /// Get a pooled clone pointer, or nullptr if pool is not allocated.
  [[nodiscard]] LinearInterface* get_ptr(SceneIndex scene,
                                         PhaseIndex phase,
                                         PlanningLP& planning,
                                         std::size_t base_nrows)
  {
    if (m_pool_.empty()) {
      return nullptr;
    }
    return &get_or_create(scene, phase, planning, base_nrows);
  }

private:
  std::vector<std::optional<LinearInterface>> m_pool_ {};
  Index m_num_phases_ {0};
};

}  // namespace gtopt
