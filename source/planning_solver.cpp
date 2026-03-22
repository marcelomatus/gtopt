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
#include <future>
#include <mutex>
#include <vector>

#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/sddp_cut_io.hpp>
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

  // ── Boundary cuts ──
  if (!boundary_cuts_file.empty()
      && boundary_cuts_mode != BoundaryCutsMode::noload)
  {
    SPDLOG_INFO("MonolithicSolver: loading boundary cuts from '{}'",
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
          "MonolithicSolver: loaded {} boundary cuts (max iteration {})",
          bc_result->count,
          bc_result->max_iteration);
    } else {
      SPDLOG_WARN("MonolithicSolver: failed to load boundary cuts: {}",
                  bc_result.error().message);
    }
  }

  // Apply solve_timeout to the solver options if configured
  auto effective_opts = opts;
  if (solve_timeout > 0.0) {
    effective_opts.time_limit = solve_timeout;
    SPDLOG_INFO("MonolithicSolver: solve_timeout={:.1f}s", solve_timeout);
  }

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
          lp_writer.compress_async(system.write_lp(lp_stem));
        }
      }
      spdlog::info(
          "MonolithicSolver: wrote LP debug file(s) to {}_*.lp{}",
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
            SPDLOG_TRACE("MonolithicSolver: scene {} starting", scene_index);
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

    // Drain LP compression tasks (run in parallel with solve)
    lp_writer.drain();

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

std::unique_ptr<PlanningSolver> make_planning_solver(const OptionsLP& options,
                                                     size_t num_phases)
{
  // Validate enum option strings and warn about unknown values
  for (const auto& w : options.validate_enum_options()) {
    SPDLOG_WARN("Options: {}", w);
  }

  if (options.solver_type_enum() == SolverType::sddp) {
    // SDDP requires at least 2 phases; fall back to monolithic for 0 or 1.
    if (num_phases > 0 && num_phases < 2) {
      SPDLOG_INFO(
          "SDDP requested but only {} phase(s); using monolithic solver",
          num_phases);
    } else {
      SDDPOptions sddp_opts;

      // Iteration control
      sddp_opts.max_iterations = options.sddp_max_iterations();
      sddp_opts.min_iterations = options.sddp_min_iterations();
      sddp_opts.convergence_tol = options.sddp_convergence_tol();

      // Advanced tuning
      sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
      sddp_opts.elastic_filter_mode =
          parse_elastic_filter_mode(options.sddp_elastic_mode());
      sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
      sddp_opts.num_apertures = options.sddp_num_apertures();
      sddp_opts.aperture_timeout = options.sddp_aperture_timeout();
      sddp_opts.save_aperture_lp = options.sddp_save_aperture_lp();
      sddp_opts.warm_start = options.sddp_warm_start();
      sddp_opts.solve_timeout = options.sddp_solve_timeout();
      sddp_opts.max_cuts_per_phase = options.sddp_max_cuts_per_phase();
      sddp_opts.cut_prune_interval = options.sddp_cut_prune_interval();
      sddp_opts.prune_dual_threshold = options.sddp_prune_dual_threshold();
      sddp_opts.single_cut_storage = options.sddp_single_cut_storage();
      sddp_opts.max_stored_cuts = options.sddp_max_stored_cuts();
      sddp_opts.use_clone_pool = options.sddp_use_clone_pool();
      sddp_opts.alpha_min = options.sddp_alpha_min();
      sddp_opts.alpha_max = options.sddp_alpha_max();

      // Cut sharing and files
      sddp_opts.cut_sharing =
          parse_cut_sharing_mode(options.sddp_cut_sharing_mode());
      // Place the cuts directory inside the output directory so that all
      // solver output is self-contained.
      const auto output_dir_sv = options.output_directory();
      const auto cut_dir =
          (std::filesystem::path(
               output_dir_sv.empty() ? "output" : std::string(output_dir_sv))
           / options.sddp_cut_directory())
              .string();
      sddp_opts.cuts_output_file =
          (std::filesystem::path(cut_dir) / sddp_file::combined_cuts).string();

      sddp_opts.hot_start_mode = options.sddp_hot_start_mode_enum();
      sddp_opts.save_per_iteration = options.sddp_save_per_iteration();
      const auto cuts_input = options.sddp_cuts_input_file();
      if (!cuts_input.empty()) {
        sddp_opts.cuts_input_file = cuts_input;
      }

      const auto boundary_cuts = options.sddp_boundary_cuts_file();
      if (!boundary_cuts.empty()) {
        sddp_opts.boundary_cuts_file = std::string(boundary_cuts);
      }
      sddp_opts.boundary_cuts_mode = options.sddp_boundary_cuts_mode_enum();
      sddp_opts.boundary_max_iterations =
          options.sddp_boundary_max_iterations();

      const auto named_cuts = options.sddp_named_cuts_file();
      if (!named_cuts.empty()) {
        sddp_opts.named_cuts_file = std::string(named_cuts);
      }

      // Sentinel: honour the user-configured path when explicitly set.
      const auto sentinel = options.sddp_sentinel_file();
      const auto output_dir = options.output_directory();
      if (!sentinel.empty()) {
        sddp_opts.sentinel_file = sentinel;
      }

      // Logging and API
      sddp_opts.log_directory = std::string(options.log_directory());
      sddp_opts.lp_debug = options.lp_debug();
      sddp_opts.just_build_lp = options.just_build_lp();
      sddp_opts.lp_debug_compression = std::string(options.lp_compression());
      sddp_opts.enable_api = options.sddp_api_enabled();
      if (!output_dir.empty()) {
        sddp_opts.api_status_file =
            (std::filesystem::path(output_dir) / "sddp_status.json").string();
        // The monitoring API stop-request file lets the webservice (or any
        // external tool) trigger a graceful stop without using the raw sentinel
        // file.  The solver checks: sentinel_file exists || stop_request
        // exists.
        sddp_opts.api_stop_request_file =
            (std::filesystem::path(output_dir) / sddp_file::stop_request)
                .string();
      }

      return std::make_unique<SDDPPlanningSolver>(std::move(sddp_opts));
    }  // else (num_phases >= 2)
  }  // solver_type == "sddp"

  // Default: monolithic (also used as fallback for single-phase SDDP)
  auto solver = std::make_unique<MonolithicSolver>();
  const auto output_dir_m = options.output_directory();
  if (options.sddp_api_enabled() && !output_dir_m.empty()) {
    solver->enable_api = true;
    solver->api_status_file =
        (std::filesystem::path(output_dir_m) / "monolithic_status.json")
            .string();
  }
  solver->lp_debug = options.lp_debug();
  solver->lp_debug_directory = std::string(options.log_directory());
  solver->lp_debug_compression = std::string(options.lp_compression());
  solver->solve_timeout = options.monolithic_solve_timeout();
  solver->solve_mode = options.monolithic_solve_mode_enum();
  solver->boundary_cuts_file =
      std::string(options.monolithic_boundary_cuts_file());
  solver->boundary_cuts_mode = options.monolithic_boundary_cuts_mode_enum();
  solver->boundary_max_iterations =
      options.monolithic_boundary_max_iterations();
  return solver;
}

}  // namespace gtopt
