/**
 * @file      planning_solver.cpp
 * @brief     Implementation of PlanningSolver interface and MonolithicSolver
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <format>

#include <gtopt/options_lp.hpp>
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

std::unique_ptr<PlanningSolver> make_planning_solver(const OptionsLP& options)
{
  if (options.sddp_solver_type() == "sddp") {
    SDDPOptions sddp_opts;

    // Iteration control
    sddp_opts.max_iterations = options.sddp_max_iterations();
    sddp_opts.convergence_tol = options.sddp_convergence_tol();

    // Advanced tuning
    sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
    sddp_opts.alpha_min = options.sddp_alpha_min();
    sddp_opts.alpha_max = options.sddp_alpha_max();

    // Cut sharing and files
    sddp_opts.cut_sharing =
        parse_cut_sharing_mode(options.sddp_cut_sharing_mode());
    const auto cut_dir = options.sddp_cut_directory();
    sddp_opts.cuts_output_file =
        (std::filesystem::path(cut_dir) / sddp_file::combined_cuts).string();

    const auto cuts_input = options.sddp_cuts_input_file();
    if (!cuts_input.empty()) {
      sddp_opts.cuts_input_file = cuts_input;
    }

    // Sentinel
    const auto sentinel = options.sddp_sentinel_file();
    if (!sentinel.empty()) {
      sddp_opts.sentinel_file = sentinel;
    }

    // Logging and API
    sddp_opts.log_directory = std::string(options.log_directory());
    sddp_opts.enable_api = options.sddp_api_enabled();
    const auto output_dir = options.output_directory();
    if (!output_dir.empty()) {
      sddp_opts.api_status_file =
          (std::filesystem::path(output_dir) / "sddp_status.json").string();
    }

    return std::make_unique<SDDPPlanningSolver>(std::move(sddp_opts));
  }

  // Default: monolithic
  return std::make_unique<MonolithicSolver>();
}

}  // namespace gtopt
