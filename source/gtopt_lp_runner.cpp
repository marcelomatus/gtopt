/**
 * @file      gtopt_lp_runner.cpp
 * @brief     LP build, solve, stats, and output writing
 * @date      2026-04-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Separated from gtopt_main.cpp so that the heavy Arrow/Parquet headers
 * (pulled in via planning_lp.hpp) compile in their own translation unit,
 * enabling parallel compilation.
 */

#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtopt/error.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/gtopt_lp_runner.hpp>
#include <gtopt/lp_stats.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

namespace
{

/// Log pre-solve system statistics.
void log_pre_solve_stats(
    [[maybe_unused]] const std::vector<std::string>& planning_files,
    const Planning& planning)
{
  const auto& sys = planning.system;
  const auto& sim = planning.simulation;
  const auto& plan_opts = planning.options;

  spdlog::info("=== System statistics ===");
  spdlog::info(std::format("  System name     : {}", sys.name));
  spdlog::info(std::format("  System version  : {}", sys.version));
  spdlog::info("=== System elements  ===");
  spdlog::info(std::format("  Buses           : {}", sys.bus_array.size()));
  spdlog::info(
      std::format("  Generators      : {}", sys.generator_array.size()));
  spdlog::info(std::format("  Generator profs : {}",
                           sys.generator_profile_array.size()));
  spdlog::info(std::format("  Demands         : {}", sys.demand_array.size()));
  spdlog::info(
      std::format("  Demand profs    : {}", sys.demand_profile_array.size()));
  spdlog::info(std::format("  Lines           : {}", sys.line_array.size()));
  spdlog::info(std::format("  Batteries       : {}", sys.battery_array.size()));
  spdlog::info(
      std::format("  Converters      : {}", sys.converter_array.size()));
  spdlog::info(
      std::format("  Reserve zones   : {}", sys.reserve_zone_array.size()));
  spdlog::info(std::format("  Reserve provisions   : {}",
                           sys.reserve_provision_array.size()));
  spdlog::info(
      std::format("  Junctions       : {}", sys.junction_array.size()));
  spdlog::info(
      std::format("  Waterways       : {}", sys.waterway_array.size()));
  spdlog::info(std::format("  Flows           : {}", sys.flow_array.size()));
  spdlog::info(
      std::format("  Reservoirs      : {}", sys.reservoir_array.size()));
  spdlog::info(std::format("  ReservoirSeepages     : {}",
                           sys.reservoir_seepage_array.size()));
  spdlog::info(std::format("  Turbines        : {}", sys.turbine_array.size()));
  if (!sys.flow_right_array.empty()) {
    spdlog::info(
        std::format("  Flow rights     : {}", sys.flow_right_array.size()));
  }
  if (!sys.volume_right_array.empty()) {
    spdlog::info(
        std::format("  Volume rights   : {}", sys.volume_right_array.size()));
  }
  if (!sys.user_constraint_array.empty()) {
    spdlog::info(std::format("  User constraints: {}",
                             sys.user_constraint_array.size()));
  }
  if (!sys.user_param_array.empty()) {
    spdlog::info(
        std::format("  User params     : {}", sys.user_param_array.size()));
  }
  spdlog::info("=== Simulation statistics ===");
  spdlog::info(std::format("  Blocks          : {}", sim.block_array.size()));
  spdlog::info(std::format("  Stages          : {}", sim.stage_array.size()));
  spdlog::info(
      std::format("  Scenarios       : {}", sim.scenario_array.size()));
  spdlog::info("=== Key options ===");
  const auto& mo = plan_opts.model_options;
  spdlog::info(
      std::format("  use_kirchhoff   : {}",
                  mo.use_kirchhoff.value_or(false) ? "true" : "false"));
  spdlog::info(
      std::format("  use_single_bus  : {}",
                  mo.use_single_bus.value_or(false) ? "true" : "false"));
  spdlog::info(std::format("  scale_objective : {}",
                           mo.scale_objective.value_or(1'000.0)));
  spdlog::info(std::format("  scale_theta     : {}",
                           mo.scale_theta.has_value()
                               ? std::format("{:.6g}", *mo.scale_theta)
                               : "auto (median reactance)"));
  spdlog::info(std::format(
      "  equilibration   : {}",
      enum_name(plan_opts.lp_matrix_options.equilibration_method.value_or(
          LpEquilibrationMethod::row_max))));
  spdlog::info(
      std::format("  demand_fail_cost: {}", mo.demand_fail_cost.value_or(0.0)));
  spdlog::info(std::format("  input_directory : {}",
                           plan_opts.input_directory.value_or("(default)")));
  spdlog::info(std::format("  output_directory: {}",
                           plan_opts.output_directory.value_or("(default)")));
  spdlog::info(std::format("  output_format   : {}",
                           plan_opts.output_format
                               ? enum_name(*plan_opts.output_format)
                               : "(default)"));
}

/// Log post-solve solution statistics.
void log_post_solve_stats(const PlanningLP& planning_lp, bool optimal)
{
  spdlog::info("=== Solution statistics ===");
  spdlog::info(std::format("  Status          : {}",
                           optimal ? "optimal" : "non-optimal"));

  if (optimal && !planning_lp.systems().empty()
      && !planning_lp.systems().front().empty())
  {
    const auto& lp_if =
        planning_lp.systems().front().front().linear_interface();
    const double obj_scaled = lp_if.get_obj_value();
    const double obj_unscaled = lp_if.get_obj_value_physical();
    spdlog::info(std::format("  LP variables    : {}", lp_if.get_numcols()));
    spdlog::info(std::format("  LP constraints  : {}", lp_if.get_numrows()));
    spdlog::info(std::format("  Obj (scaled)    : {:.6g}", obj_scaled));
    spdlog::info(std::format("  Obj (unscaled)  : {:.6g}", obj_unscaled));
    spdlog::info(std::format("  Solver kappa    : {:.6g}", lp_if.get_kappa()));
  }
}

/// Log LP coefficient statistics for all scene x phase LPs.
void log_lp_coefficient_stats(const PlanningLP& planning_lp)
{
  std::vector<ScenePhaseLPStats> lp_entries;
  for (auto&& [si, phase_systems] :
       enumerate<SceneIndex>(planning_lp.systems()))
  {
    for (auto&& [pi, system_lp] : enumerate<PhaseIndex>(phase_systems)) {
      const auto& li = system_lp.linear_interface();
      lp_entries.push_back({
          .scene_uid = system_lp.scene().uid(),
          .phase_uid = system_lp.phase().uid(),
          .num_vars = static_cast<int>(li.get_numcols()),
          .num_constraints = static_cast<int>(li.get_numrows()),
          .stats_nnz = li.lp_stats_nnz(),
          .stats_zeroed = li.lp_stats_zeroed(),
          .stats_max_abs = li.lp_stats_max_abs(),
          .stats_min_abs = li.lp_stats_min_abs(),
          .stats_max_col = static_cast<int>(li.lp_stats_max_col()),
          .stats_min_col = static_cast<int>(li.lp_stats_min_col()),
          .stats_max_col_name = std::string(li.lp_stats_max_col_name()),
          .stats_min_col_name = std::string(li.lp_stats_min_col_name()),
          .row_type_stats =
              [&]
          {
            std::vector<RowTypeStats> rts;
            for (const auto& e : li.lp_row_type_stats()) {
              rts.push_back({
                  .type = e.type,
                  .count = e.count,
                  .nnz = e.nnz,
                  .max_abs = e.max_abs,
                  .min_abs = e.min_abs,
              });
            }
            return rts;
          }(),
      });
    }
  }
  log_lp_stats_summary(lp_entries,
                       planning_lp.options().lp_coeff_ratio_threshold());
}

/// Prepare LP build options and apply per-solver config.
[[nodiscard]] LpMatrixOptions prepare_matrix_options(Planning& planning,
                                                     const MainOptions& opts,
                                                     bool do_stats)
{
  // CLI --lp-names-level overrides --set; fall back to merged planning.
  auto eff_names_level = opts.lp_names_level
      ? opts.lp_names_level
      : planning.options.lp_matrix_options.names_level;

  // Multi-phase / SDDP / cascade methods need at least minimal column names
  // for state variable transfer and cut I/O.
  const auto method = planning.options.method.value_or(MethodType::monolithic);
  const bool needs_state_names = method == MethodType::sddp
      || method == MethodType::cascade
      || planning.simulation.phase_array.size() > 1;
  if (needs_state_names) {
    if (!eff_names_level || *eff_names_level < LpNamesLevel::minimal) {
      eff_names_level = LpNamesLevel::minimal;
    }
  }

  // --lp-file and --lp-debug require *all* col+row names to be generated so
  // the solver can write a readable .lp dump.  Bump the effective names
  // level to cols_and_rows whenever LP file output is requested; this
  // avoids silently emitting an .lp file with missing row names.
  const bool needs_full_names = opts.lp_file.has_value() || opts.lp_debug;
  if (needs_full_names
      && (!eff_names_level || *eff_names_level < LpNamesLevel::cols_and_rows))
  {
    eff_names_level = LpNamesLevel::cols_and_rows;
  }
  auto flat_opts = make_lp_matrix_options(
      eff_names_level,
      opts.matrix_eps,
      do_stats,
      opts.solver,
      planning.options.lp_matrix_options.equilibration_method);

  if (do_stats) {
    log_pre_solve_stats(opts.planning_files, planning);
  }

  // Apply per-solver config from .gtopt.conf [solver.<name>] sections.
  if (const auto& solver_key = flat_opts.solver_name; !solver_key.empty()) {
    if (const auto it = opts.solver_configs.find(solver_key);
        it != opts.solver_configs.end())
    {
      auto& so = planning.options.solver_options;
      auto conf = it->second;
      conf.overlay(so);
      so = conf;
    }
  }

  return flat_opts;
}

/// Run the solver and return whether an optimal solution was found.
[[nodiscard]] bool run_solver(PlanningLP& planning_lp)
{
  const spdlog::stopwatch solve_sw;

  const auto& plp_opts_ref = planning_lp.options();
  const auto method = plp_opts_ref.method_type_enum();
  const SolverOptions solver_opts =
      (method == MethodType::sddp || method == MethodType::cascade)
      ? plp_opts_ref.sddp_forward_solver_options()
      : plp_opts_ref.monolithic_solver_options();
  const auto result = planning_lp.resolve(solver_opts);
  const auto solve_elapsed =
      std::chrono::duration<double>(solve_sw.elapsed()).count();
  spdlog::info(std::format("  Optimization time {:.3f}s", solve_elapsed));

  if (result.has_value()) {
    return true;
  }

  const auto& err = result.error();
  // Use warn for non-optimal (e.g. time-limit with feasible incumbent),
  // error only for hard failures such as infeasibility.
  const auto msg = std::format(
      "Solver did not find an optimal solution: "
      "{} (code={})",
      err.message,
      static_cast<int>(err.code));
  if (err.code == ErrorCode::SolverError) {
    spdlog::warn(msg);
  } else {
    spdlog::error(msg);
  }
  // Format optional tolerances: show "(default)" when not set.
  const auto eps_str = [](std::optional<double> v) -> std::string
  { return v ? std::format("{}", *v) : "(default)"; };
  spdlog::error(
      std::format("  Solver options used:"
                  " algorithm={}, threads={}, presolve={},"
                  " optimal_eps={}, feasible_eps={}, barrier_eps={},"
                  " log_level={}",
                  solver_opts.algorithm,
                  solver_opts.threads,
                  solver_opts.presolve,
                  eps_str(solver_opts.optimal_eps),
                  eps_str(solver_opts.feasible_eps),
                  eps_str(solver_opts.barrier_eps),
                  solver_opts.log_level));
  return false;
}

/// Write solution output and save planning JSON to the output directory.
[[nodiscard]] std::expected<void, std::string> write_solution_output(
    PlanningLP& planning_lp)
{
  spdlog::info("=== Output writing ===");
  const spdlog::stopwatch out_sw;
  try {
    planning_lp.write_out();
  } catch (const std::exception& ex) {
    return std::unexpected(std::format("Error writing output: {}", ex.what()));
  }

  // Save the merged planning JSON into the output directory so that
  // post-processing tools have a self-contained reference.
  {
    const auto out_dir = planning_lp.options().output_directory();
    const auto planning_json =
        (std::filesystem::path(out_dir) / "planning.json").string();
    auto pj_result = write_json_output(planning_lp.planning(), planning_json);
    if (pj_result) {
      spdlog::info("  planning JSON saved to {}", planning_json);
    } else {
      spdlog::warn("  failed to save planning JSON: {}", pj_result.error());
    }
  }

  spdlog::info(
      std::format("  Write output time {:.3f}s", out_sw.elapsed().count()));
  return {};
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────────

std::expected<int, std::string> build_solve_and_output(Planning&& planning,
                                                       const MainOptions& opts)
{
  try {
    const bool do_stats = opts.print_stats.value_or(true)
        || planning.options.lp_matrix_options.compute_stats.value_or(false);
    const auto flat_opts = prepare_matrix_options(planning, opts, do_stats);

    spdlog::info("=== Building LP model ===");
    const spdlog::stopwatch build_sw;
    PlanningLP planning_lp {std::move(planning),  // NOLINT
                            flat_opts};
    spdlog::info(
        std::format("  Build lp time {:.3f}s", build_sw.elapsed().count()));

    // Log the active solver backend so monitoring tools can display it.
    if (!planning_lp.systems().empty()
        && !planning_lp.systems().front().empty())
    {
      const auto& li = planning_lp.systems().front().front().linear_interface();
      spdlog::info(std::format("  Solver: {}", li.solver_id()));
    }

    if (opts.lp_file) {
      planning_lp.write_lp(opts.lp_file.value());
    }

    if (do_stats) {
      log_lp_coefficient_stats(planning_lp);
    }

    // lp_only: skip solving if only LP assembly was requested
    if (opts.lp_only.value_or(false) || planning_lp.options().lp_only()) {
      spdlog::info("lp_only: all LP matrices built, skipping solve");
      return 0;
    }

    // Solve the LP
    spdlog::info("=== System optimization ===");
    const bool optimal = run_solver(planning_lp);

    if (do_stats) {
      log_post_solve_stats(planning_lp, optimal);
    }

    if (!optimal) {
      return 1;
    }

    // Write solution output
    if (auto wr = write_solution_output(planning_lp); !wr) {
      return std::unexpected(std::move(wr.error()));
    }

    return 0;

  } catch (const std::exception& ex) {
    return std::unexpected(
        std::format("Error during LP creation or solving: {}", ex.what()));
  }
}

}  // namespace gtopt
