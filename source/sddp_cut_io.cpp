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

auto build_phase_uid_map(const PlanningLP& planning_lp)
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

auto build_scene_uid_map(const PlanningLP& planning_lp)
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

auto effective_scale_alpha(const PlanningLP& planning_lp,
                           double option_scale_alpha) -> double
{
  if (option_scale_alpha > 0.0) {
    return option_scale_alpha;
  }
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

namespace
{

/// Returns true if *col_name* is a final-state-variable name that can
/// appear in boundary/hot-start cut CSV headers (efin, soc, vfin).
[[nodiscard]] constexpr auto is_final_state_col(std::string_view col_name)
    -> bool
{
  return col_name == "efin" || col_name == "soc" || col_name == "vfin";
}

// ─── Structured key helpers ─────────────────────────────────────────────────

/// Compact reverse map from ColIndex to state variable identity.
struct ColKeyInfo
{
  std::string_view class_name;
  std::string_view col_name;
  Uid uid {unknown_uid};
};

using ColKeyMap = std::unordered_map<ColIndex, ColKeyInfo>;

/// Build a ColIndex → (class_name, col_name, uid) reverse map from the
/// state variables registered for a given (scene, phase).
[[nodiscard]] auto build_col_key_map(const SimulationLP& sim,
                                     SceneIndex si,
                                     PhaseIndex pi) -> ColKeyMap
{
  ColKeyMap map;
  map_reserve(map, sim.state_variables(si, pi).size());
  for (const auto& [key, svar] : sim.state_variables(si, pi)) {
    map.try_emplace(svar.col(),
                    ColKeyInfo {
                        .class_name = key.class_name,
                        .col_name = key.col_name,
                        .uid = key.uid,
                    });
  }
  return map;
}

/// Write a single cut's coefficients to an output stream.
///
/// Format: class:var:uid=coeff (structured key, portable without LP names).
/// Non-state-variable columns (alpha) are written as @alpha=coeff.
/// Coefficients are in fully physical space:
///   coeff_csv = LP_coeff * scale_obj / col_scale
void write_cut_coefficients(std::ostream& ofs,
                            const StoredCut& cut,
                            const LinearInterface& li,
                            const ColKeyMap& col_keys)
{
  const auto scale_obj = li.scale_objective();
  for (const auto& [col, coeff] : cut.coefficients) {
    const auto scale = li.get_col_scale(col);
    const auto phys_coeff = coeff * scale_obj / scale;
    if (auto it = col_keys.find(col); it != col_keys.end()) {
      const auto& [cls, var, uid] = it->second;
      ofs << "," << as_label<':'>(cls, var, uid) << "=" << phys_coeff;
    } else {
      // Non-state-variable column (typically alpha)
      ofs << ",@alpha=" << phys_coeff;
    }
  }
}

/// Write cut coefficients without variable scaling (fallback when
/// phase UID is not found).
void write_cut_coefficients_unscaled(std::ostream& ofs,
                                     const StoredCut& cut,
                                     double scale_obj)
{
  for (const auto& [col, coeff] : cut.coefficients) {
    ofs << "," << col << ":" << (coeff * scale_obj);
  }
}

/// Extract the iteration number from a cut name.
///
/// Cut name formats:
///   sddp_scut_{scene}_{phase}_{iteration}_{offset}  → field [4]
///   sddp_fcut_{scene}_{phase}_{iteration}_{offset}  → field [4]
///   sddp_ecut_{scene}_{phase}_{total_cuts}           → no iteration
///
/// Returns 0 if the iteration cannot be determined.
[[nodiscard]] auto extract_iteration_from_name(std::string_view name)
    -> IterationIndex
{
  // Only scut and fcut encode the iteration
  if (!name.starts_with("sddp_scut_") && !name.starts_with("sddp_fcut_")) {
    return IterationIndex {0};
  }
  // Split by '_' and take the 5th field (index 4)
  int field = 0;
  std::string_view::size_type pos = 0;
  while (pos < name.size() && field < 4) {
    pos = name.find('_', pos);
    if (pos == std::string_view::npos) {
      return IterationIndex {0};
    }
    ++pos;
    ++field;
  }
  if (field != 4 || pos >= name.size()) {
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

}  // namespace

// ─── Save functions ─────────────────────────────────────────────────────────

auto save_cuts_csv(std::span<const StoredCut> cuts,
                   const PlanningLP& planning_lp,
                   const std::string& filepath,
                   bool append_mode) -> std::expected<void, Error>
{
  try {
    // Ensure parent directory exists before writing
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto open_mode =
        append_mode ? (std::ios::out | std::ios::app) : std::ios::out;
    std::ofstream ofs(filepath, open_mode);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for writing: {}", filepath),
      });
    }

    // Get scale_objective from a representative LinearInterface.
    const auto& rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    if (!append_mode) {
      ofs << "# scale_objective=" << scale_obj << "\n";
      ofs << "type,phase,scene,name,rhs,dual,coefficients\n";
    }

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    // Build per-phase ColIndex → structured key reverse maps (cached).
    const auto& sim = planning_lp.simulation();
    std::unordered_map<PhaseIndex, ColKeyMap> phase_col_keys;
    map_reserve(phase_col_keys, phase_map.size());
    for (const auto& [uid, pi] : phase_map) {
      phase_col_keys.try_emplace(
          pi, build_col_key_map(sim, first_scene_index(), pi));
    }

    for (const auto& cut : cuts) {
      // Type column: 'o' = optimality, 'f' = feasibility
      const char type_char = (cut.type == CutType::Feasibility) ? 'f' : 'o';

      // RHS in physical objective units
      ofs << as_label<','>(type_char,
                           cut.phase_uid,
                           cut.scene_uid,
                           cut.name,
                           cut.rhs * scale_obj)
          << ",";
      if (cut.dual.has_value()) {
        ofs << *cut.dual;
      }

      // Look up the LinearInterface to retrieve column scales.
      // Use scene 0 as representative (scales are identical
      // across scenes because the LP structure is built
      // identically per phase).
      auto pit = phase_map.find(cut.phase_uid);
      if (pit != phase_map.end()) {
        const auto& li = planning_lp.system(first_scene_index(), pit->second)
                             .linear_interface();
        write_cut_coefficients(ofs, cut, li, phase_col_keys[pit->second]);
      } else {
        SPDLOG_WARN(
            "save_cuts: unknown phase UID {} for cut '{}'; "
            "writing without variable scaling",
            cut.phase_uid,
            cut.name);
        write_cut_coefficients_unscaled(ofs, cut, scale_obj);
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts to {}", cuts.size(), filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error saving cuts to {}: {}", filepath, e.what()),
    });
  }
}

