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
          .name = cut.name,
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
                "SDDP save_cuts_json: cut '{}' references col {} that "
                "is not a registered state variable",
                cut.name,
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

      // Build the row in physical space — same contract as
      // `build_benders_cut_physical`.  `add_row` applies col_scales
      // + per-row row-max equilibration on insertion.
      auto row = SparseRow {
          .lowb = entry.rhs,
          .uppb = LinearProblem::DblMax,
          .class_name = sddp_loaded_cut_class_name,
          .constraint_name = sddp_loaded_cut_constraint_name,
          .variable_uid = sim.uid_of(phase_index),
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
        if (std::ranges::count(key, ':') == 2) {
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

      // Add resolved cut (physical) to all scenes — add_row handles
      // col_scales + row-max.  Use IterationContext so each loaded
      // cut has a unique metadata signature (see CSV loader for
      // rationale).
      const auto cut_iter = extract_iteration_from_name(entry.name);
      const auto cut_offset = result.count;
      const auto phase_uid_ctx = sim.uid_of(phase_index);
      const auto cut_type =
          (entry.type == "f") ? CutType::Feasibility : CutType::Optimality;
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
        auto scene_row = row;
        scene_row.context = make_iteration_context(sim.uid_of(scene_index),
                                                   phase_uid_ctx,
                                                   uid_of(cut_iter),
                                                   cut_offset);
        for (const auto& [col, coeff] : resolved_coeffs) {
          scene_row[col] = coeff;
        }
        // Unified `add_cut_row`: same semantics as the CSV loader.
        std::ignore = add_cut_row(
            planning_lp, scene_index, phase_index, cut_type, scene_row);
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

}  // namespace gtopt
