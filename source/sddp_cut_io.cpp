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

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Phase UID lookup helper ────────────────────────────────────────────────

auto build_phase_uid_map(const PlanningLP& planning_lp)
    -> flat_map<int, PhaseIndex>
{
  const auto& sim = planning_lp.simulation();
  const auto num_phases = static_cast<Index>(sim.phases().size());

  flat_map<int, PhaseIndex> phase_map;

  for (Index pi = 0; pi < num_phases; ++pi) {
    phase_map.emplace(static_cast<int>(sim.phases()[pi].uid()),
                      PhaseIndex {pi});
  }
  return phase_map;
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
/// Coefficients are in fully physical space:
///   coeff_csv = LP_coeff * scale_obj / col_scale
void write_cut_coefficients(std::ostream& ofs,
                            const StoredCut& cut,
                            const LinearInterface& li,
                            double scale_obj)
{
  for (const auto& [col, coeff] : cut.coefficients) {
    const auto scale = li.get_col_scale(ColIndex {col});
    ofs << "," << col << ":" << (coeff * scale_obj / scale);
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

}  // namespace

// ─── Save functions ─────────────────────────────────────────────────────────

auto save_cuts_csv(std::span<const StoredCut> cuts,
                   const PlanningLP& planning_lp,
                   const std::string& filepath) -> std::expected<void, Error>
{
  try {
    // Ensure parent directory exists before writing
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for writing: {}", filepath),
      });
    }

    const auto scale_obj = planning_lp.options().scale_objective();
    ofs << "# scale_objective=" << scale_obj << "\n";
    ofs << "phase,scene,name,rhs,coefficients\n";

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    for (const auto& cut : cuts) {
      // RHS in physical objective units
      ofs << cut.phase << "," << cut.scene << "," << cut.name << ","
          << (cut.rhs * scale_obj);

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
    ofs << "phase,scene,name,rhs,coefficients\n";

    // Build phase UID -> PhaseIndex lookup
    const auto phase_map = build_phase_uid_map(planning_lp);

    for (const auto& cut : cuts) {
      // RHS in physical objective units
      ofs << cut.phase << "," << cut.scene << "," << cut.name << ","
          << (cut.rhs * scale_obj);

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
                   const LabelMaker& label_maker) -> std::expected<int, Error>
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
    // Skip metadata comments (# ...) and the CSV header line
    while (std::getline(ifs, line)) {
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      // First non-empty, non-comment line is the header -- skip
      break;
    }

    int cuts_loaded = 0;
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto scale_obj = planning_lp.options().scale_objective();

    // Build phase UID -> PhaseIndex lookup
    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Process data lines
    while (std::getline(ifs, line)) {
      if (line.empty() || line.starts_with('#')) {
        continue;
      }

      // Parse CSV: phase,scene,name,rhs,col1:coeff1,...
      std::istringstream iss(line);
      std::string token;

      std::getline(iss, token, ',');
      const auto phase_val = std::stoi(token);

      std::getline(iss, token, ',');
      // scene is parsed but intentionally ignored: loaded cuts
      // are broadcast to all scenes as warm-start approximations.
      [[maybe_unused]] const auto scene_val = std::stoi(token);

      std::getline(iss, token, ',');
      const auto cut_name = token;

      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // RHS in CSV is in physical objective units; convert to LP
      // space.
      auto row = SparseRow {
          .name = label_maker.lp_label("loaded", cut_name),
          .lowb = rhs / scale_obj,
          .uppb = LinearProblem::DblMax,
      };

      // Collect raw physical-space coefficients
      // (col_index:coeff pairs)
      std::vector<std::pair<int, double>> raw_coeffs;
      while (std::getline(iss, token, ',')) {
        const auto colon = token.find(':');
        if (colon != std::string::npos) {
          const auto col = std::stoi(token.substr(0, colon));
          const auto coeff = std::stod(token.substr(colon + 1));
          raw_coeffs.emplace_back(col, coeff);
        }
      }

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

      // Add the loaded cut to all scenes for this phase.
      // Coefficients in the CSV are in fully physical space;
      // convert to LP space:
      //   LP_coeff = phys_coeff * col_scale / scale_objective
      for (Index si = 0; si < num_scenes; ++si) {
        auto& li =
            planning_lp.system(SceneIndex {si}, phase).linear_interface();
        auto scene_row = row;
        for (const auto& [col, coeff] : raw_coeffs) {
          const auto scale = li.get_col_scale(ColIndex {col});
          scene_row[ColIndex {col}] = coeff * scale / scale_obj;
        }
        li.add_row(scene_row);
      }
      ++cuts_loaded;
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {}", cuts_loaded, filepath);
    return cuts_loaded;

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
    -> std::expected<int, Error>
{
  int total_loaded = 0;

  if (!std::filesystem::exists(directory)) {
    return 0;  // No directory = no cuts to load (not an error)
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
      total_loaded += *result;
      SPDLOG_TRACE("SDDP hot-start: loaded {} cuts from {}", *result, filename);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load {}: {}",
                  filename,
                  result.error().message);
    }
  }

  return total_loaded;
}

// ─── Boundary (future-cost) cuts ────────────────────────────────────────────

auto load_boundary_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    const LabelMaker& label_maker,
    StrongIndexVector<SceneIndex,
                      StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<int, Error>
{
  // ── Mode check ────────────────────────────────────────────────
  const auto& mode = options.boundary_cuts_mode;
  if (mode == "noload") {
    SPDLOG_INFO("SDDP: boundary cuts mode is 'noload' -- skipping");
    return 0;
  }

  const bool separated = (mode == "separated");

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
    std::unordered_map<int, Index> scene_uid_to_index;
    for (Index si = 0; si < num_scenes; ++si) {
      scene_uid_to_index[static_cast<int>(sim.scenes()[si].uid())] = si;
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

      if (!found_col.has_value()) {
        SPDLOG_WARN(
            "Boundary cuts: state variable '{}' not found in "
            "the current model (it may have been removed or "
            "renamed since the cuts were saved); its "
            "coefficients will be ignored in the loaded cuts",
            hdr);
      }
      header_col_map.push_back(found_col);
    }

    // ── Pre-scan: collect all rows for max_iterations filtering ──
    struct RawBoundaryCut
    {
      std::string name;
      int iteration;
      int scene;
      double rhs;
      std::string coeff_line;
    };

    std::vector<RawBoundaryCut> raw_cuts;
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      int iteration = 0;
      if (has_iteration_col) {
        // Column 1: iteration
        std::getline(iss, token, ',');
        iteration = std::stoi(token);
      }

      // Next column: scene UID
      std::getline(iss, token, ',');
      const auto scene_val = std::stoi(token);

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
    for (Index si = 0; si < num_scenes; ++si) {
      const auto scene = SceneIndex {si};
      auto& state = scene_phase_states[scene][last_phase];
      if (state.alpha_col == ColIndex {unknown_index}) {
        auto& li = planning_lp.system(scene, last_phase).linear_interface();
        state.alpha_col =
            li.add_col(label_maker.lp_label("sddp", "alpha", scene, last_phase),
                       options.alpha_min,
                       options.alpha_max);
        li.set_obj_coeff(state.alpha_col, 1.0);
      }
    }

    // ── Add cuts to the LP ──────────────────────────────────────
    const auto scale_obj = planning_lp.options().scale_objective();
    int cuts_loaded = 0;
    for (const auto& rc : raw_cuts) {
      // Determine which scenes get this cut
      Index scene_start = 0;
      Index scene_end = num_scenes;
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
        scene_end = it->second + 1;
      }

      for (Index si = scene_start; si < scene_end; ++si) {
        const auto scene = SceneIndex {si};
        const auto& state = scene_phase_states[scene][last_phase];

        auto row = SparseRow {
            .name = label_maker.lp_label("bdr", rc.name),
            .lowb = rc.rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = 1.0;

        auto& li = planning_lp.system(scene, last_phase).linear_interface();
        std::istringstream coeff_ss(rc.coeff_line);
        std::string token;
        for (const auto& col_opt : header_col_map) {
          if (!std::getline(coeff_ss, token, ',')) {
            break;
          }
          if (!col_opt.has_value()) {
            continue;
          }
          const auto coeff = std::stod(token);
          if (coeff != 0.0) {
            const auto scale = li.get_col_scale(*col_opt);
            row[*col_opt] = -coeff * scale / scale_obj;
          }
        }

        li.add_row(row);
      }
      ++cuts_loaded;
    }

    SPDLOG_INFO(
        "SDDP: loaded {} boundary cuts from {} (mode={}, "
        "max_iters={})",
        cuts_loaded,
        filepath,
        mode,
        max_iters);
    return cuts_loaded;

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
        scene_phase_states) -> std::expected<int, Error>
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
    std::unordered_map<Index, std::vector<std::optional<ColIndex>>>
        phase_col_maps;

    auto get_col_map =
        [&](PhaseIndex phase) -> const std::vector<std::optional<ColIndex>>&
    {
      auto it = phase_col_maps.find(static_cast<Index>(phase));
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

        if (!found_col.has_value()) {
          SPDLOG_TRACE(
              "Named cuts: could not find state variable "
              "for header '{}' in phase {}; column ignored",
              hdr,
              static_cast<Index>(phase));
        }
        col_map.push_back(found_col);
      }

      auto [ins_it, _] =
          phase_col_maps.emplace(static_cast<Index>(phase), std::move(col_map));
      return ins_it->second;
    };

    // ── Read all cut rows ───────────────────────────────────────
    int cuts_loaded = 0;
    std::string line;
    while (std::getline(ifs, line)) {
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
      [[maybe_unused]] const auto iteration = std::stoi(token);

      // Column 2: scene UID
      std::getline(iss, token, ',');
      [[maybe_unused]] const auto scene_val = std::stoi(token);

      // Column 3: phase UID
      std::getline(iss, token, ',');
      const auto phase_val = std::stoi(token);

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
      for (Index si = 0; si < num_scenes; ++si) {
        const auto scene = SceneIndex {si};
        auto& state = scene_phase_states[scene][phase];
        if (state.alpha_col == ColIndex {unknown_index}) {
          auto& li = planning_lp.system(scene, phase).linear_interface();
          state.alpha_col =
              li.add_col(label_maker.lp_label("sddp", "alpha", scene, phase),
                         options.alpha_min,
                         options.alpha_max);
          li.set_obj_coeff(state.alpha_col, 1.0);
        }
      }

      // Get the column map for this phase
      const auto& col_map = get_col_map(phase);

      // Parse coefficient values and build the cut row
      std::string remainder;
      std::getline(iss, remainder);

      for (Index si = 0; si < num_scenes; ++si) {
        const auto scene = SceneIndex {si};
        const auto& state = scene_phase_states[scene][phase];

        auto row = SparseRow {
            .name = label_maker.lp_label("named_hs", cut_name),
            .lowb = rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = 1.0;

        auto& li = planning_lp.system(scene, phase).linear_interface();
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
            row[*col_opt] = -coeff * scale / scale_obj;
          }
        }

        li.add_row(row);
      }
      ++cuts_loaded;
    }

    SPDLOG_INFO(
        "SDDP: loaded {} named hot-start cuts from {}", cuts_loaded, filepath);
    return cuts_loaded;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading named cuts from {}: {}", filepath, e.what()),
    });
  }
}

}  // namespace gtopt
