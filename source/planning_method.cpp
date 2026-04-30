/**
 * @file      planning_method.cpp
 * @brief     Factory for planning method instances (monolithic, SDDP, cascade)
 * @date      2026-03-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>

#include <gtopt/cascade_method.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Populate a fresh `SDDPOptions` from a `PlanningOptionsLP`.
///
/// Shared by the SDDP and cascade dispatch branches — both need the
/// full ~50-field wiring.  Prior to this helper, the two branches
/// each inlined the wiring, producing ~130 lines of near-duplicate
/// code that drifted over time (e.g. a field added to one branch
/// and forgotten in the other).  The characterisation snapshot test
/// at `test/source/test_planning_method_dispatch.cpp` asserts each
/// field's value for both default and override scenarios so a silent
/// drop is caught at test time.
[[nodiscard]] auto build_sddp_options(const PlanningOptionsLP& options)
    -> SDDPOptions
{
  SDDPOptions sddp_opts;

  // Iteration control
  sddp_opts.max_iterations = options.sddp_max_iterations();
  sddp_opts.min_iterations = options.sddp_min_iterations();
  sddp_opts.convergence_tol = options.sddp_convergence_tol();
  sddp_opts.convergence_mode = options.sddp_convergence_mode();
  sddp_opts.stationary_tol = options.sddp_stationary_tol();
  sddp_opts.stationary_window = options.sddp_stationary_window();
  sddp_opts.convergence_confidence = options.sddp_convergence_confidence();
  sddp_opts.forward_fail_stop = options.sddp_forward_fail_stop();

  // Simulation mode: forward-only evaluation, no training, no cut saving
  const bool simulation_mode = options.sddp_simulation_mode();
  if (simulation_mode) {
    sddp_opts.max_iterations = 0;
    sddp_opts.save_per_iteration = false;
    sddp_opts.save_simulation_cuts = false;
  }

  // Advanced tuning
  sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
  sddp_opts.elastic_filter_mode = options.sddp_elastic_mode_enum();
  sddp_opts.cut_coeff_eps = options.sddp_cut_coeff_eps();
  sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
  sddp_opts.apertures = options.sddp_apertures();
  sddp_opts.aperture_timeout = options.sddp_aperture_timeout();
  sddp_opts.save_aperture_lp = options.sddp_save_aperture_lp();
  sddp_opts.max_cuts_per_phase = options.sddp_max_cuts_per_phase();
  sddp_opts.cut_prune_interval = options.sddp_cut_prune_interval();
  sddp_opts.prune_dual_threshold = options.sddp_prune_dual_threshold();
  sddp_opts.single_cut_storage = options.sddp_single_cut_storage();
  sddp_opts.max_stored_cuts = options.sddp_max_stored_cuts();
  sddp_opts.low_memory_mode = options.sddp_low_memory();
  sddp_opts.memory_codec = options.sddp_memory_codec();
  sddp_opts.scale_alpha = options.sddp_scale_alpha();

  // Cut sharing and files.  Place the cuts directory inside the output
  // directory so all solver output is self-contained.
  sddp_opts.cut_sharing = options.sddp_cut_sharing_mode_enum();
  const auto output_dir_sv = options.output_directory();
  const auto cut_dir =
      (std::filesystem::path(output_dir_sv.empty() ? "output"
                                                   : std::string(output_dir_sv))
       / options.sddp_cut_directory())
          .string();
  sddp_opts.cuts_output_file =
      (std::filesystem::path(cut_dir) / sddp_file::combined_cuts).string();

  sddp_opts.cut_recovery_mode = options.sddp_cut_recovery_mode_enum();
  sddp_opts.recovery_mode = options.sddp_recovery_mode_enum();
  if (!simulation_mode) {
    sddp_opts.save_per_iteration = options.sddp_save_per_iteration();
  }

  if (const auto cuts_input = options.sddp_cuts_input_file();
      !cuts_input.empty())
  {
    sddp_opts.cuts_input_file = cuts_input;
  }
  if (const auto boundary_cuts = options.sddp_boundary_cuts_file();
      !boundary_cuts.empty())
  {
    sddp_opts.boundary_cuts_file = std::string(boundary_cuts);
  }
  sddp_opts.boundary_cuts_mode = options.sddp_boundary_cuts_mode_enum();
  sddp_opts.boundary_max_iterations = options.sddp_boundary_max_iterations();
  sddp_opts.missing_cut_var_mode = options.sddp_missing_cut_var_mode();

  if (const auto named_cuts = options.sddp_named_cuts_file();
      !named_cuts.empty())
  {
    sddp_opts.named_cuts_file = std::string(named_cuts);
  }
  if (const auto sentinel = options.sddp_sentinel_file(); !sentinel.empty()) {
    sddp_opts.sentinel_file = sentinel;
  }

  // Logging and API.  The stop-request file lets the webservice trigger
  // a graceful stop without using the raw sentinel path.
  sddp_opts.log_directory = std::string(options.log_directory());
  sddp_opts.lp_debug = options.lp_debug();
  sddp_opts.lp_debug_compression = std::string(options.lp_compression());
  sddp_opts.lp_debug_scene_min = options.lp_debug_scene_min();
  sddp_opts.lp_debug_scene_max = options.lp_debug_scene_max();
  sddp_opts.lp_debug_phase_min = options.lp_debug_phase_min();
  sddp_opts.lp_debug_phase_max = options.lp_debug_phase_max();
  sddp_opts.enable_api = options.sddp_api_enabled();
  if (const auto output_dir = options.output_directory(); !output_dir.empty()) {
    sddp_opts.api_status_file =
        (std::filesystem::path(output_dir) / "solver_status.json").string();
    sddp_opts.api_stop_request_file =
        (std::filesystem::path(output_dir) / sddp_file::stop_request).string();
  }

  // Simulation mode: clear output file to suppress all cut persistence
  if (simulation_mode) {
    sddp_opts.cuts_output_file.clear();
  }

  // Async + work-pool resource limits
  sddp_opts.max_async_spread = options.sddp_max_async_spread();
  sddp_opts.pool_cpu_factor = options.sddp_pool_cpu_factor();
  sddp_opts.pool_memory_limit_mb = options.sddp_pool_memory_limit_mb();

  // Wire solve_timeout from forward solver's time_limit (if set)
  if (const auto fwd_solver = options.sddp_forward_solver_options();
      fwd_solver.time_limit)
  {
    sddp_opts.solve_timeout = *fwd_solver.time_limit;
  }

  // Wire forward/backward solver options (pre-merged with global).
  // NOTE: SolverOptions::memory_emphasis is independent of
  // sddp_options.low_memory_mode — callers must set it explicitly per
  // pass if they want the backend's native memory-emphasis mode.
  sddp_opts.forward_solver_options = options.sddp_forward_solver_options();
  sddp_opts.backward_solver_options = options.sddp_backward_solver_options();

  return sddp_opts;
}

}  // namespace

// ─── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<PlanningMethod> make_planning_method(
    const PlanningOptionsLP& options, size_t num_phases)
{
  // Validate enum option strings and warn about unknown values
  for (const auto& w : PlanningOptionsLP::validate_enum_options()) {
    SPDLOG_WARN("Options: {}", w);
  }

  if (options.method_type_enum() == MethodType::sddp) {
    // SDDP requires at least 2 phases; fall back to monolithic for 0 or 1.
    if (num_phases > 0 && num_phases < 2) {
      SPDLOG_INFO(
          "SDDP requested but only {} phase(s); using monolithic solver",
          num_phases);
    } else {
      return std::make_unique<SDDPPlanningMethod>(build_sddp_options(options));
    }
  }

  if (options.method_type_enum() == MethodType::cascade) {
    if (num_phases > 0 && num_phases < 2) {
      SPDLOG_INFO(
          "Cascade requested but only {} phase(s); using monolithic solver",
          num_phases);
    } else {
      auto sddp_opts = build_sddp_options(options);

      // Per-cascade extension: gather cascade-level options.
      CascadeOptions cascade_opts;
      cascade_opts.sddp_options = options.cascade_sddp_options();
      if (options.has_cascade_levels()) {
        cascade_opts.level_array = {options.cascade_levels().begin(),
                                    options.cascade_levels().end()};
      }

      return std::make_unique<CascadePlanningMethod>(std::move(sddp_opts),
                                                     std::move(cascade_opts));
    }
  }

  // Default: monolithic (also used as fallback for single-phase SDDP/cascade)
  auto solver = std::make_unique<MonolithicMethod>();
  const auto output_dir_m = options.output_directory();
  if (options.sddp_api_enabled() && !output_dir_m.empty()) {
    solver->enable_api = true;
    solver->api_status_file =
        (std::filesystem::path(output_dir_m) / "solver_status.json").string();
  }
  solver->lp_debug = options.lp_debug();
  solver->lp_debug_directory = std::string(options.log_directory());
  solver->lp_debug_compression = std::string(options.lp_compression());
  solver->lp_debug_scene_min = options.lp_debug_scene_min();
  solver->lp_debug_scene_max = options.lp_debug_scene_max();
  solver->lp_debug_phase_min = options.lp_debug_phase_min();
  solver->lp_debug_phase_max = options.lp_debug_phase_max();
  solver->solve_mode = options.monolithic_solve_mode_enum();
  solver->boundary_cuts_file =
      std::string(options.monolithic_boundary_cuts_file());
  solver->boundary_cuts_mode = options.monolithic_boundary_cuts_mode_enum();
  solver->boundary_max_iterations =
      options.monolithic_boundary_max_iterations();
  return solver;
}

}  // namespace gtopt
