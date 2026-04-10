/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <ranges>

#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── Adaptive scale_theta computation ──────────────────────────────────────

void PlanningLP::auto_scale_theta(Planning& planning)
{
  // Only compute when scale_theta is not explicitly set at any level.
  auto& opts = planning.options;
  if (opts.scale_theta.has_value()
      || opts.model_options.scale_theta.has_value())
  {
    return;
  }

  // Don't auto-scale when Kirchhoff is disabled or single-bus mode.
  const auto& mo = opts.model_options;
  if ((opts.use_kirchhoff.has_value() && !*opts.use_kirchhoff)
      || (mo.use_kirchhoff.has_value() && !*mo.use_kirchhoff))
  {
    return;
  }
  if ((opts.use_single_bus.has_value() && *opts.use_single_bus)
      || (mo.use_single_bus.has_value() && *mo.use_single_bus))
  {
    return;
  }

  // Collect scalar reactance values from all lines.
  std::vector<double> reactances;
  for (const auto& line : planning.system.line_array) {
    if (!line.reactance.has_value()) {
      continue;
    }
    const auto& sched = *line.reactance;
    double x = 0.0;
    if (std::holds_alternative<double>(sched)) {
      x = std::get<double>(sched);
    } else if (std::holds_alternative<std::vector<double>>(sched)) {
      const auto& vec = std::get<std::vector<double>>(sched);
      if (!vec.empty()) {
        x = vec.front();
      }
    }
    if (x > 0.0) {
      reactances.push_back(x);
    }
  }

  if (reactances.empty()) {
    return;
  }

  // Compute median reactance.
  std::ranges::sort(reactances);
  const auto n = reactances.size();
  const double median_x = (n % 2 == 0)
      ? (reactances[(n / 2) - 1] + reactances[n / 2]) / 2.0
      : reactances[n / 2];

  // scale_theta = median_x so that x_tau = X/(V²·scale_theta) ≈ 1 for
  // the median line (V=1 per-unit).
  opts.model_options.scale_theta = median_x;
  spdlog::info("  Auto scale_theta = {:.6g} (median of {} line reactances)",
               median_x,
               n);
}

// ── Adaptive reservoir energy scaling ────────────────────────────────────

void PlanningLP::auto_scale_reservoirs(Planning& planning)
{
  auto& opts = planning.options;
  auto& sys = planning.system;

  // Build a set of UIDs already covered by explicit variable_scales entries.
  auto has_entry = [&](Uid uid) -> bool
  {
    return std::ranges::any_of(opts.variable_scales,
                               [uid](const VariableScale& vs)
                               {
                                 return vs.class_name == "Reservoir"
                                     && vs.variable == "energy"
                                     && vs.uid == uid;
                               });
  };

  // Helper: extract a representative scalar from a FieldSched optional.
  auto scalar_of = [](const OptTRealFieldSched& fs) -> std::optional<double>
  {
    if (!fs.has_value()) {
      return std::nullopt;
    }
    if (std::holds_alternative<double>(*fs)) {
      return std::get<double>(*fs);
    }
    if (std::holds_alternative<std::vector<Real>>(*fs)) {
      const auto& vec = std::get<std::vector<Real>>(*fs);
      if (!vec.empty()) {
        return *std::ranges::max_element(vec);
      }
    }
    return std::nullopt;  // FileSched — can't resolve statically
  };

  size_t count = 0;
  for (const auto& rsv : sys.reservoir_array) {
    if (has_entry(rsv.uid)) {
      continue;
    }
    const auto emax = scalar_of(rsv.emax);
    if (!emax.has_value() || *emax <= 1000.0) {
      continue;
    }
    // 10^ceil(log10(emax / 1000)) — round up to next power of 10.
    const double raw = *emax / 1000.0;
    const double energy_scale = std::pow(10.0, std::ceil(std::log10(raw)));
    const double flow_scale = energy_scale / 1000.0;

    opts.variable_scales.push_back(VariableScale {
        .class_name = "Reservoir",
        .variable = "energy",
        .uid = rsv.uid,
        .scale = energy_scale,
        .name = rsv.name,
    });
    opts.variable_scales.push_back(VariableScale {
        .class_name = "Reservoir",
        .variable = "flow",
        .uid = rsv.uid,
        .scale = flow_scale,
        .name = rsv.name,
    });
    ++count;
  }

  if (count > 0) {
    spdlog::info(
        "  Auto scale_reservoir: computed energy/flow scales for {} reservoirs",
        count);
  }
}

