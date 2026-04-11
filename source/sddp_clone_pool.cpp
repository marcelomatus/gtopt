/**
 * @file      sddp_clone_pool.cpp
 * @brief     Cached LP clone pool implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_clone_pool.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

LinearInterface& SDDPClonePool::get_or_create(SceneIndex scene_index,
                                              PhaseIndex phase_index,
                                              PlanningLP& planning,
                                              std::size_t base_nrows)
{
  const auto idx = (static_cast<std::size_t>(scene_index)
                    * static_cast<std::size_t>(m_num_phases_))
      + static_cast<std::size_t>(phase_index);

  auto& slot = m_pool_[idx];
  if (!slot.has_value()) {
    // First use: create the clone from the original LP
    slot = planning.system(scene_index, phase_index).linear_interface().clone();
    SPDLOG_TRACE("SDDP clone pool: created clone for scene {} phase {}",
                 scene_index,
                 phase_index);
  } else {
    // Reuse: reset bounds and delete cut rows
    const auto& src_li =
        planning.system(scene_index, phase_index).linear_interface();
    slot->reset_from(src_li, base_nrows);
  }
  return *slot;
}

void SDDPClonePool::batch_create(PlanningLP& planning, Index num_scenes)
{
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    for (const auto pi : iota_range<PhaseIndex>(0, m_num_phases_)) {
      const auto idx = (static_cast<std::size_t>(si)
                        * static_cast<std::size_t>(m_num_phases_))
          + static_cast<std::size_t>(pi);

      auto& slot = m_pool_[idx];
      auto& li = planning.system(si, pi).linear_interface();
      if (li.has_backend()) {
        slot = li.clone();
      }
    }
  }
  SPDLOG_DEBUG("SDDP clone pool: batch-created {} clones",
               num_scenes * m_num_phases_);
}

void SDDPClonePool::release_all() noexcept
{
  for (auto& slot : m_pool_) {
    slot.reset();
  }
}

}  // namespace gtopt
