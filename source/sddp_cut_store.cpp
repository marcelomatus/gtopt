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
#include <unordered_map>
#include <vector>

#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_state_io.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ── store_cut ──────────────────────────────────────────────────────────────

void SDDPCutStore::store_cut(SceneIndex scene_index,
                             PhaseIndex /*src_phase_index*/,
                             const SparseRow& cut,
                             CutType type,
                             RowIndex row,
                             SceneUid scene_uid_val,
                             PhaseUid phase_uid_val)
{
  auto cut_name = LabelMaker {LpNamesLevel::all}.make_row_label(cut);

  StoredCut stored {
      .type = type,
      .phase_uid = phase_uid_val,
      .scene_uid = scene_uid_val,
      .name = std::move(cut_name),
      .rhs = cut.lowb,
      .scale = cut.scale,
      .row = row,
  };
  stored.coefficients.reserve(cut.cmap.size());
  for (const auto& [col, coeff] : cut.cmap) {
    stored.coefficients.emplace_back(col, coeff);
  }
  // Single source of truth: per-scene vector.  Phase access within a
  // scene is serial in the forward/backward pass, so this is a
  // single-writer operation — no lock needed even though multiple
  // scene workers may call store_cut concurrently (each touches its
  // own scene's vector).
  m_scene_cuts_[scene_index].push_back(std::move(stored));
}

// ── clear ──────────────────────────────────────────────────────────────────

void SDDPCutStore::clear()
{
  for (auto& sc : m_scene_cuts_) {
    sc.clear();
  }
}

// ── forget_first_cuts ──────────────────────────────────────────────────────

void SDDPCutStore::forget_first_cuts(std::ptrdiff_t count,
                                     PlanningLP& planning_lp)
{
  if (count <= 0) {
    return;
  }

  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  using ScenePhaseKey = std::pair<SceneIndex, PhaseIndex>;

  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase_uid);
    auto sit = scene_map.find(cut.scene_uid);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return std::nullopt;
    }
    return ScenePhaseKey {sit->second, pit->second};
  };

  std::map<ScenePhaseKey, std::vector<int>> rows_to_delete;
  std::set<std::string> names_to_forget;

  // `count` is a per-scene cap: forget the first `count` cuts of each
  // scene (covering inherited cuts transferred across cascade levels,
  // which are prepended to each per-scene vector in the same order).
  std::ptrdiff_t n = 0;
  for (auto& cuts : m_scene_cuts_) {
    const auto take = std::min(count, std::ssize(cuts));
    for (const auto& cut : cuts | std::views::take(take)) {
      if (!cut.name.empty()) {
        names_to_forget.insert(cut.name);
      }
      if (auto key = resolve_key(cut)) {
        rows_to_delete[*key].push_back(static_cast<int>(cut.row));
      }
    }
    cuts.erase(cuts.begin(), cuts.begin() + take);
    n = std::max(n, take);
  }

  std::map<ScenePhaseKey, int> deleted_count;
  int total_deleted = 0;
  for (auto& [key, rows] : rows_to_delete) {
    auto& sys = planning_lp.system(key.first, key.second);
    auto& li = sys.linear_interface();
    std::ranges::sort(rows);
    li.delete_rows(rows);
    sys.record_cut_deletion(rows);
    const auto n = static_cast<int>(std::ssize(rows));
    deleted_count[key] = n;
    total_deleted += n;
  }

  auto shift_row = [&](StoredCut& cut)
  {
    if (auto key = resolve_key(cut)) {
      if (auto it = deleted_count.find(*key); it != deleted_count.end()) {
        cut.row -= RowIndex {it->second};
      }
    }
  };

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

