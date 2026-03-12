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

#include <gtopt/options_lp.hpp>
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
  auto pool = make_solver_work_pool();

  // ── Monitoring setup ──
  const auto solve_start = std::chrono::steady_clock::now();
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());

  SPDLOG_INFO("MonolithicSolver: starting {} scene(s)", num_scenes);

  std::atomic<int> scenes_done {0};
  std::mutex times_mutex;
  std::vector<double> scene_times(num_scenes, 0.0);

  SolverMonitor monitor(api_update_interval);
  if (enable_api && !api_status_file.empty()) {
    monitor.start(*pool, solve_start, "MonolithicMonitor");
  }

  try {
    using future_t = std::future<std::expected<void, Error>>;

    std::vector<future_t> futures;
    futures.reserve(planning_lp.systems().size());

    for (auto&& [scene_index, phase_systems] :
         enumerate<SceneIndex>(planning_lp.systems()))
    {
      auto result = pool->submit(
          [&, scene_index]
          {
            SPDLOG_TRACE("MonolithicSolver: scene {} starting", scene_index);
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
            SPDLOG_INFO("MonolithicSolver: scene {} done in {:.3f}s ({}/{})",
                        scene_index,
                        elapsed,
                        scenes_done.load(),
                        num_scenes);
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
    {
      const double total_elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - solve_start)
              .count();
      SPDLOG_INFO("MonolithicSolver: all {} scene(s) done in {:.3f}s",
                  num_scenes,
                  total_elapsed);
    }
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

std::unique_ptr<PlanningSolver> make_planning_solver(const OptionsLP& options)
{
  if (options.sddp_solver_type() == "sddp") {
    SDDPOptions sddp_opts;

    // Iteration control
    sddp_opts.max_iterations = options.sddp_max_iterations();
    sddp_opts.convergence_tol = options.sddp_convergence_tol();

    // Advanced tuning
    sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
    sddp_opts.elastic_filter_mode =
        parse_elastic_filter_mode(options.sddp_elastic_mode());
    sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
    sddp_opts.num_apertures = options.sddp_num_apertures();
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

    // Sentinel: honour the user-configured path when explicitly set.
    const auto sentinel = options.sddp_sentinel_file();
    const auto output_dir = options.output_directory();
    if (!sentinel.empty()) {
      sddp_opts.sentinel_file = sentinel;
    }

    // Logging and API
    sddp_opts.log_directory = std::string(options.log_directory());
    sddp_opts.enable_api = options.sddp_api_enabled();
    if (!output_dir.empty()) {
      sddp_opts.api_status_file =
          (std::filesystem::path(output_dir) / "sddp_status.json").string();
      // The monitoring API stop-request file lets the webservice (or any
      // external tool) trigger a graceful stop without using the raw sentinel
      // file.  The solver checks: sentinel_file exists || stop_request exists.
      sddp_opts.api_stop_request_file =
          (std::filesystem::path(output_dir) / sddp_file::stop_request)
              .string();
    }

    return std::make_unique<SDDPPlanningSolver>(std::move(sddp_opts));
  }

  // Default: monolithic
  auto solver = std::make_unique<MonolithicSolver>();
  const auto output_dir_m = options.output_directory();
  if (options.sddp_api_enabled() && !output_dir_m.empty()) {
    solver->enable_api = true;
    solver->api_status_file =
        (std::filesystem::path(output_dir_m) / "monolithic_status.json")
            .string();
  }
  return solver;
}

}  // namespace gtopt