void PlanningLP::tighten_scene_phase_links(phase_systems_t& phase_systems,
                                           SimulationLP& simulation)
{
  for (auto& sys : phase_systems) {
    auto& links = sys.pending_state_links();
    for (const auto& link : links) {
      auto prev_var = simulation.state_variable(link.prev_key);
      if (prev_var) {
        prev_var->get().add_dependent_variable(link.here_key, link.here_col);
      } else {
        // Producer-side StateVariable was never registered: the previous
        // phase's element either didn't run or chose not to publish an
        // efin/state column for this (scenario, stage).  Mirrors the
        // build-time warning that lived in storage_lp.hpp before the
        // deferred-linking refactor — kept here so cross-phase coupling
        // gaps remain visible.
        SPDLOG_WARN(
            "tighten_scene_phase_links: no producer StateVariable for "
            "deferred link (class='{}' col='{}' uid={} prev_stage_uid={} "
            "scene={} prev_phase={}). Cross-phase state coupling will be "
            "missing for this element.",
            link.prev_key.class_name,
            link.prev_key.col_name,
            static_cast<int>(link.prev_key.uid),
            static_cast<int>(link.prev_key.stage_uid),
            static_cast<int>(link.prev_key.lp_key.scene_index),
            static_cast<int>(link.prev_key.lp_key.phase_index));
      }
    }
    links.clear();
    links.shrink_to_fit();
  }
}

auto PlanningLP::create_systems(System& system,
                                SimulationLP& simulation,
                                const PlanningOptionsLP& options,
                                const LpMatrixOptions& flat_opts)
    -> scene_phase_systems_t
{
  system.expand_batteries();
  system.expand_reservoir_constraints();
  system.setup_reference_bus(options);

  // Note: AMPL element-name and compound registries are populated by
  // SystemLP's constructor under std::call_once on
  // SimulationLP::ampl_registry_flag(), so the registry is filled
  // exactly once regardless of whether construction goes through
  // PlanningLP or directly via tests.

  auto&& scenes = simulation.scenes();
  auto&& phases = simulation.phases();

  const auto num_scenes = static_cast<int>(scenes.size());
  const auto num_phases = static_cast<int>(phases.size());
  SPDLOG_INFO(
      "  Building LP: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // Pre-resolve the solver name on the main thread so worker threads
  // don't race through the plugin registry in parallel.
  auto resolved_opts = flat_opts;
  if (resolved_opts.solver_name.empty()) {
    resolved_opts.solver_name =
        std::string(SolverRegistry::instance().default_solver());
  }

  PlanningLP::scene_phase_systems_t all_systems(scenes.size());

  // Use the work pool to build scenes in parallel.  Each scene's phases
  // are constructed sequentially (state variables within a scene depend
  // on phase order), but different scenes are independent.
  auto pool = make_solver_work_pool();

  const auto build_start = std::chrono::steady_clock::now();

  std::vector<std::future<void>> futures;
  futures.reserve(scenes.size());

  for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
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
                         phase.index(),
                         phase.uid());
            phase_systems.emplace_back(
                system, simulation, phase, scene, resolved_opts);
          }

          // Resolve any deferred cross-phase state-variable links
          // queued by this scene's phases.  Runs inside the scene
          // task so that different scenes' tightening passes can
          // overlap and so that linking happens before the per-scene
          // update_lp pass below sees the dependent columns.
          tighten_scene_phase_links(phase_systems, simulation);

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

  // After all add_to_lp calls, dispatch a single initial update_lp pass
  // so that volume-dependent LP elements are set from the reservoir eini
  // values before any solver is called.
  for (auto& phase_systems : all_systems) {
    for (auto& sys : phase_systems) {
      std::ignore = sys.update_lp();
    }
  }

  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - build_start)
                             .count();
  SPDLOG_INFO("  Building LP done in {:.3f}s", elapsed);

  return all_systems;
}

void PlanningLP::write_lp(const std::string& filename) const
{
  for (auto&& phase_systems : m_systems_) {
    for (auto&& system : phase_systems) {
      auto result = system.write_lp(filename);
      if (!result) {
        spdlog::warn("{}", result.error().message);
        return;
      }
    }
  }
}