void SDDPCutStore::update_stored_cut_duals(PlanningLP& planning_lp)
{
  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  auto update_dual = [&](StoredCut& cut)
  {
    auto pit = phase_map.find(cut.phase_uid);
    auto sit = scene_map.find(cut.scene_uid);
    if (pit == phase_map.end() || sit == scene_map.end()) {
      return;
    }
    auto& li = planning_lp.system(sit->second, pit->second).linear_interface();
    const auto row_idx = static_cast<std::size_t>(cut.row);
    const auto duals = li.get_row_dual_raw();
    if (row_idx < duals.size()) {
      cut.dual = duals[row_idx];
    }
  };

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

  for (auto&& [scene_index, scene_states] :
       enumerate<SceneIndex>(scene_phase_states))
  {
    for (auto&& [phase_index, psi] : enumerate<PhaseIndex>(scene_states)) {
      auto& sys = planning_lp.system(scene_index, phase_index);
      auto& li = sys.linear_interface();

      const auto total_rows = static_cast<Index>(li.get_numrows());
      const auto base = static_cast<Index>(psi.base_nrows);
      const auto num_cut_rows = total_rows - base;
      if (num_cut_rows <= max_cuts) {
        continue;
      }

      const auto duals = li.get_row_dual_raw();
      if (duals.empty()) {
        continue;  // solution vectors released (low-memory mode)
      }

      struct CutInfo
      {
        int row;
        double abs_dual;
      };
      std::vector<CutInfo> cut_infos;
      cut_infos.reserve(total_rows - base);
      for (auto r = base; r < total_rows; ++r) {
        if (static_cast<std::size_t>(r) >= duals.size()) {
          break;
        }
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
          planning_lp.simulation().uid_of(scene_index),
          planning_lp.simulation().uid_of(phase_index),
          num_cut_rows,
          max_cuts);

      li.delete_rows(rows_to_delete);
      sys.record_cut_deletion(rows_to_delete);
      total_pruned += static_cast<int>(std::ssize(rows_to_delete));
      all_deleted[{scene_index, phase_index}] = std::move(rows_to_delete);
    }
  }

  if (total_pruned == 0) {
    return;
  }

  const auto phase_map = build_phase_uid_map(planning_lp);
  const auto scene_map = build_scene_uid_map(planning_lp);

  auto resolve_key = [&](const StoredCut& cut) -> std::optional<ScenePhaseKey>
  {
    auto pit = phase_map.find(cut.phase_uid);
    auto sit = scene_map.find(cut.scene_uid);
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
  { return std::ranges::binary_search(deleted, Index {row}); };

  auto compute_shift = [](const std::vector<Index>& deleted,
                          RowIndex row) -> RowIndex
  {
    return RowIndex {static_cast<Index>(
        std::ranges::lower_bound(deleted, Index {row} + 1) - deleted.begin())};
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

  const auto num_scenes = planning_lp.simulation().scene_count();
  int total_dropped = 0;

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    auto& cuts = m_scene_cuts_[scene_index];
    if (cuts.size() > limit) {
      const auto excess = cuts.size() - limit;
      cuts.erase(cuts.begin(),
                 cuts.begin() + static_cast<std::ptrdiff_t>(excess));
      total_dropped += static_cast<int>(excess);
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
  const auto num_scenes = planning_lp.simulation().scene_count();
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    const auto& cuts = m_scene_cuts_[scene_index];
    combined.insert(combined.end(), cuts.begin(), cuts.end());
  }
  return combined;
}

// ── apply_cut_sharing_for_iteration ────────────────────────────────────────

void SDDPCutStore::apply_cut_sharing_for_iteration(
    IterationIndex iteration_index,
    const SDDPOptions& options,
    PlanningLP& planning_lp,
    [[maybe_unused]] const LabelMaker& label_maker)
{
  const auto num_scenes = planning_lp.simulation().scene_count();
  const auto num_phases = planning_lp.simulation().phase_count();

  if (options.cut_sharing == CutSharingMode::none || num_scenes <= 1) {
    return;
  }

  // ``to_sparse_row`` populates the row's label metadata so that
  // ``LinearInterface::generate_labels_from_maps`` can synthesise a
  // unique LP-row name for the shared cut after it lands in the
  // destination scene's LP.  Without ``class_name`` /
  // ``constraint_name`` set the labeller throws
  // ``"row N has metadata without a class_name (unlabelable)"`` (juan/
  // gtopt_iplp iter-1 reproducer at row 5251).  ``variable_uid`` and
  // ``context`` carry the originating cut's identity so two shared
  // cuts on the same destination phase but from different source
  // scenes do not collide on the duplicate-label check at
  // ``generate_labels_from_maps:1581``.
  auto to_sparse_row = [iteration_index](const StoredCut& sc,
                                         int extra) -> SparseRow
  {
    auto row = SparseRow {
        .lowb = sc.rhs,
        .uppb = LinearProblem::DblMax,
        .scale = sc.scale,
        .class_name = sddp_loaded_cut_class_name,
        .constraint_name = sddp_loaded_cut_constraint_name,
        .variable_uid = sc.phase_uid,
        .context = make_iteration_context(
            sc.scene_uid, sc.phase_uid, uid_of(iteration_index), extra),
    };
    for (const auto& [col, coeff] : sc.coefficients) {
      row[col] = coeff;
    }
    return row;
  };

  const auto& phases = planning_lp.simulation().phases();
  auto phase_uid_fn = [&](PhaseIndex pi) -> PhaseUid
  { return phases[pi].uid(); };

  // Iterate each phase step; for each scene, use its per-scene offset
  // (captured by the caller in `m_scene_cuts_before_` before the
  // backward pass) to find cuts newly added this iteration.  Only
  // optimality cuts are shared across scenes (feasibility cuts are
  // scene-local by construction).
  for (const auto pi : iota_range<PhaseIndex>(0, num_phases - 1)) {
    StrongIndexVector<SceneIndex, std::vector<SparseRow>> per_scene_cuts;
    per_scene_cuts.resize(num_scenes);

    const auto pi_uid = phase_uid_fn(pi);

    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      const auto& cuts = m_scene_cuts_[si];
      const auto si_sz = static_cast<std::size_t>(si);
      const auto offset =
          si_sz < m_scene_cuts_before_.size() ? m_scene_cuts_before_[si_sz] : 0;
      // ``int`` matches ``make_iteration_context``'s ``extra`` slot, so
      // ``ci`` flows straight through ``to_sparse_row`` with no cast.
      // ``std::ssize(cuts)`` returns a signed type so the range-end
      // doesn't trigger a sign-conversion narrowing.
      for (const auto ci : iota_range<int>(offset, std::ssize(cuts))) {
        const auto& sc = cuts[ci];
        if (sc.type != CutType::Optimality || sc.phase_uid != pi_uid) {
          continue;
        }
        per_scene_cuts[si].push_back(to_sparse_row(sc, ci));
      }
    }

    // ``share_cuts_for_phase`` builds a per-scene
    // ``IterationContext`` internally (using the destination scene's
    // UID) so the accumulated/expected shared cut row carries
    // unique label metadata in every destination scene's LP — see
    // its ``stamp_for_scene`` helper.  Without that, the new
    // SparseRow synthesised by ``accumulate_benders_cuts`` /
    // ``average_benders_cut`` would land in the LP with empty
    // ``class_name`` and ``generate_labels_from_maps`` would
    // throw "row N has metadata without a class_name
    // (unlabelable)".
    gtopt::share_cuts_for_phase(
        pi, per_scene_cuts, options.cut_sharing, planning_lp, iteration_index);
  }
}

// ── save_cuts_for_iteration ────────────────────────────────────────────────

void SDDPCutStore::save_cuts_for_iteration(
    IterationIndex iteration_index,
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
    // Always serialise from the per-scene union so feasibility cuts
    // (which live exclusively in `m_scene_cuts_` to avoid the
    // combined-vector mutex) are included regardless of the
    // `single_cut_storage` flag.  Under `single_cut_storage=false`
    // optimality cuts also live in `m_scene_cuts_` alongside the
    // combined vector, so the union view via `build_combined_cuts`
    // contains both types without duplication.
    const auto combined = build_combined_cuts(planning_lp);
    return save_cuts_csv(combined, planning_lp, filepath);
  };

  // Save to a versioned file: sddp_cuts_<iter>.csv
  if (!cut_dir.empty()) {
    const auto versioned_file =
        (cut_dir / std::format(sddp_file::versioned_cuts_fmt, iteration_index))
            .string();
    auto result = save_all(versioned_file);
    if (!result.has_value()) {
      SPDLOG_WARN("SDDP: could not save versioned cuts at iter {}: {}",
                  iteration_index,
                  result.error().message);
    }
  }

  // Save per-scene cuts
  if (!cut_dir.empty()) {
    const auto num_scenes = planning_lp.simulation().scene_count();
    const auto& scenes = planning_lp.simulation().scenes();
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto result = save_scene_cuts_csv(m_scene_cuts_[scene_index],
                                        scene_index,
                                        scenes[scene_index].uid(),
                                        planning_lp,
                                        cut_dir.string());
      if (!result.has_value()) {
        SPDLOG_WARN("SDDP: could not save per-scene cuts at iter {}: {}",
                    iteration_index,
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
                  iteration_index,
                  sr.error().message);
    }
    const auto versioned_state =
        (cut_dir / std::format(sddp_file::versioned_state_fmt, iteration_index))
            .string();
    sr = save_state_csv(
        planning_lp, versioned_state, IterationIndex {current_iteration});
    if (!sr.has_value()) {
      SPDLOG_WARN("SDDP: could not save versioned state at iter {}: {}",
                  iteration_index,
                  sr.error().message);
    }
  }

  // Rename cut files for infeasible scenes
  const auto num_scenes = planning_lp.simulation().scene_count();
  const auto& scenes = planning_lp.simulation().scenes();
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    if (scene_feasible[scene_index] != 0U) {
      continue;
    }
    if (cut_dir.empty()) {
      continue;
    }
    const auto suid = scenes[scene_index].uid();
    const auto scene_file =
        cut_dir / std::format(sddp_file::scene_cuts_fmt, suid);
    const auto error_file =
        cut_dir / std::format(sddp_file::error_scene_cuts_fmt, suid);
    std::error_code ec;
    if (std::filesystem::exists(scene_file, ec)) {
      std::filesystem::rename(scene_file, error_file, ec);
      if (!ec) {
        SPDLOG_TRACE("SDDP: renamed cut file for infeasible scene {} to {}",
                     scene_index,
                     error_file.string());
      }
    }
  }
}

}  // namespace gtopt