auto save_scene_cuts_csv(std::span<const StoredCut> cuts,
                         SceneIndex scene_index,
                         SceneUid scene_uid,
                         const PlanningLP& planning_lp,
                         const std::string& directory)
    -> std::expected<void, Error>
{
  try {
    std::filesystem::create_directories(directory);

    const auto filepath = (std::filesystem::path(directory)
                           / std::format(sddp_file::scene_cuts_fmt, scene_uid))
                              .string();

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open scene cut file for writing: {}",
                                 filepath),
      });
    }

    // Get scale_objective from a representative LinearInterface.
    const auto& rep_li =
        planning_lp.system(scene_index, first_phase_index()).linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    ofs << "# scale_objective=" << scale_obj << "\n";
    ofs << "type,phase,scene,name,rhs,dual,coefficients\n";

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    // Build per-phase ColIndex → structured key reverse maps (cached).
    const auto& sim = planning_lp.simulation();
    std::unordered_map<PhaseIndex, ColKeyMap> phase_col_keys;
    map_reserve(phase_col_keys, phase_map.size());
    for (const auto& [uid, pi] : phase_map) {
      phase_col_keys.try_emplace(pi, build_col_key_map(sim, scene_index, pi));
    }

    for (const auto& cut : cuts) {
      // Type column: 'o' = optimality, 'f' = feasibility
      const char type_char = (cut.type == CutType::Feasibility) ? 'f' : 'o';

      // RHS in physical objective units
      ofs << as_label<','>(type_char,
                           cut.phase_uid,
                           cut.scene_uid,
                           cut.name,
                           cut.rhs * scale_obj)
          << ",";
      if (cut.dual.has_value()) {
        ofs << *cut.dual;
      }

      auto pit = phase_map.find(cut.phase_uid);
      if (pit != phase_map.end()) {
        const auto& li =
            planning_lp.system(scene_index, pit->second).linear_interface();
        write_cut_coefficients(ofs, cut, li, phase_col_keys[pit->second]);
      } else {
        SPDLOG_WARN(
            "save_scene_cuts: unknown phase UID {} for cut "
            "'{}'; writing without variable scaling",
            cut.phase_uid,
            cut.name);
        write_cut_coefficients_unscaled(ofs, cut, scale_obj);
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts for scene UID {} to {}",
                 cuts.size(),
                 scene_uid,
                 filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("Error saving scene UID {} cuts to {}: {}",
                               scene_uid,
                               directory,
                               e.what()),
    });
  }
}

// ─── Load functions ─────────────────────────────────────────────────────────

