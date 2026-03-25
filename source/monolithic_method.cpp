/**
 * @file      monolithic_method.cpp
 * @brief     Implementation of MonolithicMethod::solve()
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <vector>

#include <gtopt/label_maker.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto MonolithicMethod::solve(PlanningLP& planning_lp, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  auto pool = make_solver_work_pool();

  // ── Monitoring setup ──
  const auto solve_start = std::chrono::steady_clock::now();
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());

  SPDLOG_INFO("MonolithicMethod: starting {} scene(s)", num_scenes);

  // ── Boundary cuts ──
  if (!boundary_cuts_file.empty()
      && boundary_cuts_mode != BoundaryCutsMode::noload)
  {
    SPDLOG_INFO("MonolithicMethod: loading boundary cuts from '{}'",
                boundary_cuts_file);

    // Build temporary SDDPOptions with boundary cut settings
    SDDPOptions bc_opts;
    bc_opts.boundary_cuts_file = boundary_cuts_file;
    bc_opts.boundary_cuts_mode = boundary_cuts_mode;
    bc_opts.boundary_max_iterations = boundary_max_iterations;

    // Build per-scene phase state info (alpha columns + outgoing links)
    const auto num_phases_bc = planning_lp.systems().empty()
        ? 0UZ
        : planning_lp.systems().front().size();
    StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, PhaseStateInfo>>
        scene_phase_states;
    scene_phase_states.resize(
        static_cast<std::size_t>(num_scenes),
        StrongIndexVector<PhaseIndex, PhaseStateInfo>(num_phases_bc));

    const LabelMaker label_maker(planning_lp.options());

    auto bc_result = load_boundary_cuts_csv(planning_lp,
                                            boundary_cuts_file,
                                            bc_opts,
                                            label_maker,
                                            scene_phase_states);
    if (bc_result) {
      SPDLOG_INFO(
          "MonolithicMethod: loaded {} boundary cuts (max iteration {})",
          bc_result->count,
          bc_result->max_iteration);
    } else {
      SPDLOG_WARN("MonolithicMethod: failed to load boundary cuts: {}",
                  bc_result.error().message);
    }
  }

  auto effective_opts = opts;

  std::atomic<int> scenes_done {0};
  std::mutex times_mutex;
  std::vector<double> scene_times(num_scenes, 0.0);

  SolverMonitor monitor(api_update_interval);
  if (enable_api && !api_status_file.empty()) {
    monitor.start(*pool, solve_start, "MonolithicMonitor");
  }

  try {
    using future_t = std::future<std::expected<void, Error>>;

    LpDebugWriter lp_writer(lp_debug ? lp_debug_directory : std::string {},
                            lp_debug_compression,
                            pool.get());
    if (lp_writer.is_active()) {
      std::filesystem::create_directories(lp_debug_directory);
      const auto lp_stem =
          (std::filesystem::path(lp_debug_directory) / "gtopt_lp").string();
      for (const auto& phase_systems : planning_lp.systems()) {
        for (const auto& system : phase_systems) {
          if (auto lp_result = system.write_lp(lp_stem)) {
            lp_writer.compress_async(*lp_result);
          } else {
            spdlog::warn("{}", lp_result.error().message);
          }
        }
      }
      spdlog::info(
          "MonolithicMethod: wrote LP debug file(s) to {}_*.lp{}",
          lp_stem,
          (lp_debug_compression.empty() || lp_debug_compression == "none"
           || lp_debug_compression == "uncompressed")
              ? ""
              : " (compressing async)");
    }

    std::vector<future_t> futures;
    futures.reserve(planning_lp.systems().size());

    for (auto&& [scene_index, phase_systems] :
         enumerate<SceneIndex>(planning_lp.systems()))
    {
      auto result = pool->submit(
          [&, scene_index]
          {
            SPDLOG_TRACE("MonolithicMethod: scene {} starting", scene_index);
            const auto t_scene = std::chrono::steady_clock::now();
            auto r = planning_lp.resolve_scene_phases(
                scene_index, phase_systems, effective_opts);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - t_scene)
                    .count();
            {
              const std::scoped_lock lk(times_mutex);
              scene_times[static_cast<std::size_t>(scene_index)] = elapsed;
            }
            ++scenes_done;
            SPDLOG_INFO("MonolithicMethod: scene {} done in {:.3f}s ({}/{})",
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

    // Drain LP compression tasks (run in parallel with solve)
    lp_writer.drain();

    // ── Write monitoring status file ──
    monitor.stop();
    {
      const double total_elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - solve_start)
              .count();
      SPDLOG_INFO("MonolithicMethod: all {} scene(s) done in {:.3f}s",
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

}  // namespace gtopt
