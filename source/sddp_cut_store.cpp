/**
 * @file      sddp_cut_store.cpp
 * @brief     Thread-safe storage for SDDP Benders cuts — implementation
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements SDDPCutStore methods extracted from sddp_method.cpp.
 */

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <vector>

#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_state_io.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ── store_cut ──────────────────────────────────────────────────────────────

void SDDPCutStore::store_cut(SceneIndex scene,
                             PhaseIndex /*src_phase*/,
                             const SparseRow& cut,
                             CutType type,
                             RowIndex row,
                             bool single_cut_storage,
                             SceneUid scene_uid_val,
                             PhaseUid phase_uid_val)
{
  StoredCut stored {
      .type = type,
      .phase = phase_uid_val,
      .scene = scene_uid_val,
      .name = cut.name,
      .rhs = cut.lowb,
      .row = row,
  };
  stored.coefficients.reserve(cut.cmap.size());
  for (const auto& [col, coeff] : cut.cmap) {
    stored.coefficients.emplace_back(static_cast<int>(col), coeff);
  }
  if (single_cut_storage) {
    m_scene_cuts_[scene].push_back(std::move(stored));
  } else {
    m_scene_cuts_[scene].push_back(stored);
    const std::scoped_lock lock(m_cuts_mutex_);
    m_stored_cuts_.push_back(std::move(stored));
  }
}

// ── clear ──────────────────────────────────────────────────────────────────

void SDDPCutStore::clear() noexcept
{
  {
    const std::scoped_lock lk(m_cuts_mutex_);
    m_stored_cuts_.clear();
  }
  for (auto& sc : m_scene_cuts_) {
    sc.clear();
  }
}

// ── forget_first_cuts ──────────────────────────────────────────────────────

void SDDPCutStore::forget_first_cuts(int count, PlanningLP& planning_lp)
{
  if (count <= 0) {
    return;
  }

  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  using ScenePhaseKey = std::pair<SceneIndex, PhaseIndex>;

  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return std::nullopt;
    }
    return ScenePhaseKey {sit->second, pit->second};
  };

  std::map<ScenePhaseKey, std::vector<int>> rows_to_delete;
  std::set<std::string> names_to_forget;

  int n = 0;
  {
    const std::scoped_lock lk(m_cuts_mutex_);
    n = std::min(count, static_cast<int>(m_stored_cuts_.size()));
    for (const auto& cut : m_stored_cuts_ | std::views::take(n)) {
      if (!cut.name.empty()) {
        names_to_forget.insert(cut.name);
      }
      if (auto key = resolve_key(cut)) {
        rows_to_delete[*key].push_back(static_cast<int>(cut.row));
      }
    }
    m_stored_cuts_.erase(m_stored_cuts_.begin(), m_stored_cuts_.begin() + n);
  }

  std::map<ScenePhaseKey, int> deleted_count;
  int total_deleted = 0;
  for (auto& [key, rows] : rows_to_delete) {
    auto& li = planning_lp.system(key.first, key.second).linear_interface();
    std::ranges::sort(rows);
    li.delete_rows(rows);
    deleted_count[key] = static_cast<int>(rows.size());
    total_deleted += static_cast<int>(rows.size());
  }

  auto shift_row = [&](StoredCut& cut)
  {
    if (auto key = resolve_key(cut)) {
      if (auto it = deleted_count.find(*key); it != deleted_count.end()) {
        cut.row -= RowIndex {it->second};
      }
    }
  };

  {
    const std::scoped_lock lk(m_cuts_mutex_);
    std::ranges::for_each(m_stored_cuts_, shift_row);
  }

  for (auto&& [si, cuts] : enumerate<SceneIndex>(m_scene_cuts_)) {
    std::erase_if(
        cuts,
        [&](const StoredCut& c)
        { return !c.name.empty() && names_to_forget.contains(c.name); });
    std::ranges::for_each(cuts, shift_row);
  }

  SPDLOG_INFO(
      "SDDP: forgot {} inherited cuts ({} LP rows deleted)", n, total_deleted);
}

// ── update_stored_cut_duals ────────────────────────────────────────────────

