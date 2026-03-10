/**
 * @file      planning_solver.cpp
 * @brief     Implementation of PlanningSolver interface and MonolithicSolver
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <format>

#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── MonolithicSolver ───────────────────────────────────────────────────────

auto MonolithicSolver::solve(PlanningLP& planning_lp, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  WorkPoolConfig pool_config {};

  const double cpu_factor = 1.25;
  pool_config.max_threads = static_cast<int>(
      std::lround((cpu_factor * std::thread::hardware_concurrency())));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  AdaptiveWorkPool pool(pool_config);
  pool.start();

  try {
    using future_t = std::future<std::expected<void, Error>>;

    std::vector<future_t> futures;
    futures.reserve(planning_lp.systems().size());

    for (auto&& [scene_index, phase_systems] :
         enumerate<SceneIndex>(planning_lp.systems()))
    {
      auto result = pool.submit(
          [&]
          {
            return planning_lp.resolve_scene_phases(
                scene_index, phase_systems, opts);
          });
      futures.push_back(std::move(result.value()));
    }

    // Check all futures for errors
    for (auto& future : futures) {
      if (auto result = future.get(); !result) {
        return std::unexpected(std::move(result.error()));
      }
    }

    return futures.size();  // Return number of successfully processed scenes

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message = std::format("Unexpected error in resolve: {}", e.what()),
    });
  }
}

// ─── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<PlanningSolver> make_planning_solver(
    std::string_view solver_type,
    std::string_view cut_sharing_mode,
    std::string_view cut_directory,
    std::string_view log_directory,
    bool enable_api,
    std::string_view api_output_dir)
{
  if (solver_type == "sddp") {
    SDDPOptions sddp_opts;
    sddp_opts.cut_sharing = parse_cut_sharing_mode(cut_sharing_mode);
    // Use cut_directory for both input and output cut files
    const auto cuts_path =
        (std::filesystem::path(cut_directory) / sddp_file::combined_cuts)
            .string();
    sddp_opts.cuts_output_file = cuts_path;
    sddp_opts.log_directory = std::string(log_directory);
    sddp_opts.enable_api = enable_api;
    // api_status_file is left empty; SDDPSolver will derive it from
    // api_output_dir at solve time when api_status_file is empty.
    if (!api_output_dir.empty()) {
      sddp_opts.api_status_file =
          (std::filesystem::path(api_output_dir) / "sddp_status.json").string();
    }
    return std::make_unique<SDDPPlanningSolver>(std::move(sddp_opts));
  }

  // Default: monolithic
  return std::make_unique<MonolithicSolver>();
}

}  // namespace gtopt
