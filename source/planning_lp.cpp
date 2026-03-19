/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <ranges>

#include <gtopt/check_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto PlanningLP::create_systems(System& system,
                                SimulationLP& simulation,
                                const OptionsLP& options,
                                const FlatOptions& flat_opts)
    -> scene_phase_systems_t
{
  system.expand_batteries();
  system.setup_reference_bus(options);
  auto&& scenes = simulation.scenes();
  auto&& phases = simulation.phases();

  const auto num_scenes = static_cast<int>(scenes.size());
  const auto num_phases = static_cast<int>(phases.size());
  SPDLOG_INFO(
      "  Building LP: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  PlanningLP::scene_phase_systems_t all_systems(scenes.size());

  // Use the work pool to build scenes in parallel.  Each scene's phases
  // are constructed sequentially (state variables within a scene depend
  // on phase order), but different scenes are independent.
  auto pool = make_solver_work_pool();

  const auto build_start = std::chrono::steady_clock::now();

  std::vector<std::future<void>> futures;
  futures.reserve(scenes.size());

  for (auto&& [scene_num, scene] : std::views::enumerate(scenes)) {
    const auto scene_index = SceneIndex {static_cast<Index>(scene_num)};
    auto result = pool->submit(
        [&, scene_index]
        {
          [[maybe_unused]] const auto sn =
              static_cast<int>(scene_index) + 1;  // 1-based for logging
          SPDLOG_DEBUG("  Building LP scene {}/{} (uid {})",
                       sn,
                       num_scenes,
                       scene.uid());
          PlanningLP::phase_systems_t phase_systems;
          phase_systems.reserve(phases.size());
          for (auto&& phase : phases) {
            SPDLOG_TRACE("    Building LP scene {}/{} phase {} (uid {})",
                         sn,
                         num_scenes,
                         static_cast<int>(phase.index()),
                         phase.uid());
            phase_systems.emplace_back(
                system, simulation, phase, scene, flat_opts);
          }
          all_systems[scene_index] = std::move(phase_systems);
        });
    if (!result.has_value()) {
      throw std::runtime_error(
          std::format("Failed to submit scene {} to work pool", scene_index));
    }
    futures.push_back(std::move(*result));
  }

  for (auto& fut : futures) {
    fut.get();
  }

  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - build_start)
                             .count();
  SPDLOG_INFO("  Building LP done in {:.3f}s", elapsed);

  return all_systems;
}

void PlanningLP::write_lp(const std::string& filename) const
{
  try {
    for (auto&& phase_systems : m_systems_) {
      for (auto&& system : phase_systems) {
        try {
          [[maybe_unused]] auto _ = system.write_lp(filename);
        } catch (const std::exception& e) {
          SPDLOG_ERROR(
              std::format("Failed to write LP for system: {}", e.what()));
          throw;
        }
      }
    }
  } catch (const std::exception& e) {
    SPDLOG_ERROR(
        "Failed to write LP file {}: {}", filename, std::string(e.what()));
    throw;
  }
}

