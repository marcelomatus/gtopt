/**
 * @file      sddp_state_io.cpp
 * @brief     State variable column I/O for SDDP solver
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the state variable save/load free functions declared in
 * sddp_state_io.hpp.  Extracted from sddp_cut_io.cpp to improve
 * modularity.
 */

#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_state_io.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

auto save_state_csv(PlanningLP& planning_lp,
                    const std::string& filepath,
                    IterationIndex iteration_index)
    -> std::expected<void, Error>
{
  try {
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open state file for writing: {}", filepath),
      });
    }

    ofs << as_label<'='>("# iteration", iteration_index) << "\n";
    ofs << "name,phase,scene,value,rcost\n";

    const auto& sim = planning_lp.simulation();
    [[maybe_unused]] int count = 0;

    // Iterate the state variable map directly — no column name
    // infrastructure needed.  Each StateVariable carries its ColIndex,
    // scale, and enough metadata to reconstruct its label.
    for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
      for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
        auto& sys = planning_lp.system(si, pi);
        // Under low-memory modes the backend may be released at this
        // point; force a reload so the solution vectors are readable.
        sys.ensure_lp_built();
        const auto& li = sys.linear_interface();

        if (!li.is_optimal()) {
          continue;
        }

        const auto col_sol = li.get_col_sol();
        const auto col_rc = li.get_col_cost();
        const auto ncols = li.numcols_as_index();
        const auto rc_upper = std::min(ncols, col_index_size(col_rc));
        const auto phase_uid = phase.uid();
        const auto scene_uid = scene.uid();

        const auto& sv_map = sim.state_variables(si, pi);
        for (const auto& [key, sv] : sv_map) {
          const auto ci = sv.col();
          if (ci >= ncols) {
            continue;
          }
          constexpr LabelMaker lm {LpNamesLevel::all};
          const auto label = lm.make_col_label(SparseCol {
              .class_name = key.class_name,
              .variable_name = key.col_name,
              .variable_uid = key.uid,
              .context = sv.context(),
          });
          if (label.empty()) {
            continue;
          }
          const auto phys_val = col_sol[ci];
          const auto rc = ci < rc_upper ? col_rc[ci] : 0.0;

          ofs << as_label<','>(label, phase_uid, scene_uid, phys_val, rc)
              << "\n";
          ++count;
        }
      }
    }

    SPDLOG_TRACE("SDDP: saved {} state columns to {}", count, filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error saving state to {}: {}", filepath, e.what()),
    });
  }
}

