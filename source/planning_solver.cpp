/**
 * @file      planning_solver.cpp
 * @brief     Implementation of PlanningSolver interface and MonolithicSolver
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/solver_monitor.hpp>
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

  // ── Monitoring setup ──
  const auto solve_start = std::chrono::steady_clock::now();
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());

  std::atomic<int> scenes_done {0};
  std::mutex times_mutex;
  std::vector<double> scene_times(num_scenes, 0.0);

  SolverMonitor monitor(api_update_interval);
  if (enable_api && !api_status_file.empty()) {
    monitor.start(pool, solve_start, "MonolithicMonitor");
  }

  try {
    using future_t = std::future<std::expected<void, Error>>;

    std::vector<future_t> futures;
    futures.reserve(planning_lp.systems().size());

    for (auto&& [scene_index, phase_systems] :
         enumerate<SceneIndex>(planning_lp.systems()))
    {
      auto result = pool.submit(
          [&, scene_index]
          {
            const auto t_scene = std::chrono::steady_clock::now();
            auto r = planning_lp.resolve_scene_phases(
                scene_index, phase_systems, opts);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - t_scene)
                    .count();
            {
              const std::scoped_lock lk(times_mutex);
              scene_times[static_cast<std::size_t>(scene_index)] = elapsed;
            }
            ++scenes_done;
            return r;
          });
      futures.push_back(std::move(result.value()));
    }

    // Check all futures for errors
    for (auto& future : futures) {
      if (auto result = future.get(); !result) {
        monitor.stop();
        return std::unexpected(std::move(result.error()));
      }
    }

    // ── Write monitoring status file ──
    monitor.stop();
    if (enable_api && !api_status_file.empty()) {
      const double elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - solve_start)
                                 .count();

      std::string json;
      json.reserve(1024);
      json += "{\n";
      json += std::format("  \"version\": 1,\n");
      json += std::format("  \"status\": \"done\",\n");
      json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed);
      json += std::format("  \"total_scenes\": {},\n", num_scenes);
      json += std::format("  \"scenes_done\": {},\n", scenes_done.load());

      json += "  \"scene_times\": [";
      {
        const std::scoped_lock lk(times_mutex);
        for (int i = 0; i < num_scenes; ++i) {
          if (i > 0) {
            json += ", ";
          }
          json += std::format("{:.4f}", scene_times[i]);
        }
      }
      json += "],\n";

      monitor.append_history_json(json);
      json += "}\n";

      SolverMonitor::write_status(json, api_status_file);
    }

    return static_cast<int>(futures.size());

  } catch (const std::exception& e) {
    monitor.stop();
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
    std::string_view api_output_dir,
    int max_iterations,
    double convergence_tol,
    double elastic_penalty,
    std::string_view elastic_mode)
{
  if (solver_type == "sddp") {
    SDDPOptions sddp_opts;
    sddp_opts.cut_sharing = parse_cut_sharing_mode(cut_sharing_mode);
    sddp_opts.elastic_filter_mode = parse_elastic_filter_mode(elastic_mode);
    sddp_opts.max_iterations = max_iterations;
    sddp_opts.convergence_tol = convergence_tol;
    sddp_opts.elastic_penalty = elastic_penalty;
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
  auto solver = std::make_unique<MonolithicSolver>();
  if (enable_api && !api_output_dir.empty()) {
    solver->enable_api = true;
    solver->api_status_file =
        (std::filesystem::path(api_output_dir) / "monolithic_status.json")
            .string();
  }
  return solver;
}

}  // namespace gtopt