void PlanningLP::write_out()
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

  for (auto&& [scene_num, phase_systems] : enumerate<SceneIndex>(m_systems_)) {
    for (auto&& [phase_num, system] : enumerate<PhaseIndex>(phase_systems)) {
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
    double max_kappa;
    double gap;  ///< Final SDDP gap (0.0 for monolithic)
    double
        gap_change;  ///< Final SDDP stationary gap-change (1.0 for monolithic)
  };

  std::vector<SolutionRow> rows;
  rows.reserve(static_cast<std::size_t>(num_scenes)
               * static_cast<std::size_t>(std::max(num_phases, 0)));

  const auto& sddp = m_sddp_summary_;

  for (const auto& phase_systems : m_systems_) {
    for (const auto& system : phase_systems) {
      const auto& li = system.linear_interface();
      rows.push_back({
          .scene_uid = static_cast<Uid>(system.scene().uid()),
          .phase_uid = static_cast<Uid>(system.phase().uid()),
          .status = li.get_status(),
          .obj_value = li.get_obj_value_physical(),
          .kappa = li.get_kappa(),
          .max_kappa = sddp.max_kappa,
          .gap = sddp.gap,
          .gap_change = sddp.gap_change,
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

  // Status names: CLP convention (0=optimal, 1=primal infeasible,
  // 2=dual infeasible/unbounded, 3=iteration limit, 4=error, 5=not solved)
  static constexpr auto status_name = [](int s) constexpr -> std::string_view
  {
    switch (s) {
      case 0:
        return "optimal";
      case 1:
        return "infeasible";
      case 2:
        return "unbounded";
      case 3:
        return "iteration_limit";
      case 4:
        return "error";
      default:
        return "unknown";
    }
  };

  sol_file << "scene,phase,status,status_name,obj_value,kappa,max_kappa,"
              "gap,gap_change\n";
  for (const auto& row : rows) {
    sol_file << std::format("{},{},{},{},{},{},{},{},{}\n",
                            row.scene_uid,
                            row.phase_uid,
                            row.status,
                            status_name(row.status),
                            row.obj_value,
                            row.kappa,
                            row.max_kappa,
                            row.gap,
                            row.gap_change);
    SPDLOG_DEBUG(
        "  solution.csv: scene={} phase={} status={} obj_value={} "
        "kappa={} max_kappa={} gap={} gap_change={}",
        row.scene_uid,
        row.phase_uid,
        row.status,
        row.obj_value,
        row.kappa,
        row.max_kappa,
        row.gap,
        row.gap_change);
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

  // Configure per-scene/phase solver log files when detailed mode is active
  const auto log_mode = lp_opts.log_mode.value_or(SolverLogMode::nolog);
  const auto log_dir = m_options_.log_directory();

  for (auto&& [phase_index, system_sp] : enumerate<PhaseIndex>(phase_systems)) {
    SPDLOG_TRACE("    Solving scene {} phase {} ({} cols, {} rows)",
                 scene_index,
                 phase_index,
                 system_sp.linear_interface().get_numcols(),
                 system_sp.linear_interface().get_numrows());

    if (log_mode == SolverLogMode::detailed && !log_dir.empty()) {
      std::filesystem::create_directories(log_dir);
      auto& li = system_sp.linear_interface();
      li.set_log_file(
          (std::filesystem::path(log_dir)
           / std::format(
               "{}_sc{}_ph{}", li.solver_name(), scene_index, phase_index))
              .string());
    }

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
      if (auto lp_result = system_sp.write_lp(filename)) {
        spdlog::error("  Infeasible LP written to: {}", *lp_result);
      } else {
        spdlog::warn("{}", lp_result.error().message);
      }

      // LP diagnostic analysis is performed by run_gtopt after the solver
      // exits.  It scans the log directory for error*.lp files and runs
      // gtopt_check_lp on them with richer output (AI, IIS, parallel).

      auto error = std::move(result.error());
      error.message += std::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(std::move(error));
    }

    SPDLOG_DEBUG("    Scene {} phase {} solved ok", scene_index, phase_index);

    // update state variable dependents with the last solution
    const auto& solution_vector =
        system_sp.linear_interface().get_col_sol_raw();

    for (auto&& state_var :
         simulation().state_variables(scene_index, phase_index)
             | std::views::values)
    {
      const double solution_value = solution_vector[state_var.col()];

      for (auto&& dep_var : state_var.dependent_variables()) {
        auto& target_system =
            system(dep_var.scene_index(), dep_var.phase_index());
        target_system.linear_interface().set_col_raw(dep_var.col(),
                                                     solution_value);
      }
    }
  }

  return {};
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  const auto num_phases = simulation().phases().size();
  auto solver = make_planning_method(m_options_, num_phases);
  return solver->solve(*this, lp_opts);
}

}  // namespace gtopt
