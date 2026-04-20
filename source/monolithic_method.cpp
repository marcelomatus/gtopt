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

#include <gtopt/as_label.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto MonolithicMethod::solve(PlanningLP& planning_lp, const SolverOptions& opts)
    -> std::expected<int, Error>
{
  auto pool = make_solver_work_pool(
      /*cpu_factor=*/2.0,
      /*cpu_threshold_override=*/0.0,
      /*scheduler_interval=*/std::chrono::milliseconds(50),
      /*memory_limit_mb=*/planning_lp.options().sddp_pool_memory_limit_mb());

  // ── Monitoring setup ──
  const auto solve_start = std::chrono::steady_clock::now();
  const auto num_scenes = static_cast<Index>(planning_lp.systems().size());

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

    const LabelMaker label_maker {};

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
          const auto su = system.scene().uid();
          const auto pu = system.phase().uid();
          if (!in_lp_debug_range(su,
                                 pu,
                                 lp_debug_scene_min,
                                 lp_debug_scene_max,
                                 lp_debug_phase_min,
                                 lp_debug_phase_max))
          {
            continue;
          }
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

    // ── Kappa threshold checking ──
    {
      const auto& sim = planning_lp.planning().simulation;
      const auto kappa_mode =
          sim.kappa_warning.value_or(KappaWarningMode::warn);
      if (kappa_mode != KappaWarningMode::none) {
        constexpr double default_kappa_threshold = 1e9;
        const double threshold =
            sim.kappa_threshold.value_or(default_kappa_threshold);
        double max_kappa = -1.0;
        for (auto&& [si, phase_systems_ref] :
             enumerate<SceneIndex>(planning_lp.systems()))
        {
          for (auto&& [pi, system] : enumerate<PhaseIndex>(phase_systems_ref)) {
            const auto& li = system.linear_interface();
            const auto kappa_opt = li.get_kappa();
            if (!kappa_opt.has_value()) {
              continue;  // backend reported unavailable — skip entirely
            }
            const double kappa = *kappa_opt;
            max_kappa = std::max(max_kappa, kappa);
            if (kappa > threshold) {
              spdlog::warn(
                  "High kappa {:.2e} (threshold {:.2e}) at scene {} phase {}",
                  kappa,
                  threshold,
                  system.scene().uid(),
                  system.phase().uid());
              if (kappa_mode == KappaWarningMode::save_lp
                  && !lp_debug_directory.empty())
              {
                std::filesystem::create_directories(lp_debug_directory);
                const auto stem =
                    (std::filesystem::path(lp_debug_directory)
                     / as_label(
                         "kappa", system.scene().uid(), system.phase().uid()))
                        .string();
                if (auto r = li.write_lp(stem)) {
                  spdlog::warn("Saved high-kappa LP to {}.lp", stem);
                }
              }
            }
          }
        }
        planning_lp.set_sddp_summary({
            .max_kappa = max_kappa,
        });
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
      json += std::format("  \"method\": \"monolithic\",\n");
      // Query the actual solver identity from the first available LP
      if (!planning_lp.systems().empty()
          && !planning_lp.systems().front().empty())
      {
        const auto& li =
            planning_lp.systems().front().front().linear_interface();
        json += std::format("  \"solver\": \"{}\",\n", li.solver_id());
      }
      json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed);
      json += std::format("  \"total_scenes\": {},\n", num_scenes);
      json += std::format("  \"scenes_done\": {},\n", scenes_done.load());

      json += "  \"scene_times\": [";
      {
        const std::scoped_lock lk(times_mutex);
        for (const auto& [i, t] : enumerate(scene_times)) {
          if (i > 0) {
            json += ", ";
          }
          json += std::format("{:.4f}", t);
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