auto load_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    [[maybe_unused]] const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  const auto sa = scale_alpha;  // row scale for loaded cuts
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for reading: {}", filepath),
      });
    }

    std::string line;
    // Skip metadata comments (# ...) and find the CSV header line
    bool has_type_col = false;
    while (std::getline(ifs, line)) {
      strip_cr(line);
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      // First non-empty, non-comment line is the header.
      // Detect new format with type column: "type,phase,..."
      // vs legacy format: "phase,scene,..."
      has_type_col = line.starts_with("type,");
      break;
    }

    // Detect if header includes a dual column
    const bool has_dual_col = line.contains(",dual,");

    CutLoadResult result {};
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto& rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    // Build phase UID -> PhaseIndex lookup
    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Track (phase_uid, cut_name) pairs already loaded.
    // Cuts are broadcast to all scenes, so if the CSV contains
    // the same cut for multiple scenes we must load it only once.
    std::set<std::pair<PhaseUid, std::string>> loaded_keys;

    // Process data lines
    int line_num = 1;  // header was line 1
    while (std::getline(ifs, line)) {
      strip_cr(line);
      ++line_num;
      if (line.empty() || line.starts_with('#')) {
        continue;
      }

      // Parse CSV: [type,]phase,scene,name,rhs,col1:coeff1,...
      std::istringstream iss(line);
      std::string token;

      // Parse optional type column (backward compatible).
      // The type is tracked for future use but currently loaded cuts
      // are added to the LP directly regardless of type.
      [[maybe_unused]] CutType cut_type = CutType::Optimality;
      if (has_type_col) {
        std::getline(iss, token, ',');
        if (token == "f") {
          cut_type = CutType::Feasibility;
        }
      }

      if (!std::getline(iss, token, ',') || token.empty()) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "missing phase column; skipping",
            line_num,
            filepath);
        continue;
      }
      PhaseUid phase_uid {};
      try {
        phase_uid = make_uid<Phase>(std::stoi(token));
      } catch (const std::exception&) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "invalid phase '{}'; skipping",
            line_num,
            filepath,
            token);
        continue;
      }

      if (!std::getline(iss, token, ',')) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "missing scene column; skipping",
            line_num,
            filepath);
        continue;
      }
      // scene is parsed but intentionally ignored: loaded cuts
      // are broadcast to all scenes as warm-start approximations.
      [[maybe_unused]] SceneUid scene_uid {};
      try {
        scene_uid = make_uid<Scene>(std::stoi(token));
      } catch (const std::exception&) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "invalid scene '{}'; skipping",
            line_num,
            filepath,
            token);
        continue;
      }

      if (!std::getline(iss, token, ',') || token.empty()) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "missing name column; skipping",
            line_num,
            filepath);
        continue;
      }
      const auto cut_name = token;

      // Skip duplicate (phase, name) pairs — these arise when
      // the CSV was saved with per-scene rows but loading
      // broadcasts each cut to every scene.
      if (!loaded_keys.emplace(phase_uid, cut_name).second) {
        continue;
      }

      // Track the highest iteration found for iteration offset
      result.max_iteration =
          std::max(result.max_iteration, extract_iteration_from_name(cut_name));

      if (!std::getline(iss, token, ',') || token.empty()) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "missing rhs for cut '{}'; skipping",
            line_num,
            filepath,
            cut_name);
        continue;
      }
      double rhs = 0.0;
      try {
        rhs = std::stod(token);
      } catch (const std::exception&) {
        SPDLOG_WARN(
            "SDDP load_cuts: malformed line {} in {}: "
            "invalid rhs '{}' for cut '{}'; skipping",
            line_num,
            filepath,
            token,
            cut_name);
        continue;
      }

      // Parse optional dual column (backward compatible).
      // The dual is informational — it is not used when loading
      // cuts into the LP, but skipping the column is required
      // to correctly parse the remaining coefficient fields.
      if (has_dual_col) {
        std::getline(iss, token, ',');  // consume dual field
      }

      // RHS in CSV is in physical objective units; convert to LP
      // space.  Row scale = scale_alpha so the cut is consistent with
      // optimality/feasibility cuts built by build_benders_cut().
      auto row = SparseRow {
          .lowb = rhs / scale_obj,
          .uppb = LinearProblem::DblMax,
          .scale = sa,
          .class_name = "Loaded",
          .constraint_name = "cut",
      };

      // Resolve the phase UID to a PhaseIndex
      auto pit = phase_uid_to_index.find(phase_uid);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "SDDP load_cuts: unknown phase UID {} in {}, "
            "skipping cut '{}'",
            phase_uid,
            filepath,
            cut_name);
        continue;
      }
      const auto phase_index = pit->second;

      // Use scene 0 as representative for column resolution
      // (LP structure is identical across scenes).
      auto& li_ref = planning_lp.system(first_scene_index(), phase_index)
                         .linear_interface();

      // State variable map for structured key resolution.
      const auto& sv_map = planning_lp.simulation().state_variables(
          first_scene_index(), phase_index);

      // Collect coefficients from CSV.  Three formats are supported:
      //   class:var:uid=coeff  (structured key — preferred, no LP names)
      //   @alpha=coeff         (alpha column marker)
      //   name=coeff           (legacy name-based — resolve by column name)
      //   index:coeff          (legacy — validate index against current LP)
      struct ResolvedCoeff
      {
        ColIndex col;
        double coeff;
      };
      std::vector<ResolvedCoeff> resolved_coeffs;
      bool cut_valid = true;

      while (std::getline(iss, token, ',')) {
        if (const auto eq = token.find('='); eq != std::string::npos) {
          const auto key_part = token.substr(0, eq);
          const auto coeff = std::stod(token.substr(eq + 1));

          if (key_part == "@alpha") {
            // Alpha column — resolve via scene_phase_states if available
            if (scene_phase_states != nullptr) {
              const auto& states = (*scene_phase_states)[first_scene_index()];
              if (phase_index < PhaseIndex {states.size()}) {
                const auto alpha_col = states[phase_index].alpha_col;
                if (alpha_col != ColIndex {unknown_index}) {
                  resolved_coeffs.push_back({
                      .col = alpha_col,
                      .coeff = coeff,
                  });
                }
              }
            } else {
              // No alpha info — try legacy name-based resolution
              const auto& name_map = li_ref.col_name_map();
              for (const auto& [nm, idx] : name_map) {
                if (nm.starts_with("sddp_alpha_")) {
                  resolved_coeffs.push_back({
                      .col = idx,
                      .coeff = coeff,
                  });
                  break;
                }
              }
            }
          } else if (std::ranges::count(key_part, ':') == 2) {
            // Structured key format: class:var:uid
            const auto c1 = key_part.find(':');
            const auto c2 = key_part.find(':', c1 + 1);
            const auto cls = key_part.substr(0, c1);
            const auto var = key_part.substr(c1 + 1, c2 - c1 - 1);
            const auto uid_str = key_part.substr(c2 + 1);
            int uid_val = 0;
            auto [ptr, ec] =
                std::from_chars(uid_str.data(),
                                uid_str.data() + uid_str.size(),  // NOLINT
                                uid_val);
            if (ec != std::errc {}) {
              SPDLOG_WARN(
                  "SDDP load_cuts: invalid uid '{}' in "
                  "structured key for cut '{}'; skipping coeff",
                  uid_str,
                  cut_name);
              continue;
            }
            bool found = false;
            for (const auto& [key, svar] : sv_map) {
              if (key.class_name == cls && key.col_name == var
                  && key.uid == Uid {uid_val})
              {
                resolved_coeffs.push_back({
                    .col = svar.col(),
                    .coeff = coeff,
                });
                found = true;
                break;
              }
            }
            if (!found) {
              SPDLOG_WARN(
                  "SDDP load_cuts: structured key '{}' not "
                  "found in state variables for cut '{}'; "
                  "ignoring coefficient",
                  key_part,
                  cut_name);
            }
          } else {
            // Legacy name-based format: name=coeff
            const auto& name_map = li_ref.col_name_map();
            auto it = name_map.find(key_part);
            if (it != name_map.end()) {
              resolved_coeffs.push_back({
                  .col = it->second,
                  .coeff = coeff,
              });
            } else {
              SPDLOG_WARN(
                  "SDDP load_cuts: column '{}' not found in "
                  "current LP for cut '{}'; ignoring coefficient",
                  key_part,
                  cut_name);
            }
          }
        }
        // Legacy format (index:coeff)
        else if (const auto colon = token.find(':'); colon != std::string::npos)
        {
          const auto file_col = std::stoi(token.substr(0, colon));
          const auto coeff = std::stod(token.substr(colon + 1));

          // Validate the column index against the current LP
          if (file_col < 0
              || static_cast<size_t>(file_col) >= li_ref.get_numcols())
          {
            SPDLOG_WARN(
                "SDDP load_cuts: column index {} out of range "
                "(LP has {} cols) in cut '{}'; skipping cut",
                file_col,
                li_ref.get_numcols(),
                cut_name);
            cut_valid = false;
            break;
          }

          const auto& idx_names = li_ref.col_index_to_name();
          const auto fc = ColIndex {file_col};
          if (static_cast<size_t>(file_col) < idx_names.size()
              && !idx_names[fc].empty())
          {
            SPDLOG_DEBUG(
                "SDDP load_cuts: legacy index-based coefficient "
                "col={} (name='{}') in cut '{}'; consider "
                "re-saving cuts with named format",
                file_col,
                idx_names[fc],
                cut_name);
          }
          resolved_coeffs.push_back({
              .col = fc,
              .coeff = coeff,
          });
        }
      }

      if (!cut_valid) {
        continue;
      }

      // Add the resolved cut to all scenes for this phase.
      // Coefficients in the CSV are in fully physical space;
      // convert to LP space:
      //   LP_coeff = phys_coeff * col_scale / scale_objective
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        auto scene_row = row;
        scene_row.context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                               sim.phases()[phase_index].uid()};
        for (const auto& [col, coeff] : resolved_coeffs) {
          const auto scale = li.get_col_scale(col);
          scene_row[col] = coeff * scale / scale_obj;
        }
        li.add_row(scene_row);
      }
      ++result.count;
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {}", result.count, filepath);
    return result;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error loading cuts from {}: {}", filepath, e.what()),
    });
  }
}

