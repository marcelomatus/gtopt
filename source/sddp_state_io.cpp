/**
 * @file      sddp_state_io.cpp
 * @brief     State variable column I/O for SDDP solver
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the state variable save free function declared in
 * sddp_state_io.hpp.  Extracted from sddp_cut_io.cpp to improve
 * modularity.
 *
 * The CSV body is written via `arrow::csv::WriteCSV` so the float
 * representation is the Grisu shortest-round-trip form (bit-exact via
 * `std::strtod`), matching the precision the legacy `as_label<','>` path
 * produced through `std::to_chars`.  The file is a pure tabular CSV
 * (header + rows); the producing iteration is identified by the log
 * line in `sddp_cut_store.cpp` next to the save call, not by an in-file
 * marker.
 */

#include <filesystem>
#include <format>
#include <vector>

#include <arrow/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
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

namespace
{

auto make_io_error(const std::string& filepath, const std::string& detail)
    -> Error
{
  return Error {
      .code = ErrorCode::FileIOError,
      .message = std::format("{}: {}", filepath, detail),
  };
}

}  // namespace

auto save_state_csv(PlanningLP& planning_lp, const std::string& filepath)
    -> std::expected<void, Error>
{
  try {
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    // Collect rows into typed vectors for one-shot Arrow assembly.
    // Each state variable in every (scene, phase) cell yields at most
    // one row, so total size is bounded by Σ|state_variables(si,pi)|.
    std::vector<std::string> names;
    std::vector<int32_t> phases;
    std::vector<int32_t> scenes;
    std::vector<double> values;
    std::vector<double> rcosts;

    const auto& sim = planning_lp.simulation();

    for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
      for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
        auto& sys = planning_lp.system(si, pi);
        const auto& li = sys.linear_interface();
        // No ensure_lp_built() here: under low-memory modes the cell
        // was released with Phase-2a cached col_sol / col_cost via
        // `LinearInterface::release_backend()`, and the getters route
        // to those cached vectors when `m_backend_released_ == true`.
        // Forcing a reload would reconstruct the backend from the
        // snapshot under compress mode — leaving the backend LIVE
        // but UNSOLVED, clobbering `is_optimal()` for the subsequent
        // `PlanningLP::write_out` pass.  Under `off` the backend
        // stays alive throughout and is already readable; this call
        // is a no-op there too.

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
          auto label = lm.make_col_label(SparseCol {
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

          names.push_back(std::move(label));
          phases.push_back(static_cast<int32_t>(phase_uid.value()));
          scenes.push_back(static_cast<int32_t>(scene_uid.value()));
          values.push_back(phys_val);
          rcosts.push_back(rc);
        }
      }
    }

    // Build Arrow arrays.  Reserved capacity matches the collected
    // row count so each builder pre-allocates exactly once.
    const auto nrows = names.size();

    arrow::StringBuilder name_b;
    arrow::Int32Builder phase_b;
    arrow::Int32Builder scene_b;
    arrow::DoubleBuilder val_b;
    arrow::DoubleBuilder rc_b;
    if (auto s = name_b.Reserve(static_cast<int64_t>(nrows)); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = phase_b.Reserve(static_cast<int64_t>(nrows)); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = scene_b.Reserve(static_cast<int64_t>(nrows)); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = val_b.Reserve(static_cast<int64_t>(nrows)); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = rc_b.Reserve(static_cast<int64_t>(nrows)); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = name_b.AppendValues(names); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = phase_b.AppendValues(phases); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = scene_b.AppendValues(scenes); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = val_b.AppendValues(values); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = rc_b.AppendValues(rcosts); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }

    std::shared_ptr<arrow::Array> name_a;
    std::shared_ptr<arrow::Array> phase_a;
    std::shared_ptr<arrow::Array> scene_a;
    std::shared_ptr<arrow::Array> val_a;
    std::shared_ptr<arrow::Array> rc_a;
    if (auto s = name_b.Finish(&name_a); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = phase_b.Finish(&phase_a); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = scene_b.Finish(&scene_a); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = val_b.Finish(&val_a); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = rc_b.Finish(&rc_a); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }

    auto schema = arrow::schema({
        arrow::field("name", arrow::utf8()),
        arrow::field("phase", arrow::int32()),
        arrow::field("scene", arrow::int32()),
        arrow::field("value", arrow::float64()),
        arrow::field("rcost", arrow::float64()),
    });
    auto table = arrow::Table::Make(std::move(schema),
                                    {name_a, phase_a, scene_a, val_a, rc_a});

    auto open_result = arrow::io::FileOutputStream::Open(filepath);
    if (!open_result.ok()) {
      return std::unexpected(
          make_io_error(filepath, open_result.status().ToString()));
    }
    const auto& out = *open_result;

    const auto write_options = arrow::csv::WriteOptions::Defaults();
    if (auto s = arrow::csv::WriteCSV(*table, write_options, out.get());
        !s.ok())
    {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = out->Close(); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }

    SPDLOG_TRACE("SDDP: saved {} state columns to {}", nrows, filepath);
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
