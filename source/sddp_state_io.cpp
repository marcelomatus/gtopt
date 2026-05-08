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
        const auto& li = sys.linear_interface();
        // No ensure_lp_built() here: under low-memory modes the cell
        // was released with Phase-2a cached col_sol / col_cost via
        // `LinearInterface::release_backend()`, and the getters route
        // to those cached vectors when `m_backend_released_ == true`.
        // Forcing a reload would fire `rebuild_in_place()` under
        // rebuild mode, which flattens fresh — leaving the backend
        // LIVE but UNSOLVED, clobbering `is_optimal()` for the
        // subsequent `PlanningLP::write_out` pass (write_out would
        // then short-circuit and emit no per-element parquet).  Under
        // `off` the backend stays alive throughout and is already
        // readable; this call is a no-op there too.

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

}  // namespace gtopt
