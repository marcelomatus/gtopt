/**
 * @file      sddp_cut_json.cpp
 * @brief     JSON save / load for SDDP cuts.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from `source/sddp_cut_io.cpp`.
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

#include <daw/json/daw_json_link.h>
#include <gtopt/as_label.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/json/json_sddp_cut_io.hpp>
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
using namespace gtopt::detail;

[[nodiscard]] auto save_cuts_json(std::span<const StoredCut> cuts,
                                  const PlanningLP& planning_lp,
                                  const std::string& filepath)
    -> std::expected<void, Error>
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
          .iteration = static_cast<int>(uid_of(cut.iteration_index)),
          .extra = cut.extra,
          .rhs = cut.rhs,
          .dual = cut.dual,
      };

      auto pit = phase_map.find(cut.phase_uid);
      if (pit != phase_map.end()) {
        const auto& col_keys = phase_col_keys[pit->second];

        for (const auto& [col, coeff] : cut.coefficients) {
          const auto it = col_keys.find(col);
          if (it == col_keys.end()) {
            throw std::runtime_error(std::format(
                "SDDP save_cuts_json: cut at (scene={}, phase={}, iter={}) "
                "references col {} that is not a registered state variable",
                cut.scene_uid,
                cut.phase_uid,
                uid_of(cut.iteration_index),
                col));
          }
          const auto& [cls, var, uid] = it->second;
          entry.coefficients.push_back(CutCoeffEntry {
              .key = as_label<':'>(cls, var, uid),
              .coeff = coeff,
          });
        }
      } else {
        // Unknown phase — write raw index-based coefficients (still
        // physical; no scale applied).
        for (const auto& [col, coeff] : cut.coefficients) {
          entry.coefficients.push_back(CutCoeffEntry {
              .key = as_label<':'>("@col", col),
              .coeff = coeff,
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

[[nodiscard]] auto save_scene_cuts_json(std::span<const StoredCut> cuts,
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

[[nodiscard]] auto load_cuts_json(
    PlanningLP& planning_lp,
    const std::string& filepath,
    [[maybe_unused]] double scale_alpha,
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

    const std::string json_str(std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char> {});

    const auto file_data = daw::json::from_json<CutFileData>(json_str);
    // scale_objective from file is informational; the LP's own
    // scale_objective is used for coefficient conversion.
    [[maybe_unused]] const auto scale_obj_file = file_data.scale_objective;

    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();

    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Use :class:`CutKey` (the full 5-tuple including ``extra``) to
    // skip duplicates.  Including ``extra`` keeps multi-cut
    // feasibility siblings distinguishable (forward pass ``multi_cut``
    // mode emits multiple per-slack cuts per ``(scene, phase, iter)``
    // cell, all with ``type=Feasibility``).
    std::set<CutKey> loaded_keys;

    // Per-cell accumulator: collect every (scene, phase, cut_type) install
    // in pass 1, then dispatch one bulk `add_rows` per (scene, phase, type)
    // group in pass 2.  Mirrors the cut_csv / named_cuts / boundary_cuts
    // loaders (commit 08f0202a) — saves O(num_scenes × num_cuts) backend
    // round-trips on a recovery run that streams thousands of warm-start
    // cuts.
    using CellKey = std::pair<SceneIndex, PhaseIndex>;
    struct CellCuts
    {
      std::vector<SparseRow> opt;
      std::vector<SparseRow> feas;
    };
    flat_map<CellKey, CellCuts> accum;
    map_reserve(accum,
                static_cast<size_t>(num_scenes) * phase_uid_to_index.size());

    CutLoadResult result {};

    for (const auto& entry : file_data.cuts) {
      const auto entry_type =
          (entry.type == "f") ? CutType::Feasibility : CutType::Optimality;
      const auto entry_key = CutKey {
          .type = entry_type,
          .scene_uid = make_uid<Scene>(entry.scene_uid),
          .phase_uid = make_uid<Phase>(entry.phase_uid),
          .iteration_index = IterationIndex {entry.iteration},
          .extra = entry.extra,
      };
      if (!loaded_keys.insert(entry_key).second) {
        continue;
      }

      result.max_iteration =
          std::max(result.max_iteration, IterationIndex {entry.iteration});

      auto pit = phase_uid_to_index.find(make_uid<Phase>(entry.phase_uid));
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "SDDP load_cuts_json: unknown phase UID {} for cut at iter "
            "{}; skipping",
            entry.phase_uid,
            entry.iteration);
        continue;
      }
      const auto phase_index = pit->second;

      // Build the row in physical space — same contract as
      // `build_benders_cut_physical`.  `add_row` applies col_scales
      // + per-row row-max equilibration on insertion.
      auto row = SparseRow {
          .lowb = entry.rhs,
          .uppb = LinearProblem::DblMax,
          .variable_uid = sim.uid_of(phase_index),
      };
      sddp_loaded_cut_tag.apply_to(row);

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
        if (std::ranges::count(key, ':') == 2) {
          // Structured key: class:var:uid
          const auto c1 = key.find(':');
          const auto c2 = key.find(':', c1 + 1);
          const auto cls = key.substr(0, c1);
          const auto var = key.substr(c1 + 1, c2 - c1 - 1);
          const auto uid_str = key.substr(c2 + 1);
          int uid_val = 0;
          const auto* const end_ptr = uid_str.data() + uid_str.size();
          auto [ptr, ec] = std::from_chars(
              // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
              uid_str.data(),
              end_ptr,
              uid_val);
          if (ec != std::errc {}) {
            SPDLOG_WARN(
                "SDDP load_cuts_json: invalid uid '{}' in key at iter "
                "{}; skipping coeff",
                uid_str,
                entry.iteration);
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
                "variables at iter {}; ignoring coefficient",
                key,
                entry.iteration);
          }
        } else {
          SPDLOG_WARN(
              "SDDP load_cuts_json: unrecognized key '{}' at iter {}; "
              "ignoring",
              key,
              entry.iteration);
        }
      }

      if (!cut_valid) {
        continue;
      }

      // Route to the specific scene matching ``entry.scene_uid``
      // (same per-scene routing as ``load_cuts_parquet``).  Preserves
      // ``entry.extra`` as the IterationContext discriminator so
      // multi-cut feasibility siblings stay distinguishable.
      const auto cut_iter = IterationIndex {entry.iteration};
      // Sentinel ``-1`` (= "no source context attached") → fall back
      // to a per-load serial counter.  Real ``extra`` values
      // (including 0) round-trip unchanged.
      const auto cut_offset =
          (entry.extra >= 0) ? entry.extra : static_cast<int>(result.count);
      const auto phase_uid_ctx = sim.uid_of(phase_index);
      SceneIndex target_scene {};
      bool scene_found = false;
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        if (sim.uid_of(scene_index) == make_uid<Scene>(entry.scene_uid)) {
          target_scene = scene_index;
          scene_found = true;
          break;
        }
      }
      if (!scene_found) {
        SPDLOG_WARN(
            "SDDP load_cuts_json: unknown scene UID {} for cut at iter "
            "{}; skipping",
            entry.scene_uid,
            entry.iteration);
        continue;
      }
      {
        auto scene_row = row;
        scene_row.context = make_iteration_context(sim.uid_of(target_scene),
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
        auto& cell = accum[std::make_pair(target_scene, phase_index)];
        if (entry_type == CutType::Optimality) {
          cell.opt.push_back(std::move(scene_row));
        } else {
          cell.feas.push_back(std::move(scene_row));
        }
      }
      ++result.count;
    }

    // Pass 2: bulk-install per (scene, phase, cut_type) cell.  Mirrors
    // the unified `add_cut_row` semantics:
    //   * Optimality cuts → release α (idempotent across the batch),
    //     then bulk `add_rows`, then per-cut `record_cut_row`.
    //   * Feasibility cuts → bulk `add_rows` + per-cut `record_cut_row`
    //     (no α release, by `bound_alpha_for_cut`'s gating contract).
    // `auto&&` because `gtopt::flat_map` (std::flat_map under GCC 15)
    // yields a proxy `pair<key&, value&>` that doesn't bind to `auto&`.
    for (auto&& [cell_key, cell_cuts] : accum) {
      const auto [scene_index, phase_index] = cell_key;

      if (!cell_cuts.opt.empty()) {
        for (const auto& cut : cell_cuts.opt) {
          bound_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
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

}  // namespace gtopt
