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

// ─── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<PlanningMethod> make_planning_method(
    const PlanningOptionsLP& options, size_t num_phases)
{
  // Validate enum option strings and warn about unknown values
  for (const auto& w : options.validate_enum_options()) {
    SPDLOG_WARN("Options: {}", w);
  }

  if (options.method_type_enum() == MethodType::sddp) {
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
      sddp_opts.stationary_tol = options.sddp_stationary_tol();
      sddp_opts.stationary_window = options.sddp_stationary_window();

      // Simulation mode: forward-only evaluation, no training, no cut saving
      if (options.sddp_simulation_mode()) {
        sddp_opts.max_iterations = 0;
        sddp_opts.save_per_iteration = false;
        sddp_opts.save_simulation_cuts = false;
      }

      // Advanced tuning
      sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
      sddp_opts.elastic_filter_mode = options.sddp_elastic_mode_enum();
      sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
      sddp_opts.apertures = options.sddp_apertures();
      sddp_opts.aperture_timeout = options.sddp_aperture_timeout();
      sddp_opts.save_aperture_lp = options.sddp_save_aperture_lp();
      sddp_opts.max_cuts_per_phase = options.sddp_max_cuts_per_phase();
      sddp_opts.cut_prune_interval = options.sddp_cut_prune_interval();
      sddp_opts.prune_dual_threshold = options.sddp_prune_dual_threshold();
      sddp_opts.single_cut_storage = options.sddp_single_cut_storage();
      sddp_opts.max_stored_cuts = options.sddp_max_stored_cuts();
      sddp_opts.use_clone_pool = options.sddp_use_clone_pool();
      sddp_opts.alpha_min = options.sddp_alpha_min();
      sddp_opts.alpha_max = options.sddp_alpha_max();

      // Cut sharing and files
      sddp_opts.cut_sharing = options.sddp_cut_sharing_mode_enum();
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

      sddp_opts.cut_recovery_mode = options.sddp_cut_recovery_mode_enum();
      sddp_opts.recovery_mode = options.sddp_recovery_mode_enum();
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
      sddp_opts.lp_build = options.lp_build();
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

      // Simulation mode: clear output file to suppress all cut persistence
      if (options.sddp_simulation_mode()) {
        sddp_opts.cuts_output_file.clear();
      }

      // Wire warm_start from SddpOptions config (default: true)
      sddp_opts.warm_start = options.sddp_warm_start();

      // Wire solve_timeout from forward solver's time_limit (if set)
      {
        const auto fwd_solver = options.sddp_forward_solver_options();
        if (fwd_solver.time_limit) {
          sddp_opts.solve_timeout = *fwd_solver.time_limit;
        }
      }

      // Wire forward/backward solver options (pre-merged with global)
      sddp_opts.forward_solver_options = options.sddp_forward_solver_options();
      sddp_opts.backward_solver_options =
          options.sddp_backward_solver_options();

      return std::make_unique<SDDPPlanningMethod>(std::move(sddp_opts));
    }  // else (num_phases >= 2)
  }  // method == "sddp"

  if (options.method_type_enum() == MethodType::cascade) {
    if (num_phases > 0 && num_phases < 2) {
      SPDLOG_INFO(
          "Cascade requested but only {} phase(s); using monolithic solver",
          num_phases);
    } else {
      // Reuse the same SDDP option wiring
      SDDPOptions sddp_opts;

      sddp_opts.max_iterations = options.sddp_max_iterations();
      sddp_opts.min_iterations = options.sddp_min_iterations();
      sddp_opts.convergence_tol = options.sddp_convergence_tol();
      sddp_opts.stationary_tol = options.sddp_stationary_tol();
      sddp_opts.stationary_window = options.sddp_stationary_window();

      // Simulation mode: forward-only evaluation, no training, no cut saving
      if (options.sddp_simulation_mode()) {
        sddp_opts.max_iterations = 0;
        sddp_opts.save_per_iteration = false;
        sddp_opts.save_simulation_cuts = false;
      }

      sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
      sddp_opts.elastic_filter_mode = options.sddp_elastic_mode_enum();
      sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
      sddp_opts.apertures = options.sddp_apertures();
      sddp_opts.aperture_timeout = options.sddp_aperture_timeout();
      sddp_opts.save_aperture_lp = options.sddp_save_aperture_lp();
      sddp_opts.max_cuts_per_phase = options.sddp_max_cuts_per_phase();
      sddp_opts.cut_prune_interval = options.sddp_cut_prune_interval();
      sddp_opts.prune_dual_threshold = options.sddp_prune_dual_threshold();
      sddp_opts.single_cut_storage = options.sddp_single_cut_storage();
      sddp_opts.max_stored_cuts = options.sddp_max_stored_cuts();
      sddp_opts.use_clone_pool = options.sddp_use_clone_pool();
      sddp_opts.alpha_min = options.sddp_alpha_min();
      sddp_opts.alpha_max = options.sddp_alpha_max();

      sddp_opts.cut_sharing = options.sddp_cut_sharing_mode_enum();
      const auto output_dir_sv = options.output_directory();
      const auto cut_dir =
          (std::filesystem::path(
               output_dir_sv.empty() ? "output" : std::string(output_dir_sv))
           / options.sddp_cut_directory())
              .string();
      sddp_opts.cuts_output_file =
          (std::filesystem::path(cut_dir) / sddp_file::combined_cuts).string();

      sddp_opts.cut_recovery_mode = options.sddp_cut_recovery_mode_enum();
      sddp_opts.recovery_mode = options.sddp_recovery_mode_enum();
      if (!options.sddp_simulation_mode()) {
        sddp_opts.save_per_iteration = options.sddp_save_per_iteration();
      } else {
        sddp_opts.cuts_output_file.clear();
      }
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

      const auto sentinel = options.sddp_sentinel_file();
      const auto output_dir = options.output_directory();
      if (!sentinel.empty()) {
        sddp_opts.sentinel_file = sentinel;
      }

      sddp_opts.log_directory = std::string(options.log_directory());
      sddp_opts.lp_debug = options.lp_debug();
      sddp_opts.lp_build = options.lp_build();
      sddp_opts.lp_debug_compression = std::string(options.lp_compression());
      sddp_opts.enable_api = options.sddp_api_enabled();
      if (!output_dir.empty()) {
        sddp_opts.api_status_file =
            (std::filesystem::path(output_dir) / "sddp_status.json").string();
        sddp_opts.api_stop_request_file =
            (std::filesystem::path(output_dir) / sddp_file::stop_request)
                .string();
      }

      // Wire warm_start from SddpOptions config (default: true)
      sddp_opts.warm_start = options.sddp_warm_start();

      // Wire solve_timeout from forward solver's time_limit (if set)
      {
        const auto fwd_solver = options.sddp_forward_solver_options();
        if (fwd_solver.time_limit) {
          sddp_opts.solve_timeout = *fwd_solver.time_limit;
        }
      }

      // Wire forward/backward solver options (pre-merged with global)
      sddp_opts.forward_solver_options = options.sddp_forward_solver_options();
      sddp_opts.backward_solver_options =
          options.sddp_backward_solver_options();

      // Get cascade options (user-configured or defaults)
      CascadeOptions cascade_opts;
      cascade_opts.sddp_options = options.cascade_sddp_options();
      if (options.has_cascade_levels()) {
        cascade_opts.level_array = {options.cascade_levels().begin(),
                                    options.cascade_levels().end()};
      }

      return std::make_unique<CascadePlanningMethod>(std::move(sddp_opts),
                                                     std::move(cascade_opts));
    }  // else (num_phases >= 2)
  }  // method == "cascade"

  // Default: monolithic (also used as fallback for single-phase SDDP/cascade)
  auto solver = std::make_unique<MonolithicMethod>();
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
  solver->solve_mode = options.monolithic_solve_mode_enum();
  solver->boundary_cuts_file =
      std::string(options.monolithic_boundary_cuts_file());
  solver->boundary_cuts_mode = options.monolithic_boundary_cuts_mode_enum();
  solver->boundary_max_iterations =
      options.monolithic_boundary_max_iterations();
  return solver;
}

}  // namespace gtopt
