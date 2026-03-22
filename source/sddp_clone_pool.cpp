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

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

LinearInterface& SDDPClonePool::get_or_create(SceneIndex scene,
                                              PhaseIndex phase,
                                              PlanningLP& planning,
                                              std::size_t base_nrows)
{
  const auto idx = (static_cast<std::size_t>(scene)
                    * static_cast<std::size_t>(m_num_phases_))
      + static_cast<std::size_t>(phase);

  auto& slot = m_pool_[idx];
  if (!slot.has_value()) {
    // First use: create the clone from the original LP
    slot = planning.system(scene, phase).linear_interface().clone();
    SPDLOG_TRACE(
        "SDDP clone pool: created clone for scene {} phase {}", scene, phase);
  } else {
    // Reuse: reset bounds and delete cut rows
    const auto& src_li = planning.system(scene, phase).linear_interface();
    slot->reset_from(src_li, base_nrows);
  }
  return *slot;
}

}  // namespace gtopt
