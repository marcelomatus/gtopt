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
  return save_cuts_csv(cuts, planning_lp, filepath, append_mode);
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
  // Determine file paths for both formats based on the given filepath
  const auto path = std::filesystem::path(filepath);
  const auto stem = path.stem().string();
  const auto parent = path.parent_path();

  const auto csv_path = (parent / (stem + ".csv")).string();
  const auto json_path = (parent / (stem + ".json")).string();

  // Try preferred format first, fall back to the other
  if (format == CutIOFormat::json) {
    if (std::filesystem::exists(json_path)) {
      return load_cuts_json(
          planning_lp, json_path, scale_alpha, scene_phase_states);
    }
    if (std::filesystem::exists(csv_path)) {
      SPDLOG_INFO(
          "SDDP load_cuts: JSON file not found, falling back to CSV: {}",
          csv_path);
      return load_cuts_csv(
          planning_lp, csv_path, scale_alpha, label_maker, scene_phase_states);
    }
  } else {
    if (std::filesystem::exists(csv_path)) {
      return load_cuts_csv(
          planning_lp, csv_path, scale_alpha, label_maker, scene_phase_states);
    }
    if (std::filesystem::exists(json_path)) {
      SPDLOG_INFO(
          "SDDP load_cuts: CSV file not found, falling back to JSON: {}",
          json_path);
      return load_cuts_json(
          planning_lp, json_path, scale_alpha, scene_phase_states);
    }
  }

  // Neither format exists
  return std::unexpected(Error {
      .code = ErrorCode::FileIOError,
      .message = std::format(
          "Cannot find cut file in any format: {} or {}", csv_path, json_path),
  });
}

// ─── Public free function (used across CSV / JSON loaders) ─────────────────

// ``extract_iteration_from_name`` lives in namespace ``gtopt`` (not
// the anonymous namespace) so the unit test in
// ``test/source/test_sddp_cut_io.cpp`` can exercise it without having
// to reach into a translation-unit-private symbol.  The function is
// declared in ``sddp_cut_io.hpp``; the comment block on the
// declaration documents the on-disk format and return-value
// semantics.
auto extract_iteration_from_name(std::string_view name) -> IterationIndex
{
  // scut, fcut, and bcut encode the iteration; ecut does not.
  if (!name.starts_with("sddp_scut_") && !name.starts_with("sddp_fcut_")
      && !name.starts_with("sddp_bcut_"))
  {
    return IterationIndex {0};
  }
  // Format: sddp_{type}_{uid}_{scene}_{phase}_{iteration}_{offset}
  //
  // Hardening (2026-04-25): every UID-bearing field
  // (variable_uid, scene_uid, phase_uid, iteration_uid) MUST be
  // a real positive identifier.  A negative value indicates the
  // ``unknown_uid`` (= -1) sentinel propagated from a row that
  // was emitted without a real UID attached — bad-formed cuts
  // that should never reach load time.  Modern emitters
  // (``make_iteration_context`` + the cut-name generators in
  // ``label_maker.cpp``) all produce positive UIDs; the
  // ``unknown_uid`` placeholder remains as a regression-guard
  // signal rather than a routine path.  We throw
  // ``std::invalid_argument`` so the upstream bug surfaces
  // directly instead of silently round-tripping the sentinel.
  //
  // Only ``offset`` (the 6th field) is allowed to be negative —
  // it is a free ``int`` discriminator, not a UID.  Scan the
  // first five fields for any ``-`` immediately after a ``_``.
  std::string_view::size_type pos = 0;
  for (int field = 0; field < 5; ++field) {
    pos = name.find('_', pos);
    if (pos == std::string_view::npos) {
      return IterationIndex {0};
    }
    ++pos;
    if (field >= 1 && pos < name.size() && name[pos] == '-') {
      throw std::invalid_argument(std::format(
          "extract_iteration_from_name: cut name '{}' has a negative "
          "UID at field {} (= unknown_uid sentinel — variable_uid, "
          "scene_uid, phase_uid, or iteration_uid) — bad-formed "
          "label, refusing to parse",
          name,
          field + 1));
    }
  }
  // ``pos`` now points at the start of the iteration field
  // (field index 5 in the 0-based count above; after 5 underscore
  // advances).  Take the substring up to the next underscore.
  if (pos >= name.size()) {
    return IterationIndex {0};
  }
  const auto end = name.find('_', pos);
  const auto token = name.substr(pos, end - pos);
  int result = 0;
  const auto* const first = token.data();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto* const last = first + token.size();
  auto [ptr, ec] = std::from_chars(first, last, result);
  return IterationIndex {(ec == std::errc {}) ? result : 0};
}

}  // namespace gtopt
