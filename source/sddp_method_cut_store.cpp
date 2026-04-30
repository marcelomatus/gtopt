/**
 * @file      sddp_method_cut_store.cpp
 * @brief     SDDPMethod cut-store helpers + cut/state persistence
 * @date      2026-04-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Sibling translation unit of ``sddp_method.cpp``; carries a
 * focused subset of ``SDDPMethod``'s member functions to keep
 * each TU under ~700 LoC.  Split landed in commit referenced by
 * Phase B of the gtopt-hygiene refactor.  See
 * ``include/gtopt/sddp_method.hpp`` for the class declaration
 * and ``source/sddp_method.cpp`` for the constructor / solver
 * lifecycle helpers.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_status.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

void SDDPMethod::clear_stored_cuts() noexcept
{
  m_cut_store_.clear();
}

void SDDPMethod::forget_first_cuts(std::ptrdiff_t count)
{
  m_cut_store_.forget_first_cuts(count, planning_lp());
}

void SDDPMethod::update_stored_cut_duals()
{
  m_cut_store_.update_stored_cut_duals(planning_lp());
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPMethod::store_cut(SceneIndex scene_index,
                           PhaseIndex src_phase_index,
                           const SparseRow& cut,
                           CutType type,
                           RowIndex row)
{
  // No `record_cut_row` call here — the unified free function
  // `add_cut_row` (declared in <gtopt/sddp_types.hpp>) is the single
  // owner of low-memory replay registration, and every cut-install
  // site now routes through it.  `store_cut` is pure SDDP-level
  // persistence (pushes into `m_cut_store_` for later CSV/JSON save,
  // rolling-window prune, and cut-file replay).  Recording twice
  // would silently register the same row on `m_active_cuts_`,
  // inflating replay cost without changing LP semantics.
  m_cut_store_.store_cut(scene_index,
                         src_phase_index,
                         cut,
                         type,
                         row,
                         uid_of(scene_index),
                         uid_of(src_phase_index));
}

// ── Helper: resolve an LP via the work pool (avoids naked direct calls) ─────

void SDDPMethod::share_cuts_for_phase(
    PhaseIndex phase_index,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    IterationIndex iteration_index)
{
  gtopt::share_cuts_for_phase(phase_index,
                              scene_cuts,
                              m_options_.cut_sharing,
                              planning_lp(),
                              iteration_index);
}

// ── Cut pruning ─────────────────────────────────────────────────────────────

void SDDPMethod::prune_inactive_cuts()
{
  m_cut_store_.prune_inactive_cuts(
      m_options_, planning_lp(), m_scene_phase_states_);
}

// ── Cut capping ─────────────────────────────────────────────────────────────

void SDDPMethod::cap_stored_cuts()
{
  m_cut_store_.cap_stored_cuts(m_options_, planning_lp());
}

std::vector<StoredCut> SDDPMethod::build_combined_cuts() const
{
  return m_cut_store_.build_combined_cuts(planning_lp());
}

// ── Cut persistence (delegated to sddp_cut_io.hpp free functions) ───────────

auto SDDPMethod::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  // Single source of truth: build the combined view from per-scene
  // vectors.  The `single_cut_storage` option is no longer load-
  // bearing for storage (only per-scene vectors exist); kept in the
  // options struct for backward compatibility.
  const auto combined = m_cut_store_.build_combined_cuts(planning_lp());
  return save_cuts_csv(combined, planning_lp(), filepath);
}

auto SDDPMethod::save_scene_cuts(SceneIndex scene_index,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  return save_scene_cuts_csv(m_cut_store_.at(scene_index).cuts(),
                             scene_index,
                             uid_of(scene_index),
                             planning_lp(),
                             directory);
}

auto SDDPMethod::save_all_scene_cuts(const std::string& directory) const
    -> std::expected<void, Error>
{
  const auto num_scenes = planning_lp().simulation().scene_count();

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    auto result = save_scene_cuts(scene_index, directory);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

auto SDDPMethod::load_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  auto result = load_cuts_csv(planning_lp(),
                              filepath,
                              m_options_.scale_alpha,
                              m_label_maker_,
                              &m_scene_phase_states_);
  // Keep m_iteration_offset_ coherent with whatever was just loaded: the
  // first newly-generated cut must have an iteration_index strictly
  // greater than every loaded one, otherwise save_cuts_for_iteration
  // would stack two cuts under the same index in m_cut_store_.
  if (result.has_value() && result->count > 0) {
    m_iteration_offset_ =
        std::max(m_iteration_offset_, next(result->max_iteration));
  }
  return result;
}

auto SDDPMethod::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<CutLoadResult, Error>
{
  return gtopt::load_scene_cuts_from_directory(planning_lp(),
                                               directory,
                                               m_options_.scale_alpha,
                                               m_label_maker_,
                                               &m_scene_phase_states_);
}

auto SDDPMethod::load_boundary_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_boundary_cuts_csv(planning_lp(),
                                filepath,
                                m_options_,
                                m_label_maker_,
                                m_scene_phase_states_);
}

auto SDDPMethod::load_named_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_named_cuts_csv(planning_lp(),
                             filepath,
                             m_options_,
                             m_label_maker_,
                             m_scene_phase_states_);
}

auto SDDPMethod::save_state(const std::string& filepath)
    -> std::expected<void, Error>
{
  return save_state_csv(planning_lp(), filepath, current_iteration());
}

auto SDDPMethod::load_state(const std::string& filepath)
    -> std::expected<void, Error>
{
  return load_state_csv(planning_lp(), filepath);
}

// ── Monitoring API ───────────────────────────────────────────────────────────
// Implementation moved to sddp_monitor.cpp (write_solver_status free fn).
// maybe_write_api_status below builds the snapshot and delegates.

// ─── Private helper method implementations ───────────────────────────────────

}  // namespace gtopt