void SDDPCutStore::update_stored_cut_duals(const PlanningLP& planning_lp)
{
  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  auto update_dual = [&](StoredCut& cut)
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return;
    }
    const auto& li =
        planning_lp.system(sit->second, pit->second).linear_interface();
    const auto row_idx = static_cast<std::size_t>(cut.row);
    const auto duals = li.get_row_dual();
    if (row_idx < duals.size()) {
      cut.dual = duals[row_idx];
    }
  };

  {
    const std::scoped_lock lk(m_cuts_mutex_);
    std::ranges::for_each(m_stored_cuts_, update_dual);
  }
  for (auto& sc : m_scene_cuts_) {
    std::ranges::for_each(sc, update_dual);
  }
}

// ── prune_inactive_cuts ────────────────────────────────────────────────────

void SDDPCutStore::prune_inactive_cuts(
    const SDDPOptions& options,
    PlanningLP& planning_lp,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states)
{
  const auto max_cuts = options.max_cuts_per_phase;
  if (max_cuts <= 0) {
    return;
  }

  const auto threshold = options.prune_dual_threshold;
  int total_pruned = 0;

  using ScenePhaseKey = std::pair<SceneIndex, PhaseIndex>;
  std::map<ScenePhaseKey, std::vector<Index>> all_deleted;

  for (auto&& [scene, scene_states] : enumerate<SceneIndex>(scene_phase_states))
  {
    for (auto&& [phase, psi] : enumerate<PhaseIndex>(scene_states)) {
      auto& li = planning_lp.system(scene, phase).linear_interface();

      const auto total_rows = static_cast<Index>(li.get_numrows());
      const auto base = static_cast<Index>(psi.base_nrows);
      const auto num_cut_rows = total_rows - base;
      if (num_cut_rows <= max_cuts) {
        continue;
      }

      const auto duals = li.get_row_dual();

      struct CutInfo
      {
        int row;
        double abs_dual;
      };
      std::vector<CutInfo> cut_infos;
      cut_infos.reserve(total_rows - base);
      for (auto r = base; r < total_rows; ++r) {
        cut_infos.push_back(CutInfo {
            .row = r,
            .abs_dual = std::abs(duals[r]),
        });
      }
      std::ranges::sort(cut_infos, {}, &CutInfo::abs_dual);

      const auto to_remove = num_cut_rows - max_cuts;
      std::vector<Index> rows_to_delete;
      rows_to_delete.reserve(to_remove);
      for (const auto& ci : cut_infos | std::views::take(to_remove)) {
        if (ci.abs_dual < threshold) {
          rows_to_delete.push_back(ci.row);
        }
      }

      if (rows_to_delete.empty()) {
        continue;
      }

      std::ranges::sort(rows_to_delete);

      SPDLOG_DEBUG(
          "SDDP: pruning {} inactive cuts from scene {} phase {} "
          "({} cut rows, max {})",
          rows_to_delete.size(),
          planning_lp.simulation().scenes()[scene].uid(),
          planning_lp.simulation().phases()[phase].uid(),
          num_cut_rows,
          max_cuts);

      li.delete_rows(rows_to_delete);
      total_pruned += static_cast<int>(rows_to_delete.size());
      all_deleted[{scene, phase}] = std::move(rows_to_delete);
    }
  }

  if (total_pruned == 0) {
    return;
  }

  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase);
    auto sit = scene_map.find(cut.scene);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return std::nullopt;
    }
    return ScenePhaseKey {sit->second, pit->second};
  };

  auto find_deleted = [&](const StoredCut& cut) -> const std::vector<Index>*
  {
    if (auto key = resolve_key(cut)) {
      if (auto it = all_deleted.find(*key); it != all_deleted.end()) {
        return &it->second;
      }
    }
    return nullptr;
  };

  auto was_deleted = [](const std::vector<Index>& deleted, RowIndex row) -> bool
  { return std::ranges::binary_search(deleted, static_cast<Index>(row)); };

  auto compute_shift = [](const std::vector<Index>& deleted,
                          RowIndex row) -> RowIndex
  {
    return RowIndex {static_cast<Index>(
        std::ranges::lower_bound(deleted, static_cast<Index>(row) + 1)
        - deleted.begin())};
  };

  auto update_cuts = [&](std::vector<StoredCut>& cuts)
  {
    std::erase_if(cuts,
                  [&](const StoredCut& cut)
                  {
                    const auto* del = find_deleted(cut);
                    return del != nullptr && was_deleted(*del, cut.row);
                  });
    for (auto& cut : cuts) {
      if (const auto* del = find_deleted(cut)) {
        cut.row -= compute_shift(*del, cut.row);
      }
    }
  };

  {
    const std::scoped_lock lk(m_cuts_mutex_);
    update_cuts(m_stored_cuts_);
  }
  for (auto& sc : m_scene_cuts_) {
    update_cuts(sc);
  }

  SPDLOG_INFO("SDDP: pruned {} inactive cuts across all LPs", total_pruned);
}

