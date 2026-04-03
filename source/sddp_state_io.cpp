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

#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_state_io.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

auto save_state_csv(const PlanningLP& planning_lp,
                    const std::string& filepath,
                    IterationIndex iteration) -> std::expected<void, Error>
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

    ofs << "# iteration=" << iteration << "\n";
    ofs << "name,phase,scene,value,rcost\n";

    const auto& sim = planning_lp.simulation();
    [[maybe_unused]] int count = 0;

    for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
      for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
        const auto& li = planning_lp.system(si, pi).linear_interface();

        if (!li.is_optimal()) {
          continue;
        }

        const auto col_sol = li.get_col_sol_raw();
        const auto col_rc = li.get_col_cost_raw();
        const auto& names = li.col_index_to_name();
        const auto ncols = li.get_numcols();
        const auto phase_uid = phase.uid();
        const auto scene_uid = scene.uid();

        for (size_t c = 0; c < ncols && c < names.size(); ++c) {
          const auto ci = ColIndex {static_cast<int>(c)};
          if (names[ci].empty()) {
            continue;
          }
          const auto scale = li.get_col_scale(ci);
          const auto phys_val = col_sol[ci] * scale;
          const auto rc = c < static_cast<size_t>(col_rc.size())
              ? static_cast<double>(col_rc[ci])
              : 0.0;

          ofs << names[ci] << "," << phase_uid << "," << scene_uid << ","
              << phys_val << "," << rc << "\n";
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
      SceneIndex scene;
      PhaseIndex phase;
      std::vector<double> col_sol;
      bool has_data {false};
    };
    std::vector<WarmEntry> entries;

    const auto& sim = planning_lp.simulation();
    for (auto&& [si, _sc] : enumerate<SceneIndex>(sim.scenes())) {
      for (auto&& [pi, _ph] : enumerate<PhaseIndex>(sim.phases())) {
        const auto ncols =
            planning_lp.system(si, pi).linear_interface().get_numcols();
        entries.push_back(WarmEntry {
            .scene = si,
            .phase = pi,
            .col_sol = std::vector<double>(ncols, 0.0),
        });
      }
    }

    auto find_entry = [&](SceneIndex si, PhaseIndex pi) -> WarmEntry*
    {
      for (auto& e : entries) {
        if (e.scene == si && e.phase == pi) {
          return &e;
        }
      }
      return nullptr;
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

      int phase_uid = 0;
      int scene_uid = 0;
      double value = 0.0;
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      auto [p1, e1] = std::from_chars(
          phase_str.data(), phase_str.data() + phase_str.size(), phase_uid);
      auto [p2, e2] = std::from_chars(
          scene_str.data(), scene_str.data() + scene_str.size(), scene_uid);
      auto [p3, e3] = std::from_chars(
          value_str.data(), value_str.data() + value_str.size(), value);
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

      if (e1 != std::errc {} || e2 != std::errc {} || e3 != std::errc {}) {
        continue;
      }

      // Resolve phase and scene indices
      auto pit = phase_map.find(PhaseUid {phase_uid});
      auto sit = scene_map.find(SceneUid {scene_uid});
      if (pit == phase_map.end() || sit == scene_map.end()) {
        continue;
      }

      auto* entry = find_entry(sit->second, pit->second);
      if (entry == nullptr) {
        continue;
      }

      // Resolve column name to index
      const auto& li =
          planning_lp.system(entry->scene, entry->phase).linear_interface();
      const auto& col_map = li.col_name_map();
      auto cit = col_map.find(name);
      if (cit == col_map.end()) {
        continue;
      }

      const auto col_idx = static_cast<size_t>(cit->second);
      if (col_idx >= entry->col_sol.size()) {
        continue;
      }

      // Convert physical value back to LP units
      const auto scale = li.get_col_scale(ColIndex {static_cast<int>(col_idx)});
      entry->col_sol[col_idx] = (scale != 0.0) ? value / scale : value;
      entry->has_data = true;
      ++loaded;
    }

    // Inject warm solutions into each LinearInterface
    for (auto& e : entries) {
      if (e.has_data) {
        StrongIndexVector<ColIndex, double> warm(e.col_sol.begin(),
                                                 e.col_sol.end());
        planning_lp.system(e.scene, e.phase)
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
