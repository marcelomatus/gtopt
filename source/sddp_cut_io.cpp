/**
 * @file      sddp_cut_io.cpp
 * @brief     Cut persistence (save/load) for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut save/load free functions declared in sddp_cut_io.hpp.
 * Extracted from sddp_solver.cpp to reduce file size and improve
 * modularity.  All functions take explicit parameters rather than
 * accessing class members.
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

#include <daw/json/daw_json_link.h>
#include <gtopt/as_label.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/json/json_sddp_cut_io.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── UID lookup helpers ─────────────────────────────────────────────────────

[[nodiscard]] auto build_phase_uid_map(const PlanningLP& planning_lp)
    -> flat_map<PhaseUid, PhaseIndex>
{
  const auto& phases = planning_lp.simulation().phases();
  flat_map<PhaseUid, PhaseIndex> phase_map;
  map_reserve(phase_map, phases.size());
  for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
    phase_map.emplace(phase.uid(), pi);
  }
  return phase_map;
}

[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>
{
  const auto& scenes = planning_lp.simulation().scenes();
  flat_map<SceneUid, SceneIndex> scene_map;
  map_reserve(scene_map, scenes.size());
  for (auto&& [si, scene] : enumerate<SceneIndex>(scenes)) {
    scene_map.emplace(scene.uid(), si);
  }
  return scene_map;
}

// ─── Helper: state-variable name matching ───────────────────────────────────

// ─── Auto-scale alpha helper ────────────────────────────────────────────────

[[nodiscard]] auto effective_scale_alpha(const PlanningLP& planning_lp,
                                         double option_scale_alpha) -> double
{
  if (option_scale_alpha > 0.0) {
    return option_scale_alpha;
  }
  // Mirror the auto-scale heuristic in SDDPMethod::initialize_solver():
  //   scale_alpha = max(state.var_scale)
  // across every (scene, phase) state-variable cell.
  const auto& sim = planning_lp.simulation();
  double max_vs = 1.0;
  for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
      for (const auto& [key, svar] : sim.state_variables(si, pi)) {
        max_vs = std::max(max_vs, svar.var_scale());
      }
    }
  }
  return max_vs;
}
// ─── Format-dispatching functions ──────────────────────────────────────────

[[nodiscard]] auto save_cuts(std::span<const StoredCut> cuts,
                             const PlanningLP& planning_lp,
                             const std::string& filepath,
                             CutIOFormat format,
                             bool append_mode) -> std::expected<void, Error>
{
  if (format == CutIOFormat::json) {
    // JSON does not support append mode — always overwrites
    return save_cuts_json(cuts, planning_lp, filepath);
  }
  // CutIOFormat::parquet (default and only other variant).
  return save_cuts_parquet(cuts, planning_lp, filepath, append_mode);
}

[[nodiscard]] auto load_cuts(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    CutIOFormat format,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  // Determine file paths for both supported formats based on the given
  // filepath stem.  The dispatcher routes to parquet (default) or json;
  // the legacy CSV reader was removed in the Phase 1.3 cleanup.
  const auto path = std::filesystem::path(filepath);
  const auto stem = path.stem().string();
  const auto parent = path.parent_path();

  const auto json_path = (parent / (stem + ".json")).string();
  const auto parquet_path = (parent / (stem + ".parquet")).string();

  const auto try_parquet = [&]() -> std::expected<CutLoadResult, Error>
  {
    return load_cuts_parquet(planning_lp,
                             parquet_path,
                             scale_alpha,
                             label_maker,
                             scene_phase_states);
  };
  const auto try_json = [&]() -> std::expected<CutLoadResult, Error>
  {
    return load_cuts_json(
        planning_lp, json_path, scale_alpha, scene_phase_states);
  };

  // For Parquet, "exists" must account for split-file append: a directory
  // with only `<stem>.append-*.parquet` is a valid Parquet cut set even
  // if `<stem>.parquet` itself is missing.
  const auto parquet_present = [&]
  {
    if (std::filesystem::exists(parquet_path)) {
      return true;
    }
    if (!std::filesystem::exists(parent)) {
      return false;
    }
    const auto prefix = stem + ".append-";
    return std::ranges::any_of(
        std::filesystem::directory_iterator(parent),
        [&](const auto& entry)
        {
          const auto name = entry.path().filename().string();
          return name.starts_with(prefix) && name.ends_with(".parquet");
        });
  };

  if (format == CutIOFormat::json) {
    if (std::filesystem::exists(json_path)) {
      return try_json();
    }
    if (parquet_present()) {
      SPDLOG_INFO(
          "SDDP load_cuts: JSON file not found, falling back to Parquet: {}",
          parquet_path);
      return try_parquet();
    }
  } else {  // parquet (default and only other variant)
    if (parquet_present()) {
      return try_parquet();
    }
    if (std::filesystem::exists(json_path)) {
      SPDLOG_INFO(
          "SDDP load_cuts: Parquet file not found, falling back to JSON: {}",
          json_path);
      return try_json();
    }
  }

  return std::unexpected(Error {
      .code = ErrorCode::FileIOError,
      .message = std::format("Cannot find cut file in any format: {} or {}",
                             parquet_path,
                             json_path),
  });
}

// ``extract_iteration_from_name`` was removed in 2026-05.  Every
// consumer now reads the iteration index directly from the matching
// struct field (``StoredCut::iteration_index``, ``CutEntry::iteration``,
// ``RawBoundaryCut::iteration_index``) rather than parsing it back out
// of a generated row label.  See:
//   * ``sddp_cut_parquet.cpp::load_cuts_parquet`` — reads the
//     ``iteration`` int32 column directly.
//   * ``sddp_cut_json.cpp`` — uses ``CutEntry.iteration``.
//   * ``sddp_boundary_cuts.cpp`` / ``sddp_named_cuts.cpp`` — both
//     parsers populate ``iteration_index`` while reading CSV rows
//     and reuse that variable downstream.

}  // namespace gtopt