auto load_scene_cuts_from_directory(
    PlanningLP& planning_lp,
    const std::string& directory,
    double scale_alpha,
    [[maybe_unused]] const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  CutLoadResult total {};

  if (!std::filesystem::exists(directory)) {
    return total;  // No directory = no cuts to load (not an error)
  }

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto filename = entry.path().filename().string();

    // Skip error files from infeasible scenes (previous runs)
    if (filename.starts_with("error_")) {
      SPDLOG_INFO("SDDP hot-start: skipping error file {}", filename);
      continue;
    }

    // Only load scene_N.csv files and the combined sddp_cuts.csv
    if (!filename.starts_with("scene_") && filename != sddp_file::combined_cuts)
    {
      continue;
    }
    if (!filename.ends_with(".csv")) {
      continue;
    }

    auto result = load_cuts_csv(planning_lp,
                                entry.path().string(),
                                scale_alpha,
                                label_maker,
                                scene_phase_states);
    if (result.has_value()) {
      total.count += result->count;
      total.max_iteration =
          std::max(total.max_iteration, result->max_iteration);
      SPDLOG_TRACE(
          "SDDP hot-start: loaded {} cuts from {}", result->count, filename);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load {}: {}",
                  filename,
                  result.error().message);
    }
  }

  return total;
}

// ─── Boundary (future-cost) cuts ────────────────────────────────────────────

