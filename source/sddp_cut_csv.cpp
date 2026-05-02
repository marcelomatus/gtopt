/**
 * @file      sddp_cut_csv.cpp
 * @brief     CSV save / load for SDDP cuts.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from `source/sddp_cut_io.cpp`.  Implements the CSV
 * subset of cut persistence: `save_cuts_csv`, `save_scene_cuts_csv`,
 * `load_cuts_csv`, `load_scene_cuts_from_directory`.  The format
 * dispatchers `save_cuts` / `load_cuts` stay in
 * `source/sddp_cut_io.cpp`.
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

#include <gtopt/fmap.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_io_internal.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{
using namespace gtopt::detail;  // helpers shared with sibling TUs

[[nodiscard]] auto save_cuts_csv(std::span<const StoredCut> cuts,
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

      // RHS in physical objective units (stored verbatim — the cut was
      // built by `build_benders_cut_physical` and captured in
      // StoredCut pre-equilibration).  Stream fields directly so the
      // per-cut path does NOT allocate a transient `std::string` from
      // `as_label`; runs once per cut, of which there can be tens of
      // thousands per save.
      ofs << type_char << ',' << cut.phase_uid << ',' << cut.scene_uid << ','
          << cut.name << ',' << cut.rhs << ',';
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

[[nodiscard]] auto save_scene_cuts_csv(std::span<const StoredCut> cuts,
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

      // RHS in physical objective units (stored verbatim — see
      // save_cuts_csv above for rationale).  Stream fields directly to
      // avoid the per-cut `std::string` from `as_label`.
      ofs << type_char << ',' << cut.phase_uid << ',' << cut.scene_uid << ','
          << cut.name << ',' << cut.rhs << ',';
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

[[nodiscard]] auto load_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    [[maybe_unused]] double scale_alpha,
    [[maybe_unused]] const LabelMaker& label_maker,
    [[maybe_unused]] const StrongIndexVector<
        SceneIndex,
        StrongIndexVector<PhaseIndex, PhaseStateInfo>>* scene_phase_states)
    -> std::expected<CutLoadResult, Error>
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

    // Build phase UID -> PhaseIndex lookup
    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Track (phase_uid, cut_name) pairs already loaded.
    // Cuts are broadcast to all scenes, so if the CSV contains
    // the same cut for multiple scenes we must load it only once.
    std::set<std::pair<PhaseUid, std::string>> loaded_keys;

    // Per-cell accumulator: collect every (scene, phase, cut_type) install
    // in pass 1, then dispatch one bulk `add_rows` per (scene, phase, type)
    // group in pass 2.  Saves O(num_scenes × num_cuts) backend round-trips
    // on a recovery run that streams thousands of warm-start cuts.
    //
    // Key shape: `(SceneIndex, PhaseIndex)`; each cell holds two named
    // vectors (Optimality cuts trigger `free_alpha_for_cut`, Feasibility
    // cuts do not).  `gtopt::flat_map` is the project-standard sorted /
    // contiguous map (cf. `feedback_no_views_iota.md` / `fmap.hpp`),
    // chosen here for cache-friendly iteration over the small cell set
    // (typically ≤ num_scenes × num_phases ~ a few hundred entries) and
    // a deterministic install order across runs.
    using CellKey = std::pair<SceneIndex, PhaseIndex>;
    struct CellCuts
    {
      std::vector<SparseRow> opt;
      std::vector<SparseRow> feas;
    };
    flat_map<CellKey, CellCuts> accum;
    // Upper bound: every (scene, phase) cell receives at least one cut.
    // `phase_uid_to_index.size()` is the count of phases known to the LP;
    // the CSV may target any subset.  Reserving up-front avoids repeated
    // reallocation of the underlying vector storage as cells are created.
    map_reserve(accum,
                static_cast<size_t>(num_scenes) * phase_uid_to_index.size());

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

      // Parse optional type column (backward compatible).  The type
      // determines whether `free_alpha` fires when the cut lands —
      // only optimality cuts certify a tighter lower bound on the
      // true future cost, so only they should release α's bootstrap
      // floor `lowb = sddp_alpha_bootstrap_min (= 0)`.  Feasibility
      // cuts convey "these states cause downstream infeasibility"
      // but not a lower-bound update, so α must stay floored at 0.
      CutType cut_type = CutType::Optimality;
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

      // CSV values are physical.  Build the row in physical space and
      // let `add_row` fold col_scales + per-row row-max equilibration
      // internally — same path as freshly-built
      // `build_benders_cut_physical` cuts.
      auto row = SparseRow {
          .lowb = rhs,
          .uppb = LinearProblem::DblMax,
          .variable_uid = phase_uid,
      };
      sddp_loaded_cut_tag.apply_to(row);

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

      // Collect coefficients from CSV.  Two formats are supported:
      //   class:var:uid=coeff  (structured key — preferred, no LP names)
      //   name=coeff           (legacy name-based — resolve by column name)
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

          if (std::ranges::count(key_part, ':') == 2) {
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
          if (file_col < 0 || file_col >= li_ref.get_numcols()) {
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
      // Coefficients are physical; `add_row` applies col_scales +
      // per-row row-max on an equilibrated LP, matching the
      // treatment of freshly-built cuts (`build_benders_cut_physical`).
      //
      // Use IterationContext rather than ScenePhaseContext so each
      // loaded cut carries a unique `(scene, phase, iter, offset)`
      // signature — otherwise the metadata-based duplicate detector
      // in `LinearInterface::add_row` would reject the second cut
      // for the same (scene, phase) pair.  `extract_iteration_from_
      // name` pulls the SDDP iteration index encoded in the CSV
      // cut name; `result.count` breaks ties when the same iteration
      // emits multiple cuts.
      const auto cut_iter = extract_iteration_from_name(cut_name);
      const auto cut_offset = result.count;
      const auto phase_uid_ctx = sim.uid_of(phase_index);
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        auto scene_row = row;
        scene_row.context = make_iteration_context(sim.uid_of(scene_index),
                                                   phase_uid_ctx,
                                                   uid_of(cut_iter),
                                                   cut_offset);
        for (const auto& [col, coeff] : resolved_coeffs) {
          scene_row[col] = coeff;
        }
        // Stage the cut into the per-cell accumulator; install happens
        // in one bulk pass after the file is fully streamed (below).
        // Coefficients stay in physical space — `add_rows` applies
        // col_scales + per-row row-max identically to the per-cut path.
        auto& cell = accum[std::make_pair(scene_index, phase_index)];
        if (cut_type == CutType::Optimality) {
          cell.opt.push_back(std::move(scene_row));
        } else {
          cell.feas.push_back(std::move(scene_row));
        }
      }
      ++result.count;
    }

    // Pass 2: bulk-install per (scene, phase, cut_type) cell.  Mirrors the
    // unified `add_cut_row` semantics:
    //   * Optimality cuts → release α (idempotent across the batch),
    //     then bulk `add_rows`, then per-cut `record_cut_row`.
    //   * Feasibility cuts → bulk `add_rows` + per-cut `record_cut_row`
    //     (no α release, by `free_alpha_for_cut`'s gating contract).
    // `auto&&` — iterating `gtopt::flat_map` (std::flat_map under GCC 15)
    // yields a proxy `pair<key&, value&>` that is a temporary, so a plain
    // `auto&` lvalue reference would not bind.
    for (auto&& [cell_key, cell_cuts] : accum) {
      const auto [scene_index, phase_index] = cell_key;

      if (!cell_cuts.opt.empty()) {
        for (const auto& cut : cell_cuts.opt) {
          free_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
        }
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        li.add_rows(cell_cuts.opt);
        for (const auto& cut : cell_cuts.opt) {
          li.record_cut_row(cut);
        }
      }

      if (!cell_cuts.feas.empty()) {
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        li.add_rows(cell_cuts.feas);
        for (const auto& cut : cell_cuts.feas) {
          li.record_cut_row(cut);
        }
      }
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

[[nodiscard]] auto load_scene_cuts_from_directory(
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

}  // namespace gtopt