// ── cap_stored_cuts ────────────────────────────────────────────────────────

void SDDPCutStore::cap_stored_cuts(const SDDPOptions& options,
                                   const PlanningLP& planning_lp)
{
  const auto max_cuts = options.max_stored_cuts;
  if (max_cuts <= 0) {
    return;
  }
  const auto limit = static_cast<std::size_t>(max_cuts);

  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  int total_dropped = 0;

  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    auto& cuts = m_scene_cuts_[scene];
    if (cuts.size() > limit) {
      const auto excess = cuts.size() - limit;
      cuts.erase(cuts.begin(),
                 cuts.begin() + static_cast<std::ptrdiff_t>(excess));
      total_dropped += static_cast<int>(excess);
    }
  }

  if (!options.single_cut_storage) {
    const std::scoped_lock lock(m_cuts_mutex_);
    const auto total_limit = limit * static_cast<std::size_t>(num_scenes);
    if (m_stored_cuts_.size() > total_limit) {
      const auto excess = m_stored_cuts_.size() - total_limit;
      m_stored_cuts_.erase(
          m_stored_cuts_.begin(),
          m_stored_cuts_.begin() + static_cast<std::ptrdiff_t>(excess));
    }
  }

  if (total_dropped > 0) {
    SPDLOG_INFO("SDDP: capped stored cuts, dropped {} oldest entries",
                total_dropped);
  }
}

// ── build_combined_cuts ────────────────────────────────────────────────────

std::vector<StoredCut> SDDPCutStore::build_combined_cuts(
    const PlanningLP& planning_lp) const
{
  std::vector<StoredCut> combined;
  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    const auto& cuts = m_scene_cuts_[scene];
    combined.insert(combined.end(), cuts.begin(), cuts.end());
  }
  return combined;
}

// ── apply_cut_sharing_for_iteration ────────────────────────────────────────

void SDDPCutStore::apply_cut_sharing_for_iteration(
    std::size_t cuts_before,
    IterationIndex iteration,
    const SDDPOptions& options,
    PlanningLP& planning_lp,
    const LabelMaker& label_maker)
{
  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  const auto num_phases =
      static_cast<Index>(planning_lp.simulation().phases().size());

  if (options.cut_sharing == CutSharingMode::none || num_scenes <= 1) {
    return;
  }

  auto to_sparse_row = [](const StoredCut& sc) -> SparseRow
  {
    auto row = SparseRow {
        .name = sc.name,
        .lowb = sc.rhs,
        .uppb = LinearProblem::DblMax,
    };
    for (const auto& [col, coeff] : sc.coefficients) {
      row[ColIndex {col}] = coeff;
    }
    return row;
  };

  flat_map<SceneUid, SceneIndex> scene_uid_map;
  const auto& scenes = planning_lp.simulation().scenes();
  for (auto&& [si, sc_lp] : enumerate<SceneIndex>(scenes)) {
    scene_uid_map[sc_lp.uid()] = si;
  }

  const auto& phases = planning_lp.simulation().phases();
  auto phase_uid_fn = [&](PhaseIndex pi) -> PhaseUid
  { return phases[pi].uid(); };

  for (const auto pi : iota_range<PhaseIndex>(0, num_phases - 1)) {
    StrongIndexVector<SceneIndex, std::vector<SparseRow>> per_scene_cuts;
    per_scene_cuts.resize(num_scenes);

    const auto pi_uid = phase_uid_fn(pi);

    if (options.single_cut_storage) {
      for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
        const auto& cuts = m_scene_cuts_[si];
        const auto offset = m_scene_cuts_before_[static_cast<std::size_t>(si)];
        for (std::size_t ci = offset; ci < cuts.size(); ++ci) {
          const auto& sc = cuts[ci];
          if (sc.type != CutType::Optimality || sc.phase != pi_uid) {
            continue;
          }
          per_scene_cuts[si].push_back(to_sparse_row(sc));
        }
      }
    } else {
      for (std::size_t ci = cuts_before; ci < m_stored_cuts_.size(); ++ci) {
        const auto& sc = m_stored_cuts_[ci];
        if (sc.type != CutType::Optimality || sc.phase != pi_uid) {
          continue;
        }
        auto sit = scene_uid_map.find(sc.scene);
        if (sit != scene_uid_map.end()) {
          per_scene_cuts[sit->second].push_back(to_sparse_row(sc));
        }
      }
    }

    gtopt::share_cuts_for_phase(
        pi,
        per_scene_cuts,
        options.cut_sharing,
        planning_lp,
        label_maker.lp_label("sddp", "share", pi, iteration));
  }
}