auto load_boundary_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    [[maybe_unused]] const LabelMaker& label_maker,
    StrongIndexVector<SceneIndex,
                      StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  // ── Mode check ────────────────────────────────────────────────
  const auto mode = options.boundary_cuts_mode;
  if (mode == BoundaryCutsMode::noload) {
    SPDLOG_INFO("SDDP: boundary cuts mode is 'noload' -- skipping");
    return CutLoadResult {};
  }

  const bool separated = (mode == BoundaryCutsMode::separated);

  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format(
              "Cannot open boundary cuts file for reading: {}", filepath),
      });
    }

    // ── Parse header ────────────────────────────────────────────
    // Expected: name,iteration,scene,rhs,StateVar1,StateVar2,...
    // (Legacy format without iteration column is auto-detected.)
    std::string header_line;
    std::getline(ifs, header_line);
    strip_cr(header_line);

    std::vector<std::string> headers;
    {
      std::istringstream hss(header_line);
      std::string token;
      while (std::getline(hss, token, ',')) {
        headers.push_back(token);
      }
    }

    // Detect whether the CSV has the `iteration` column.
    const bool has_iteration_col =
        (headers.size() >= 2 && headers[1] == "iteration");
    const int state_var_start = has_iteration_col ? 4 : 3;

    if (static_cast<int>(headers.size()) < state_var_start + 1) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Boundary cuts CSV must have at least {} columns "
                          "(name,[iteration,]scene,rhs,<state_vars>); got {}",
                          state_var_start + 1,
                          headers.size()),
      });
    }

    // ── Determine last phase and build name->column mapping ─────
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto last_phase = sim.last_phase_index();

    // Build scene UID -> SceneIndex lookup (for "separated" mode)
    std::unordered_map<SceneUid, SceneIndex, std::hash<SceneUid>>
        scene_uid_to_index;
    map_reserve(scene_uid_to_index, static_cast<std::size_t>(num_scenes));
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      scene_uid_to_index[sim.scenes()[si].uid()] = si;
    }

    // Build element-name -> uid lookup from the System.
    const auto& sys = planning_lp.planning().system;

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    map_reserve(name_to_class_uid,
                sys.junction_array.size() + sys.battery_array.size());
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }

    // For each state-variable header column, find the
    // corresponding LP column in the last phase.
    const auto& svar_map = sim.state_variables(first_scene_index(), last_phase);

    const auto num_state_cols =
        static_cast<int>(headers.size()) - state_var_start;
    std::vector<std::optional<ColIndex>> header_col_map;
    header_col_map.reserve(num_state_cols);
    std::vector<std::string> header_names;
    header_names.reserve(num_state_cols);

    for (auto hi = static_cast<std::size_t>(state_var_start);
         hi < headers.size();
         ++hi)
    {
      const auto& hdr = headers[hi];
      std::optional<ColIndex> found_col;

      // Parse "ClassName:ElementName" or plain "ElementName"
      std::string_view class_filter;
      std::string element_name;
      if (const auto colon = hdr.find(':'); colon != std::string::npos) {
        class_filter = std::string_view(hdr).substr(0, colon);
        element_name = hdr.substr(colon + 1);
      } else {
        element_name = hdr;
      }

      // Look up element name -> (class_name, uid)
      if (auto it = name_to_class_uid.find(element_name);
          it != name_to_class_uid.end())
      {
        const auto& [cname, elem_uid] = it->second;
        if (!class_filter.empty() && class_filter != cname) {
          SPDLOG_WARN(
              "Boundary cuts: header '{}' class '{}' does "
              "not match element '{}' class '{}'; skipping",
              hdr,
              class_filter,
              element_name,
              cname);
        } else {
          for (const auto& [key, svar] : svar_map) {
            if (key.uid == elem_uid && is_final_state_col(key.col_name)) {
              found_col = svar.col();
              break;
            }
          }
        }
      }

      header_col_map.push_back(found_col);
      header_names.push_back(hdr);
    }

    // Track which missing state variables have already been warned about.
    std::set<std::size_t> warned_missing_cols;

    // ── Pre-scan: collect all rows for max_iterations filtering ──
    struct RawBoundaryCut
    {
      std::string name;
      IterationIndex iteration_index {};
      SceneUid scene_uid {};
      double rhs;
      std::string coeff_line;
    };

    std::vector<RawBoundaryCut> raw_cuts;
    std::string line;
    while (std::getline(ifs, line)) {
      strip_cr(line);
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      IterationIndex iteration_index {};
      if (has_iteration_col) {
        // Column 1: iteration
        std::getline(iss, token, ',');
        iteration_index = IterationIndex {std::stoi(token)};
      }

      // Next column: scene UID
      std::getline(iss, token, ',');
      const SceneUid scene_uid = make_uid<Scene>(std::stoi(token));

      // Next column: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // The rest contains the coefficient values
      std::string remainder;
      std::getline(iss, remainder);

      raw_cuts.push_back(RawBoundaryCut {
          .name = std::move(cut_name),
          .iteration_index = iteration_index,
          .scene_uid = scene_uid,
          .rhs = rhs,
          .coeff_line = std::move(remainder),
      });
    }

    // ── Filter by max_iterations ────────────────────────────────
    const auto max_iters = options.boundary_max_iterations;
    if (max_iters > 0 && has_iteration_col) {
      std::set<int> distinct_iters;
      for (const auto& rc : raw_cuts) {
        distinct_iters.insert(rc.iteration_index);
      }
      if (std::cmp_greater(distinct_iters.size(), max_iters)) {
        std::set<int> keep_iters;
        auto it = distinct_iters.end();
        for (int i = 0; i < max_iters; ++i) {
          --it;
          keep_iters.insert(*it);
        }
        std::erase_if(raw_cuts,
                      [&keep_iters](const RawBoundaryCut& rc)
                      { return !keep_iters.contains(rc.iteration_index); });
        SPDLOG_INFO(
            "SDDP: boundary cuts filtered to last {} "
            "iterations ({} cuts)",
            max_iters,
            raw_cuts.size());
      }
    }

    // ── Ensure the last phase has an alpha column ───────────────
    const auto sa = effective_scale_alpha(planning_lp, options.scale_alpha);
    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto& state = scene_phase_states[scene_index][last_phase];
      if (state.alpha_col == ColIndex {unknown_index}) {
        auto& li =
            planning_lp.system(scene_index, last_phase).linear_interface();
        state.alpha_col = li.add_col(SparseCol {
            .lowb = options.alpha_min / sa,
            .uppb = options.alpha_max / sa,
            .cost = sa,
            .scale = sa,
            .class_name = "Sddp",
            .variable_name = "alpha",
            .context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                          sim.phases()[last_phase].uid()},
        });
      }
    }

    // ── Add cuts to the LP ──────────────────────────────────────
    const auto& bdr_li =
        planning_lp.system(first_scene_index(), last_phase).linear_interface();
    const auto scale_obj = bdr_li.scale_objective();

    // When boundary_cuts_valuation == present_value, apply the
    // effective discount factor of the last phase's last stage to
    // the cut RHS and coefficients.  This brings present-value cuts
    // into the same basis as the LP objective.
    const auto valuation = sim.simulation().boundary_cuts_valuation.value_or(
        BoundaryCutsValuation::end_of_horizon);
    double bc_discount = 1.0;
    if (valuation == BoundaryCutsValuation::present_value) {
      const auto& last_phase_lp = sim.phases()[last_phase];
      if (!last_phase_lp.stages().empty()) {
        bc_discount = last_phase_lp.stages().back().discount_factor();
        SPDLOG_INFO(
            "Boundary cuts: applying present-value discount factor {:.6f}",
            bc_discount);
      }
    }

    IterationIndex max_iteration {};
    const auto missing_mode = options.missing_cut_var_mode;
    int cuts_loaded = 0;
    int cuts_skipped = 0;
    for (const auto& rc : raw_cuts) {
      // Pre-scan: check for missing state variables with non-zero
      // coefficients.
      bool has_missing = false;
      {
        std::istringstream scan_ss(rc.coeff_line);
        std::string tok;
        for (std::size_t ci = 0; ci < header_col_map.size(); ++ci) {
          if (!std::getline(scan_ss, tok, ',')) {
            break;
          }
          if (!header_col_map[ci].has_value() && !tok.empty()
              && std::stod(tok) != 0.0)
          {
            has_missing = true;
            if (!warned_missing_cols.contains(ci)) {
              warned_missing_cols.insert(ci);
              SPDLOG_WARN(
                  "Boundary cuts: state variable '{}' not "
                  "found in the current model; non-zero "
                  "coefficient {} (mode={})",
                  header_names[ci],
                  tok,
                  enum_name(missing_mode));
            }
          }
        }
      }
      if (has_missing && missing_mode == MissingCutVarMode::skip_cut) {
        ++cuts_skipped;
        continue;
      }

      // Determine which scenes get this cut
      SceneIndex scene_start {0};
      SceneIndex scene_end {num_scenes};
      if (separated) {
        auto it = scene_uid_to_index.find(rc.scene_uid);
        if (it == scene_uid_to_index.end()) {
          SPDLOG_TRACE(
              "Boundary cut '{}' scene UID {} not found in "
              "scene_array -- skipping",
              rc.name,
              rc.scene_uid);
          continue;
        }
        scene_start = it->second;
        scene_end = next(it->second);
      }

      for (const auto scene_index : iota_range(scene_start, scene_end)) {
        const auto& state = scene_phase_states[scene_index][last_phase];

        auto row = SparseRow {
            .lowb = rc.rhs * bc_discount / scale_obj,
            .uppb = LinearProblem::DblMax,
            .scale = sa,
            .class_name = "Bdr",
            .constraint_name = "cut",
            .context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                          sim.phases()[last_phase].uid()},
        };
        row[state.alpha_col] = sa;

        auto& li =
            planning_lp.system(scene_index, last_phase).linear_interface();
        std::istringstream coeff_ss(rc.coeff_line);
        std::string token;
        for (std::size_t ci = 0; ci < header_col_map.size(); ++ci) {
          if (!std::getline(coeff_ss, token, ',')) {
            break;
          }
          if (!header_col_map[ci].has_value()) {
            continue;  // skip_coeff: drop missing coefficient
          }
          const auto coeff = std::stod(token);
          if (coeff != 0.0) {
            const auto scale = li.get_col_scale(*header_col_map[ci]);
            row[*header_col_map[ci]] = -coeff * scale * bc_discount / scale_obj;
          }
        }

        li.add_row(row);
      }
      max_iteration = std::max(max_iteration, rc.iteration_index);
      ++cuts_loaded;
    }

    if (cuts_skipped > 0) {
      SPDLOG_WARN(
          "Boundary cuts: skipped {} cut(s) referencing "
          "missing state variables",
          cuts_skipped);
    }

    SPDLOG_INFO(
        "SDDP: loaded {} boundary cuts from {} (mode={}, "
        "max_iters={})",
        cuts_loaded,
        filepath,
        enum_name(mode),
        max_iters);
    return CutLoadResult {
        .count = cuts_loaded,
        .max_iteration = max_iteration,
    };

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading boundary cuts from {}: {}", filepath, e.what()),
    });
  }
}

