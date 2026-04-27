/**
 * @file      sddp_named_cuts.cpp
 * @brief     Named hot-start cuts (cross-phase, named state variables) — load.
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

#include <gtopt/as_label.hpp>
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
using namespace gtopt::detail;

[[nodiscard]] auto load_named_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    [[maybe_unused]] const LabelMaker& label_maker,
    [[maybe_unused]] const StrongIndexVector<
        SceneIndex,
        StrongIndexVector<PhaseIndex, PhaseStateInfo>>& scene_phase_states)
    -> std::expected<CutLoadResult, Error>
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
    const auto num_state_cols = std::ssize(headers) - kFixedCols;

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

      // α is already registered on every phase by
      // `SDDPMethod::initialize_alpha_variables`; the install loop
      // below calls `free_alpha(…)` per cell before `add_row` to
      // release the bootstrap pin, mirroring the backward-pass
      // contract.

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
        const auto* alpha_svar = find_alpha_state_var(
            planning_lp.simulation(), scene_index, phase_index);
        if (alpha_svar == nullptr) {
          continue;  // No α on this (scene, phase) — nothing to add
        }

        // IterationContext with `(iter, cut_offset)` so each loaded
        // named hot-start cut has a unique metadata signature —
        // see the CSV loader comment above for rationale.
        auto row = SparseRow {
            .lowb = rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
            .scale = sa,
            .class_name = sddp_named_cut_class_name,
            .constraint_name = sddp_loaded_cut_constraint_name,
            .variable_uid = sim.uid_of(phase_index),
            .context = make_iteration_context(
                sim.uid_of(scene_index),
                sim.uid_of(phase_index),
                uid_of(
                    extract_iteration_from_name(std::string_view {cut_name})),
                result.count),
        };
        row[alpha_svar->col()] = sa;

        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        std::istringstream coeff_ss(remainder);
        std::string ctok;
        for (const auto& col_opt : col_map) {
          if (!std::getline(coeff_ss, ctok, ',')) {
            break;
          }
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

        // Named hot-start cuts are optimality-style (they carry α at
        // coefficient `sa` by construction above).  Routed through
        // the unified `add_cut_row` so the α release, row addition,
        // and low-memory replay registration happen in one call.
        std::ignore = add_cut_row(
            planning_lp, scene_index, phase_index, CutType::Optimality, row);
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

}  // namespace gtopt