auto load_state_csv(PlanningLP& planning_lp, const std::string& filepath)
    -> std::expected<void, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open state file for reading: {}", filepath),
      });
    }

    const auto phase_map = build_phase_uid_map(planning_lp);
    const auto scene_map = build_scene_uid_map(planning_lp);

    // Build per-(scene, phase) warm vectors, initialized from current LP
    // column count.
    struct WarmEntry
    {
      SceneIndex scene_index;
      PhaseIndex phase_index;
      std::vector<double> col_sol;
      bool has_data {false};
    };
    std::vector<WarmEntry> entries;

    // Build per-(scene, phase) name→(ColIndex, scale) maps from state
    // variables — no dependency on LinearInterface column name vectors.
    struct ColResolveEntry
    {
      ColIndex col;
      double scale;
    };
    using col_resolve_map_t = std::unordered_map<std::string, ColResolveEntry>;
    std::vector<col_resolve_map_t> resolve_maps;

    const auto& sim = planning_lp.simulation();
    for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
      for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
        const auto& li = planning_lp.system(si, pi).linear_interface();
        const auto ncols = li.get_numcols();
        entries.push_back(WarmEntry {
            .scene_index = si,
            .phase_index = pi,
            .col_sol = std::vector<double>(ncols, 0.0),
        });

        // Populate resolve map from state variables
        col_resolve_map_t rmap;
        const auto& sv_map = sim.state_variables(si, pi);
        rmap.reserve(sv_map.size());
        for (const auto& [key, sv] : sv_map) {
          constexpr LabelMaker lm {LpNamesLevel::all};
          auto label = lm.make_col_label(SparseCol {
              .class_name = key.class_name,
              .variable_name = key.col_name,
              .variable_uid = key.uid,
              .context = sv.context(),
          });
          if (!label.empty()) {
            rmap.emplace(std::move(label),
                         ColResolveEntry {sv.col(), sv.var_scale()});
          }
        }
        resolve_maps.push_back(std::move(rmap));
      }
    }

    const auto nphases = sim.phases().size();

    auto find_entry_index = [&](SceneIndex si,
                                PhaseIndex pi) -> std::optional<size_t>
    {
      const auto idx =
          static_cast<size_t>(si) * nphases + static_cast<size_t>(pi);
      if (idx < entries.size() && entries[idx].scene_index == si
          && entries[idx].phase_index == pi)
      {
        return idx;
      }
      return std::nullopt;
    };

    std::string line;
    int loaded = 0;

    while (std::getline(ifs, line)) {
      strip_cr(line);
      if (line.empty() || line[0] == '#') {
        continue;
      }
      // Skip header
      if (line.starts_with("name,")) {
        continue;
      }

      // Parse: name,phase_uid,scene_uid,value,rcost
      std::istringstream ss(line);
      std::string name;
      std::string phase_str;
      std::string scene_str;
      std::string value_str;

      if (!std::getline(ss, name, ',') || !std::getline(ss, phase_str, ',')
          || !std::getline(ss, scene_str, ',')
          || !std::getline(ss, value_str, ','))
      {
        continue;
      }

      uid_t pu = 0;
      uid_t su = 0;
      double value = 0.0;
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      auto [p1, e1] = std::from_chars(
          phase_str.data(), phase_str.data() + phase_str.size(), pu);
      auto [p2, e2] = std::from_chars(
          scene_str.data(), scene_str.data() + scene_str.size(), su);
      auto [p3, e3] = std::from_chars(
          value_str.data(), value_str.data() + value_str.size(), value);
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

      if (e1 != std::errc {} || e2 != std::errc {} || e3 != std::errc {}) {
        continue;
      }

      const PhaseUid phase_uid = make_uid<Phase>(pu);
      const SceneUid scene_uid = make_uid<Scene>(su);

      // Resolve phase and scene indices
      auto pit = phase_map.find(phase_uid);
      auto sit = scene_map.find(scene_uid);
      if (pit == phase_map.end() || sit == scene_map.end()) {
        continue;
      }

      auto entry_idx = find_entry_index(sit->second, pit->second);
      if (!entry_idx) {
        continue;
      }

      auto& entry = entries[*entry_idx];
      const auto& rmap = resolve_maps[*entry_idx];

      // Resolve column name via state variable map
      auto cit = rmap.find(name);
      if (cit == rmap.end()) {
        continue;
      }

      const auto& [col, scale] = cit->second;
      const auto col_idx = static_cast<size_t>(col);
      if (col_idx >= entry.col_sol.size()) {
        continue;
      }

      // Convert physical value back to LP units
      entry.col_sol[col_idx] = (scale != 0.0) ? value / scale : value;
      entry.has_data = true;
      ++loaded;
    }

    // Inject warm solutions into each LinearInterface
    for (auto& e : entries) {
      if (e.has_data) {
        StrongIndexVector<ColIndex, double> warm(e.col_sol.begin(),
                                                 e.col_sol.end());
        planning_lp.system(e.scene_index, e.phase_index)
            .linear_interface()
            .set_warm_col_sol(std::move(warm));
      }
    }

    SPDLOG_INFO("SDDP: loaded {} state columns from {}", loaded, filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error loading state from {}: {}", filepath, e.what()),
    });
  }
}

}  // namespace gtopt