// ─── Named hot-start cuts (all phases, named state variables) ───────────────

auto load_named_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    [[maybe_unused]] const LabelMaker& label_maker,
    StrongIndexVector<SceneIndex,
                      StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open named cuts file for reading: {}",
                                 filepath),
      });
    }

    // ── Parse header ────────────────────────────────────────────
    // Format: name,iteration,scene,phase,rhs,StateVar1,...
    std::string header_line;
    std::getline(ifs, header_line);
    strip_cr(header_line);

    std::vector<std::string> headers;
    {
      std::istringstream hss(header_line);
      std::string token;
      while (std::getline(hss, token, ',')) {
        headers.push_back(token);
      }
    }

    // Expect at least: name,iteration,scene,phase,rhs + 1 state var
    constexpr int kFixedCols = 5;
    if (std::cmp_less(headers.size(), kFixedCols + 1)) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Named cuts CSV must have at least {} columns "
                          "(name,iteration,scene,phase,rhs,<state_vars>); "
                          "got {}",
                          kFixedCols + 1,
                          headers.size()),
      });
    }

    // Verify the phase column header
    if (headers[3] != "phase") {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format("Named cuts CSV: expected column 4 to be "
                                 "'phase', got '{}'",
                                 headers[3]),
      });
    }

    // ── Build element-name -> uid lookup from the System ────────
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto& sys = planning_lp.planning().system;
    const auto& named_rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = named_rep_li.scale_objective();
    const auto sa = effective_scale_alpha(planning_lp, options.scale_alpha);

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    map_reserve(name_to_class_uid,
                sys.junction_array.size() + sys.battery_array.size());
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }

    // Build phase UID -> PhaseIndex lookup
    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // ── Build per-phase state-variable column maps ──────────────
    const auto num_state_cols = static_cast<int>(headers.size()) - kFixedCols;

    // Cache per-phase column maps (lazy: built on first use)
    std::unordered_map<PhaseIndex, std::vector<std::optional<ColIndex>>>
        phase_col_maps;
    map_reserve(phase_col_maps, sim.phases().size());

    auto get_col_map = [&](PhaseIndex phase_index)
        -> const std::vector<std::optional<ColIndex>>&
    {
      auto it = phase_col_maps.find(phase_index);
      if (it != phase_col_maps.end()) {
        return it->second;
      }

      // Build mapping for this phase by scanning state
      // variables
      const auto& svar_map =
          sim.state_variables(first_scene_index(), phase_index);

      std::vector<std::optional<ColIndex>> col_map;
      col_map.reserve(num_state_cols);

      for (std::size_t hi = kFixedCols; hi < headers.size(); ++hi) {
        const auto& hdr = headers[hi];
        std::optional<ColIndex> found_col;

        // Parse "ClassName:ElementName" or "ElementName"
        std::string_view class_filter;
        std::string element_name;
        if (const auto colon = hdr.find(':'); colon != std::string::npos) {
          class_filter = std::string_view(hdr).substr(0, colon);
          element_name = hdr.substr(colon + 1);
        } else {
          element_name = hdr;
        }

        if (auto nit = name_to_class_uid.find(element_name);
            nit != name_to_class_uid.end())
        {
          const auto& [cname, elem_uid] = nit->second;
          if (!class_filter.empty() && class_filter != cname) {
            SPDLOG_WARN(
                "Named cuts: header '{}' class '{}' != "
                "element '{}' class '{}'; skipping",
                hdr,
                class_filter,
                element_name,
                cname);
          } else {
            for (const auto& [key, svar] : svar_map) {
              if (key.uid == elem_uid && is_final_state_col(key.col_name)) {
                found_col = svar.col();
                break;
              }
            }
          }
        }

        col_map.push_back(found_col);
      }

      auto [ins_it, _] =
          phase_col_maps.emplace(phase_index, std::move(col_map));
      return ins_it->second;
    };

    // Track which missing state variables have already been warned about.
    std::set<std::pair<PhaseIndex, std::size_t>> warned_missing_named;

    // ── Read all cut rows ───────────────────────────────────────
    CutLoadResult result {};
    std::string line;
    while (std::getline(ifs, line)) {
      strip_cr(line);
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      // Column 1: iteration
      std::getline(iss, token, ',');
      const IterationIndex iteration_index {std::stoi(token)};

      // Column 2: scene UID
      std::getline(iss, token, ',');
      [[maybe_unused]] const SceneUid scene_uid =
          make_uid<Scene>(std::stoi(token));

      // Column 3: phase UID
      std::getline(iss, token, ',');
      const PhaseUid phase_uid = make_uid<Phase>(std::stoi(token));

      // Column 4: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // Resolve phase UID -> PhaseIndex
      auto pit = phase_uid_to_index.find(phase_uid);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "Named cuts: unknown phase UID {} in '{}', "
            "skipping cut '{}'",
            phase_uid,
            filepath,
            cut_name);
        continue;
      }
      const auto phase_index = pit->second;

      // Ensure alpha variable exists for this phase in all
      // scenes
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        auto& state = scene_phase_states[scene_index][phase_index];
        if (state.alpha_col == ColIndex {unknown_index}) {
          auto& li =
              planning_lp.system(scene_index, phase_index).linear_interface();
          state.alpha_col = li.add_col(SparseCol {
              .lowb = options.alpha_min / sa,
              .uppb = options.alpha_max / sa,
              .cost = sa,
              .scale = sa,
              .class_name = "Sddp",
              .variable_name = "alpha",
              .context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                            sim.phases()[phase_index].uid()},
          });
        }
      }

      // Get the column map for this phase
      const auto& col_map = get_col_map(phase_index);

      // Parse coefficient values and build the cut row
      std::string remainder;
      std::getline(iss, remainder);

      // Pre-scan: check for missing state variables
      bool has_missing = false;
      {
        std::istringstream scan_ss(remainder);
        std::string tok;
        for (std::size_t ci = 0; ci < col_map.size(); ++ci) {
          if (!std::getline(scan_ss, tok, ',')) {
            break;
          }
          if (!col_map[ci].has_value() && !tok.empty() && std::stod(tok) != 0.0)
          {
            has_missing = true;
            if (!warned_missing_named.contains({phase_index, ci})) {
              warned_missing_named.emplace(phase_index, ci);
              SPDLOG_WARN(
                  "Named cuts: state variable '{}' not "
                  "found in phase {}; non-zero "
                  "coefficient {} (mode={})",
                  headers[kFixedCols + ci],
                  phase_index,
                  tok,
                  enum_name(options.missing_cut_var_mode));
            }
          }
        }
      }
      if (has_missing
          && options.missing_cut_var_mode == MissingCutVarMode::skip_cut)
      {
        continue;
      }

      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        const auto& state = scene_phase_states[scene_index][phase_index];

        auto row = SparseRow {
            .lowb = rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
            .scale = sa,
            .class_name = "NamedHs",
            .constraint_name = "cut",
            .context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                          sim.phases()[phase_index].uid()},
        };
        row[state.alpha_col] = sa;

        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        std::istringstream coeff_ss(remainder);
        std::string ctok;
        for (std::size_t ci = 0; ci < col_map.size(); ++ci) {
          if (!std::getline(coeff_ss, ctok, ',')) {
            break;
          }
          const auto& col_opt = col_map[ci];
          if (!col_opt.has_value()) {
            continue;
          }
          const auto coeff = std::stod(ctok);
          if (coeff != 0.0) {
            const auto scale = li.get_col_scale(*col_opt);
            row[*col_opt] =
                -coeff * scale / scale_obj;  // physical cut coefficient
          }
        }

        li.add_row(row);
      }
      result.max_iteration = std::max(result.max_iteration, iteration_index);
      ++result.count;
    }

    SPDLOG_INFO(
        "SDDP: loaded {} named hot-start cuts from {}", result.count, filepath);
    return result;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading named cuts from {}: {}", filepath, e.what()),
    });
  }
}