// ── save_cuts_for_iteration ────────────────────────────────────────────────

void SDDPCutStore::save_cuts_for_iteration(
    IterationIndex iter,
    std::span<const uint8_t> scene_feasible,
    const SDDPOptions& options,
    PlanningLP& planning_lp,
    const LabelMaker& /*label_maker*/,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
    /*scene_phase_states*/,
    int current_iteration)
{
  if (options.cuts_output_file.empty()) {
    return;
  }

  const auto cut_dir =
      std::filesystem::path(options.cuts_output_file).parent_path();

  // Helper: save all cuts to a file
  auto save_all = [&](const std::string& filepath) -> std::expected<void, Error>
  {
    if (options.single_cut_storage) {
      const auto combined = build_combined_cuts(planning_lp);
      return save_cuts_csv(combined, planning_lp, filepath);
    }
    return save_cuts_csv(m_stored_cuts_, planning_lp, filepath);
  };

  // Save to a versioned file: sddp_cuts_<iter>.csv
  if (!cut_dir.empty()) {
    const auto versioned_file =
        (cut_dir / std::format(sddp_file::versioned_cuts_fmt, iter)).string();
    auto result = save_all(versioned_file);
    if (!result.has_value()) {
      SPDLOG_WARN("SDDP: could not save versioned cuts at iter {}: {}",
                  iter,
                  result.error().message);
    }
  }

  // Save per-scene cuts
  if (!cut_dir.empty()) {
    const auto num_scenes =
        static_cast<Index>(planning_lp.simulation().scenes().size());
    const auto& scenes = planning_lp.simulation().scenes();
    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      auto result = save_scene_cuts_csv(m_scene_cuts_[scene],
                                        scene,
                                        scenes[scene].uid(),
                                        planning_lp,
                                        cut_dir.string());
      if (!result.has_value()) {
        SPDLOG_WARN("SDDP: could not save per-scene cuts at iter {}: {}",
                    iter,
                    result.error().message);
        break;
      }
    }
  }

  // Save state variable column solutions (latest + versioned)
  if (!cut_dir.empty()) {
    const auto state_file = (cut_dir / sddp_file::state_cols).string();
    auto sr = save_state_csv(
        planning_lp, state_file, IterationIndex {current_iteration});
    if (!sr.has_value()) {
      SPDLOG_WARN("SDDP: could not save state at iter {}: {}",
                  iter,
                  sr.error().message);
    }
    const auto versioned_state =
        (cut_dir / std::format(sddp_file::versioned_state_fmt, iter)).string();
    sr = save_state_csv(
        planning_lp, versioned_state, IterationIndex {current_iteration});
    if (!sr.has_value()) {
      SPDLOG_WARN("SDDP: could not save versioned state at iter {}: {}",
                  iter,
                  sr.error().message);
    }
  }

  // Rename cut files for infeasible scenes
  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  const auto& scenes = planning_lp.simulation().scenes();
  for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[static_cast<std::size_t>(scene)] != 0U) {
      continue;
    }
    if (cut_dir.empty()) {
      continue;
    }
    const auto suid = scenes[scene].uid();
    const auto scene_file =
        cut_dir / std::format(sddp_file::scene_cuts_fmt, suid);
    const auto error_file =
        cut_dir / std::format(sddp_file::error_scene_cuts_fmt, suid);
    std::error_code ec;
    if (std::filesystem::exists(scene_file, ec)) {
      std::filesystem::rename(scene_file, error_file, ec);
      if (!ec) {
        SPDLOG_TRACE("SDDP: renamed cut file for infeasible scene {} to {}",
                     scene,
                     error_file.string());
      }
    }
  }
}

}  // namespace gtopt