void PlanningLP::write_out() const
{
  const auto num_scenes = static_cast<int>(m_systems_.size());
  const auto num_phases =
      num_scenes > 0 ? static_cast<int>(m_systems_.front().size()) : 0;
  SPDLOG_INFO(
      "  Writing output: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // Submit one task per (scene, phase) pair to the work pool so that
  // output files are written in parallel.  Tasks use Low priority
  // (non-LP I/O work) to avoid competing with solver tasks.
  auto pool = make_solver_work_pool();

  std::vector<std::future<void>> futures;
  futures.reserve(static_cast<std::size_t>(num_scenes)
                  * static_cast<std::size_t>(std::max(num_phases, 0)));

  for (auto&& [scene_num, phase_systems] : std::views::enumerate(m_systems_)) {
    for (auto&& [phase_num, system] : std::views::enumerate(phase_systems)) {
      SPDLOG_DEBUG("  Submitting write_out scene {}/{} phase {}/{}",
                   scene_num + 1,
                   num_scenes,
                   phase_num + 1,
                   num_phases);
      auto result = pool->submit(
          [&system] { system.write_out(); },
          {
              .priority = TaskPriority::Low,
              .name = std::format("write_out_s{}_p{}", scene_num, phase_num),
          });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      } else {
        // Fall back to synchronous if the pool rejects the task.
        SPDLOG_WARN(
            "Failed to submit write_out task for scene {} phase {},"
            " running synchronously",
            scene_num,
            phase_num);
        system.write_out();
      }
    }
  }

  for (auto& fut : futures) {
    fut.get();
  }

  // ── Write consolidated solution.csv ─────────────────────────────────────
  // All parallel write tasks have completed; now collect solution values
  // from every (scene, phase) system and write solution.csv in scene/phase
  // order.  This avoids any concurrent file access and guarantees a
  // deterministic, sorted output regardless of task completion order.

  struct SolutionRow
  {
    Uid scene_uid;
    Uid phase_uid;
    int status;
    double obj_value;
    double kappa;
  };

  std::vector<SolutionRow> rows;
  rows.reserve(static_cast<std::size_t>(num_scenes)
               * static_cast<std::size_t>(std::max(num_phases, 0)));

  for (const auto& phase_systems : m_systems_) {
    for (const auto& system : phase_systems) {
      const auto& li = system.linear_interface();
      rows.push_back({
          .scene_uid = static_cast<Uid>(system.scene().uid()),
          .phase_uid = static_cast<Uid>(system.phase().uid()),
          .status = li.get_status(),
          .obj_value = li.get_obj_value(),
          .kappa = li.get_kappa(),
      });
    }
  }

  // Sort rows by (scene_uid, phase_uid) for a deterministic output order.
  std::ranges::sort(rows,
                    [](const SolutionRow& a, const SolutionRow& b)
                    {
                      if (a.scene_uid != b.scene_uid) {
                        return a.scene_uid < b.scene_uid;
                      }
                      return a.phase_uid < b.phase_uid;
                    });

  const auto out_dir = std::filesystem::path(m_options_.output_directory());
  const auto sol_path = out_dir / "solution.csv";

  std::ofstream sol_file(sol_path.string(), std::ios::out);
  if (!sol_file) [[unlikely]] {
    SPDLOG_CRITICAL("Cannot open solution file '{}' for writing",
                    sol_path.string());
    return;
  }

  sol_file << "scene,phase,status,obj_value,kappa\n";
  for (const auto& row : rows) {
    sol_file << std::format("{},{},{},{},{}\n",
                            row.scene_uid,
                            row.phase_uid,
                            row.status,
                            row.obj_value,
                            row.kappa);
    SPDLOG_DEBUG("  solution.csv: scene={} phase={} status={} obj_value={}",
                 row.scene_uid,
                 row.phase_uid,
                 row.status,
                 row.obj_value);
  }
}

std::expected<void, Error> PlanningLP::resolve_scene_phases(
    SceneIndex scene_index,
    phase_systems_t& phase_systems,
    const SolverOptions& lp_opts)
{
  [[maybe_unused]] const auto num_phases =
      static_cast<int>(phase_systems.size());
  SPDLOG_DEBUG("  Solving scene {} ({} phase(s))", scene_index, num_phases);

  for (auto&& [phase_index, system_sp] : enumerate<PhaseIndex>(phase_systems)) {
    SPDLOG_TRACE("    Solving scene {} phase {} ({} cols, {} rows)",
                 scene_index,
                 phase_index,
                 system_sp.linear_interface().get_numcols(),
                 system_sp.linear_interface().get_numrows());

    if (auto result = system_sp.resolve(lp_opts); !result) {
      // Log the solver error with scene/phase context before writing the LP
      spdlog::warn("  Scene {} phase {}: {}",
                   scene_index,
                   phase_index,
                   result.error().message);

      // On error, write the problematic model to the log directory for
      // debugging
      const auto log_dir = m_options_.log_directory();
      std::filesystem::create_directories(log_dir);
      const auto filename =
          (std::filesystem::path(log_dir)
           / std::format("error_{}_{}", scene_index, phase_index))
              .string();
      const auto lp_file = system_sp.write_lp(filename);
      spdlog::error("  Infeasible LP written to: {}", lp_file);

      // Run gtopt_check_lp static analysis and log the diagnostic.
      // Pass the full SolverOptions so the diagnostic uses the same algorithm
      // and tolerance settings as the gtopt solver.
      if (const auto diag =
              run_check_lp_diagnostic(lp_file, /*timeout_seconds=*/10, lp_opts);
          !diag.empty())
      {
        log_diagnostic_lines("error", lp_file, diag);
      }

      auto error = std::move(result.error());
      error.message += std::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(std::move(error));
    }

    SPDLOG_DEBUG("    Scene {} phase {} solved ok", scene_index, phase_index);

    // update state variable dependents with the last solution
    const auto& solution_vector = system_sp.linear_interface().get_col_sol();

    for (auto&& state_var :
         simulation().state_variables(scene_index, phase_index)
             | std::views::values)
    {
      const double solution_value = solution_vector[state_var.col()];

      for (auto&& dep_var : state_var.dependent_variables()) {
        auto& target_system =
            system(dep_var.scene_index(), dep_var.phase_index());
        target_system.linear_interface().set_col(dep_var.col(), solution_value);
      }
    }
  }

  return {};
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  const auto num_phases = simulation().phases().size();
  auto solver = make_planning_solver(m_options_, num_phases);
  return solver->solve(*this, lp_opts);
}

}  // namespace gtopt