// ─── JSON save functions ────────────────────────────────────────────────────

auto save_cuts_json(std::span<const StoredCut> cuts,
                    const PlanningLP& planning_lp,
                    const std::string& filepath) -> std::expected<void, Error>
{
  try {
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto& rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    const auto phase_map = build_phase_uid_map(planning_lp);
    const auto& sim = planning_lp.simulation();

    // Build per-phase ColKeyMap (cached)
    std::unordered_map<PhaseIndex, ColKeyMap> phase_col_keys;
    map_reserve(phase_col_keys, phase_map.size());
    for (const auto& [uid, pi] : phase_map) {
      phase_col_keys.try_emplace(
          pi, build_col_key_map(sim, first_scene_index(), pi));
    }

    // Build CutFileData
    CutFileData file_data {
        .version = 2,
        .scale_objective = scale_obj,
    };
    file_data.cuts.reserve(cuts.size());

    for (const auto& cut : cuts) {
      CutEntry entry {
          .type = (cut.type == CutType::Feasibility) ? "f" : "o",
          .phase_uid = static_cast<int>(cut.phase_uid),
          .scene_uid = static_cast<int>(cut.scene_uid),
          .name = cut.name,
          .rhs = cut.rhs * scale_obj,
          .dual = cut.dual,
      };

      auto pit = phase_map.find(cut.phase_uid);
      if (pit != phase_map.end()) {
        const auto& li = planning_lp.system(first_scene_index(), pit->second)
                             .linear_interface();
        const auto& col_keys = phase_col_keys[pit->second];

        for (const auto& [col, coeff] : cut.coefficients) {
          const auto scale = li.get_col_scale(col);
          const auto phys_coeff = coeff * scale_obj / scale;
          if (auto it = col_keys.find(col); it != col_keys.end()) {
            const auto& [cls, var, uid] = it->second;
            entry.coefficients.push_back(CutCoeffEntry {
                .key = std::format("{}:{}:{}", cls, var, uid),
                .coeff = phys_coeff,
            });
          } else {
            entry.coefficients.push_back(CutCoeffEntry {
                .key = "@alpha",
                .coeff = phys_coeff,
            });
          }
        }
      } else {
        // Unknown phase — write raw index-based coefficients
        for (const auto& [col, coeff] : cut.coefficients) {
          entry.coefficients.push_back(CutCoeffEntry {
              .key = std::format("@col:{}", static_cast<int>(col)),
              .coeff = coeff * scale_obj,
          });
        }
      }

      file_data.cuts.push_back(std::move(entry));
    }

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for writing: {}", filepath),
      });
    }
    ofs << daw::json::to_json(file_data) << '\n';

    SPDLOG_TRACE("SDDP: saved {} cuts to {} (JSON)", cuts.size(), filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error saving cuts to {} (JSON): {}", filepath, e.what()),
    });
  }
}

