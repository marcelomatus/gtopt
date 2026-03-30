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
#include <unordered_map>
#include <utility>

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
  flat_map<PhaseUid, PhaseIndex> phase_map;
  for (auto&& [pi, phase] :
       enumerate<PhaseIndex>(planning_lp.simulation().phases()))
  {
    phase_map.emplace(phase.uid(), pi);
  }
  return phase_map;
}

auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>
{
  flat_map<SceneUid, SceneIndex> scene_map;
  for (auto&& [si, scene] :
       enumerate<SceneIndex>(planning_lp.simulation().scenes()))
  {
    scene_map.emplace(scene.uid(), si);
  }
  return scene_map;
}

// ─── Helper: state-variable name matching ───────────────────────────────────

namespace
{

/// Returns true if *col_name* is a final-state-variable name that can
/// appear in boundary/hot-start cut CSV headers (efin, soc, vfin).
[[nodiscard]] constexpr auto is_final_state_col(std::string_view col_name)
    -> bool
{
  return col_name == "efin" || col_name == "soc" || col_name == "vfin";
}

/// Write a single cut's coefficients to an output stream.
///
/// Format: col_name=coeff (name-based, portable across LP changes).
/// Coefficients are in fully physical space:
///   coeff_csv = LP_coeff * scale_obj / col_scale
void write_cut_coefficients(std::ostream& ofs,
                            const StoredCut& cut,
                            const LinearInterface& li,
                            double scale_obj)
{
  const auto& names = li.col_index_to_name();
  for (const auto& [col, coeff] : cut.coefficients) {
    const auto scale = li.get_col_scale(ColIndex {col});
    const auto idx = static_cast<size_t>(col);
    const bool has_name = idx < names.size() && !names[idx].empty();
    if (has_name) {
      // Name-based format (portable across LP structure changes)
      ofs << "," << names[idx] << "=" << (coeff * scale_obj / scale);
    } else {
      // Fallback to index-based format for unnamed columns
      ofs << "," << col << ":" << (coeff * scale_obj / scale);
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

    const auto scale_obj = planning_lp.options().scale_objective();
    if (!append_mode) {
      ofs << "# scale_objective=" << scale_obj << "\n";
      ofs << "type,phase,scene,name,rhs,dual,coefficients\n";
    }

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    for (const auto& cut : cuts) {
      // Type column: 'o' = optimality, 'f' = feasibility
      const char type_char = (cut.type == CutType::Feasibility) ? 'f' : 'o';

      // RHS in physical objective units
      ofs << type_char << "," << cut.phase << "," << cut.scene << ","
          << cut.name << "," << (cut.rhs * scale_obj) << ",";
      if (cut.dual.has_value()) {
        ofs << *cut.dual;
      }

      // Look up the LinearInterface to retrieve column scales.
      // Use scene 0 as representative (scales are identical
      // across scenes because the LP structure is built
      // identically per phase).
      auto pit = phase_map.find(cut.phase);
      if (pit != phase_map.end()) {
        const auto& li =
            planning_lp.system(SceneIndex {0}, pit->second).linear_interface();
        write_cut_coefficients(ofs, cut, li, scale_obj);
      } else {
        SPDLOG_WARN(
            "save_cuts: unknown phase UID {} for cut '{}'; "
            "writing without variable scaling",
            cut.phase,
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
                         SceneIndex scene,
                         int scene_uid_val,
                         const PlanningLP& planning_lp,
                         const std::string& directory)
    -> std::expected<void, Error>
{
  try {
    std::filesystem::create_directories(directory);

    const auto filepath =
        (std::filesystem::path(directory)
         / std::format(sddp_file::scene_cuts_fmt, scene_uid_val))
            .string();

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open scene cut file for writing: {}",
                                 filepath),
      });
    }

    const auto scale_obj = planning_lp.options().scale_objective();
    ofs << "# scale_objective=" << scale_obj << "\n";
    ofs << "type,phase,scene,name,rhs,dual,coefficients\n";

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    for (const auto& cut : cuts) {
      // Type column: 'o' = optimality, 'f' = feasibility
      const char type_char = (cut.type == CutType::Feasibility) ? 'f' : 'o';

      // RHS in physical objective units
      ofs << type_char << "," << cut.phase << "," << cut.scene << ","
          << cut.name << "," << (cut.rhs * scale_obj) << ",";
      if (cut.dual.has_value()) {
        ofs << *cut.dual;
      }

      auto pit = phase_map.find(cut.phase);
      if (pit != phase_map.end()) {
        const auto& li =
            planning_lp.system(scene, pit->second).linear_interface();
        write_cut_coefficients(ofs, cut, li, scale_obj);
      } else {
        SPDLOG_WARN(
            "save_scene_cuts: unknown phase UID {} for cut "
            "'{}'; writing without variable scaling",
            cut.phase,
            cut.name);
        write_cut_coefficients_unscaled(ofs, cut, scale_obj);
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts for scene UID {} to {}",
                 cuts.size(),
                 scene_uid_val,
                 filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("Error saving scene UID {} cuts to {}: {}",
                               scene_uid_val,
                               directory,
                               e.what()),
    });
  }
}

// ─── Load functions ─────────────────────────────────────────────────────────

auto load_cuts_csv(PlanningLP& planning_lp,
                   const std::string& filepath,
                   const LabelMaker& label_maker)
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
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto scale_obj = planning_lp.options().scale_objective();

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
      PhaseUid phase_val {};
      try {
        phase_val = PhaseUid {std::stoi(token)};
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
      [[maybe_unused]] int scene_val = 0;
      try {
        scene_val = std::stoi(token);
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
      if (!loaded_keys.emplace(phase_val, cut_name).second) {
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
      // space.
      auto row = SparseRow {
          .name = label_maker.lp_label("loaded", cut_name),
          .lowb = rhs / scale_obj,
          .uppb = LinearProblem::DblMax,
      };

      // Resolve the phase UID to a PhaseIndex
      auto pit = phase_uid_to_index.find(phase_val);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "SDDP load_cuts: unknown phase UID {} in {}, "
            "skipping cut '{}'",
            phase_val,
            filepath,
            cut_name);
        continue;
      }
      const auto phase = pit->second;

      // Use scene 0 as representative for column name resolution
      // (LP structure is identical across scenes).
      auto& li_ref =
          planning_lp.system(SceneIndex {0}, phase).linear_interface();

      // Collect coefficients from CSV.  Two formats are supported:
      //   name=coeff   (name-based, preferred — resolve by column name)
      //   index:coeff  (legacy — validate index against current LP)
      struct ResolvedCoeff
      {
        ColIndex col;
        double coeff;
      };
      std::vector<ResolvedCoeff> resolved_coeffs;
      bool cut_valid = true;

      while (std::getline(iss, token, ',')) {
        // Try name-based format first (name=coeff)
        if (const auto eq = token.find('='); eq != std::string::npos) {
          const auto col_name = token.substr(0, eq);
          const auto coeff = std::stod(token.substr(eq + 1));
          const auto& name_map = li_ref.col_name_map();
          auto it = name_map.find(col_name);
          if (it != name_map.end()) {
            resolved_coeffs.push_back({
                .col = ColIndex {it->second},
                .coeff = coeff,
            });
          } else {
            // Name not found in LP name map — warn and skip this
            // coefficient (the LP structure may have changed since
            // the cuts were saved).
            SPDLOG_WARN(
                "SDDP load_cuts: column '{}' not found in "
                "current LP for cut '{}'; ignoring coefficient",
                col_name,
                cut_name);
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

          // Warn if the LP structure may have changed (column name
          // available but index-based format was used)
          const auto& idx_names = li_ref.col_index_to_name();
          if (static_cast<size_t>(file_col) < idx_names.size()
              && !idx_names[static_cast<size_t>(file_col)].empty())
          {
            SPDLOG_DEBUG(
                "SDDP load_cuts: legacy index-based coefficient "
                "col={} (name='{}') in cut '{}'; consider "
                "re-saving cuts with named format",
                file_col,
                idx_names[static_cast<size_t>(file_col)],
                cut_name);
          }
          resolved_coeffs.push_back({
              .col = ColIndex {file_col},
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
      for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
        auto& li = planning_lp.system(scene, phase).linear_interface();
        auto scene_row = row;
        // Include scene index in name to avoid duplicates across scenes
        scene_row.name = label_maker.lp_label("loaded", cut_name, scene, phase);
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

auto load_scene_cuts_from_directory(PlanningLP& planning_lp,
                                    const std::string& directory,
                                    const LabelMaker& label_maker)
    -> std::expected<CutLoadResult, Error>
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

    auto result =
        load_cuts_csv(planning_lp, entry.path().string(), label_maker);
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
    const LabelMaker& label_maker,
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
    const auto num_phases = static_cast<Index>(sim.phases().size());
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto last_phase = PhaseIndex {num_phases - 1};

    // Build scene UID -> SceneIndex lookup (for "separated" mode)
    flat_map<SceneUid, SceneIndex> scene_uid_to_index;
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      scene_uid_to_index[sim.scenes()[si].uid()] = si;
    }

    // Build element-name -> uid lookup from the System.
    const auto& sys = planning_lp.planning().system;

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }

    // For each state-variable header column, find the
    // corresponding LP column in the last phase.
    const auto& svar_map = sim.state_variables(SceneIndex {0}, last_phase);

    const auto num_state_cols =
        static_cast<int>(headers.size()) - state_var_start;
    std::vector<std::optional<ColIndex>> header_col_map;
    header_col_map.reserve(num_state_cols);
    std::vector<std::string> header_names;
    header_names.reserve(num_state_cols);

    for (int hi = state_var_start; std::cmp_less(hi, headers.size()); ++hi) {
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
    std::set<int> warned_missing_cols;

    // ── Pre-scan: collect all rows for max_iterations filtering ──
    struct RawBoundaryCut
    {
      std::string name;
      IterationIndex iteration {};
      SceneUid scene {};
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

      IterationIndex iteration {};
      if (has_iteration_col) {
        // Column 1: iteration
        std::getline(iss, token, ',');
        iteration = IterationIndex {std::stoi(token)};
      }

      // Next column: scene UID
      std::getline(iss, token, ',');
      const SceneUid scene_val {std::stoi(token)};

      // Next column: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // The rest contains the coefficient values
      std::string remainder;
      std::getline(iss, remainder);

      raw_cuts.push_back(RawBoundaryCut {
          .name = std::move(cut_name),
          .iteration = iteration,
          .scene = scene_val,
          .rhs = rhs,
          .coeff_line = std::move(remainder),
      });
    }

    // ── Filter by max_iterations ────────────────────────────────
    const auto max_iters = options.boundary_max_iterations;
    if (max_iters > 0 && has_iteration_col) {
      std::set<int> distinct_iters;
      for (const auto& rc : raw_cuts) {
        distinct_iters.insert(rc.iteration);
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
                      { return !keep_iters.contains(rc.iteration); });
        SPDLOG_INFO(
            "SDDP: boundary cuts filtered to last {} "
            "iterations ({} cuts)",
            max_iters,
            raw_cuts.size());
      }
    }

    // ── Ensure the last phase has an alpha column ───────────────
    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      auto& state = scene_phase_states[scene][last_phase];
      if (state.alpha_col == ColIndex {unknown_index}) {
        auto& li = planning_lp.system(scene, last_phase).linear_interface();
        const auto sa = options.scale_alpha;
        state.alpha_col =
            li.add_col(gtopt::as_label("sddp", "alpha", scene, last_phase),
                       options.alpha_min / sa,
                       options.alpha_max / sa);
        li.set_obj_coeff(state.alpha_col, sa);
      }
    }

    // ── Add cuts to the LP ──────────────────────────────────────
    const auto scale_obj = planning_lp.options().scale_objective();

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
        for (int ci = 0; std::cmp_less(ci, header_col_map.size()); ++ci) {
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
        auto it = scene_uid_to_index.find(rc.scene);
        if (it == scene_uid_to_index.end()) {
          SPDLOG_TRACE(
              "Boundary cut '{}' scene UID {} not found in "
              "scene_array -- skipping",
              rc.name,
              rc.scene);
          continue;
        }
        scene_start = it->second;
        scene_end = it->second + SceneIndex {1};
      }

      for (const auto scene : iota_range(scene_start, scene_end)) {
        const auto& state = scene_phase_states[scene][last_phase];

        auto row = SparseRow {
            .name = label_maker.lp_label("bdr", rc.name, scene, last_phase),
            .lowb = rc.rhs * bc_discount / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = options.scale_alpha;

        auto& li = planning_lp.system(scene, last_phase).linear_interface();
        std::istringstream coeff_ss(rc.coeff_line);
        std::string token;
        for (int ci = 0; std::cmp_less(ci, header_col_map.size()); ++ci) {
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
      max_iteration = std::max(max_iteration, rc.iteration);
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
    const LabelMaker& label_maker,
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
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto& sys = planning_lp.planning().system;
    const auto scale_obj = planning_lp.options().scale_objective();

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
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

    auto get_col_map =
        [&](PhaseIndex phase) -> const std::vector<std::optional<ColIndex>>&
    {
      auto it = phase_col_maps.find(phase);
      if (it != phase_col_maps.end()) {
        return it->second;
      }

      // Build mapping for this phase by scanning state
      // variables
      const auto& svar_map = sim.state_variables(SceneIndex {0}, phase);

      std::vector<std::optional<ColIndex>> col_map;
      col_map.reserve(num_state_cols);

      for (int hi = kFixedCols; std::cmp_less(hi, headers.size()); ++hi) {
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

      auto [ins_it, _] = phase_col_maps.emplace(phase, std::move(col_map));
      return ins_it->second;
    };

    // Track which missing state variables have already been warned about.
    std::set<std::pair<PhaseIndex, int>> warned_missing_named;

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
      const IterationIndex iteration {std::stoi(token)};

      // Column 2: scene UID
      std::getline(iss, token, ',');
      [[maybe_unused]] const SceneUid scene_val {std::stoi(token)};

      // Column 3: phase UID
      std::getline(iss, token, ',');
      const PhaseUid phase_val {std::stoi(token)};

      // Column 4: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // Resolve phase UID -> PhaseIndex
      auto pit = phase_uid_to_index.find(phase_val);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "Named cuts: unknown phase UID {} in '{}', "
            "skipping cut '{}'",
            phase_val,
            filepath,
            cut_name);
        continue;
      }
      const auto phase = pit->second;

      // Ensure alpha variable exists for this phase in all
      // scenes
      for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
        auto& state = scene_phase_states[scene][phase];
        if (state.alpha_col == ColIndex {unknown_index}) {
          auto& li = planning_lp.system(scene, phase).linear_interface();
          const auto sa = options.scale_alpha;
          state.alpha_col =
              li.add_col(gtopt::as_label("sddp", "alpha", scene, phase),
                         options.alpha_min / sa,
                         options.alpha_max / sa);
          li.set_obj_coeff(state.alpha_col, sa);
        }
      }

      // Get the column map for this phase
      const auto& col_map = get_col_map(phase);

      // Parse coefficient values and build the cut row
      std::string remainder;
      std::getline(iss, remainder);

      // Pre-scan: check for missing state variables
      bool has_missing = false;
      {
        std::istringstream scan_ss(remainder);
        std::string tok;
        for (int ci = 0; std::cmp_less(ci, col_map.size()); ++ci) {
          if (!std::getline(scan_ss, tok, ',')) {
            break;
          }
          if (!col_map[ci].has_value() && !tok.empty() && std::stod(tok) != 0.0)
          {
            has_missing = true;
            if (!warned_missing_named.contains({phase, ci})) {
              warned_missing_named.emplace(phase, ci);
              SPDLOG_WARN(
                  "Named cuts: state variable '{}' not "
                  "found in phase {}; non-zero "
                  "coefficient {} (mode={})",
                  headers[kFixedCols + ci],
                  phase,
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

      for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
        const auto& state = scene_phase_states[scene][phase];

        auto row = SparseRow {
            .name = label_maker.lp_label("named_hs", cut_name, scene, phase),
            .lowb = rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = options.scale_alpha;

        auto& li = planning_lp.system(scene, phase).linear_interface();
        std::istringstream coeff_ss(remainder);
        std::string ctok;
        for (int ci = 0; std::cmp_less(ci, col_map.size()); ++ci) {
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
            row[*col_opt] = -coeff * scale
                / scale_obj;  // NOLINT(bugprone-unchecked-optional-access)
          }
        }

        li.add_row(row);
      }
      result.max_iteration = std::max(result.max_iteration, iteration);
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