auto save_scene_cuts_json(std::span<const StoredCut> cuts,
                          [[maybe_unused]] SceneIndex scene_index,
                          SceneUid scene_uid,
                          const PlanningLP& planning_lp,
                          const std::string& directory)
    -> std::expected<void, Error>
{
  std::filesystem::create_directories(directory);
  const auto filepath =
      (std::filesystem::path(directory)
       / std::format(sddp_file::scene_cuts_json_fmt, scene_uid))
          .string();
  return save_cuts_json(cuts, planning_lp, filepath);
}

// ─── JSON load function ────────────────────────────────────────────────────

auto load_cuts_json(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for reading: {}", filepath),
      });
    }

    const std::string json_str(std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char> {});

    const auto file_data = daw::json::from_json<CutFileData>(json_str);
    // scale_objective from file is informational; the LP's own
    // scale_objective is used for coefficient conversion.
    [[maybe_unused]] const auto scale_obj_file = file_data.scale_objective;

    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto& rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Track loaded (phase_uid, name) pairs to skip duplicates
    std::set<std::pair<int, std::string>> loaded_keys;

    CutLoadResult result {};

    for (const auto& entry : file_data.cuts) {
      if (!loaded_keys.emplace(entry.phase_uid, entry.name).second) {
        continue;
      }

      result.max_iteration = std::max(result.max_iteration,
                                      extract_iteration_from_name(entry.name));

      auto pit = phase_uid_to_index.find(make_uid<Phase>(entry.phase_uid));
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "SDDP load_cuts_json: unknown phase UID {} for cut '{}'; "
            "skipping",
            entry.phase_uid,
            entry.name);
        continue;
      }
      const auto phase_index = pit->second;

      // Build the LP row from physical RHS
      auto row = SparseRow {
          .lowb = entry.rhs / scale_obj,
          .uppb = LinearProblem::DblMax,
          .scale = scale_alpha,
          .class_name = "Loaded",
          .constraint_name = "cut",
      };

      // State variable map for structured key resolution
      const auto& sv_map =
          sim.state_variables(first_scene_index(), phase_index);

      // Resolve coefficients
      const bool cut_valid = true;
      struct ResolvedCoeff
      {
        ColIndex col;
        double coeff;
      };
      std::vector<ResolvedCoeff> resolved_coeffs;

      for (const auto& [key, coeff] : entry.coefficients) {
        if (key == "@alpha") {
          // Alpha column
          if (scene_phase_states != nullptr) {
            const auto& states = (*scene_phase_states)[first_scene_index()];
            if (phase_index < PhaseIndex {states.size()}) {
              const auto alpha_col = states[phase_index].alpha_col;
              if (alpha_col != ColIndex {unknown_index}) {
                resolved_coeffs.push_back({
                    .col = alpha_col,
                    .coeff = coeff,
                });
              }
            }
          } else {
            // Fallback: find alpha column by LP name
            auto& li_ref = planning_lp.system(first_scene_index(), phase_index)
                               .linear_interface();
            const auto& name_map = li_ref.col_name_map();
            for (const auto& [nm, idx] : name_map) {
              if (nm.starts_with("sddp_alpha_")) {
                resolved_coeffs.push_back({
                    .col = idx,
                    .coeff = coeff,
                });
                break;
              }
            }
          }
        } else if (std::ranges::count(key, ':') == 2) {
          // Structured key: class:var:uid
          const auto c1 = key.find(':');
          const auto c2 = key.find(':', c1 + 1);
          const auto cls = key.substr(0, c1);
          const auto var = key.substr(c1 + 1, c2 - c1 - 1);
          const auto uid_str = key.substr(c2 + 1);
          int uid_val = 0;
          auto [ptr, ec] =
              std::from_chars(uid_str.data(),
                              uid_str.data() + uid_str.size(),  // NOLINT
                              uid_val);
          if (ec != std::errc {}) {
            SPDLOG_WARN(
                "SDDP load_cuts_json: invalid uid '{}' in key for "
                "cut '{}'; skipping coeff",
                uid_str,
                entry.name);
            continue;
          }
          bool found = false;
          for (const auto& [sv_key, svar] : sv_map) {
            if (sv_key.class_name == cls && sv_key.col_name == var
                && sv_key.uid == Uid {uid_val})
            {
              resolved_coeffs.push_back({
                  .col = svar.col(),
                  .coeff = coeff,
              });
              found = true;
              break;
            }
          }
          if (!found) {
            SPDLOG_WARN(
                "SDDP load_cuts_json: key '{}' not found in state "
                "variables for cut '{}'; ignoring coefficient",
                key,
                entry.name);
          }
        } else {
          SPDLOG_WARN(
              "SDDP load_cuts_json: unrecognized key '{}' in cut "
              "'{}'; ignoring",
              key,
              entry.name);
        }
      }

      if (!cut_valid) {
        continue;
      }

      // Add resolved cut to all scenes
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        auto scene_row = row;
        scene_row.context = ScenePhaseContext {sim.scenes()[scene_index].uid(),
                                               sim.phases()[phase_index].uid()};
        for (const auto& [col, coeff] : resolved_coeffs) {
          const auto scale = li.get_col_scale(col);
          scene_row[col] = coeff * scale / scale_obj;
        }
        li.add_row(scene_row);
      }
      ++result.count;
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {} (JSON)", result.count, filepath);
    return result;

  } catch (const daw::json::json_exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "JSON parse error loading cuts from {}: {}", filepath, e.what()),
    });
  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading cuts from {} (JSON): {}", filepath, e.what()),
    });
  }
}

// ─── Format-dispatching functions ──────────────────────────────────────────

auto save_cuts(std::span<const StoredCut> cuts,
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

auto load_cuts(
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

}  // namespace gtopt
