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
#include <ranges>
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
#include <gtopt/solver_stats.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

namespace
{

/// Compute the effective equilibration method when none is set by the user.
///
/// Kirchhoff constraints on multi-bus networks produce heterogeneous row and
/// column scales (reactances span multiple orders of magnitude, loss segments
/// introduce tiny coefficients).  Ruiz equilibration handles this much better
/// than single-pass row_max.  For single-bus or non-Kirchhoff models the
/// simpler row_max default is retained.
[[nodiscard]] constexpr auto effective_equilibration_method(
    const Planning& planning) noexcept -> LpEquilibrationMethod
{
  const auto& lp_opts = planning.options.lp_matrix_options;
  if (lp_opts.equilibration_method.has_value()) {
    return *lp_opts.equilibration_method;
  }
  const auto& mo = planning.options.model_options;
  const bool use_kirchhoff = mo.use_kirchhoff.value_or(true);
  const bool use_single_bus = mo.use_single_bus.value_or(false);
  return (use_kirchhoff && !use_single_bus) ? LpEquilibrationMethod::ruiz
                                            : LpEquilibrationMethod::row_max;
}

/// Log pre-solve system statistics.
void log_pre_solve_stats(
    [[maybe_unused]] const std::vector<std::string>& planning_files,
    const Planning& planning)
{
  const auto& sys = planning.system;
  const auto& sim = planning.simulation;
  const auto& plan_opts = planning.options;

  // Single normalized element list:
  //  - snake_case keys, plural, fixed 18-char column → all colons align
  //  - zero-count rows are suppressed when the element type is optional
  //    (rights, user constraints/params, pumps); core element types are
  //    always shown so the absence (e.g. lines=0) is still visible.
  spdlog::info("=== System statistics ===");
  spdlog::info("  system             : {}", sys.name);
  spdlog::info("  version            : {}", sys.version);
  spdlog::info("=== System elements ===");
  const auto log_count = [](std::string_view label, std::size_t n)
  { spdlog::info("  {:<18} : {}", label, n); };
  const auto log_count_if = [&](std::string_view label, std::size_t n)
  {
    if (n != 0) {
      log_count(label, n);
    }
  };
  log_count("buses", sys.bus_array.size());
  log_count("generators", sys.generator_array.size());
  log_count_if("generator_profiles", sys.generator_profile_array.size());
  log_count("demands", sys.demand_array.size());
  log_count_if("demand_profiles", sys.demand_profile_array.size());
  log_count("lines", sys.line_array.size());
  log_count_if("batteries", sys.battery_array.size());
  log_count_if("converters", sys.converter_array.size());
  log_count_if("reserve_zones", sys.reserve_zone_array.size());
  log_count_if("reserve_provisions", sys.reserve_provision_array.size());
  log_count_if("junctions", sys.junction_array.size());
  log_count_if("waterways", sys.waterway_array.size());
  log_count_if("flows", sys.flow_array.size());
  log_count_if("reservoirs", sys.reservoir_array.size());
  log_count_if("reservoir_seepages", sys.reservoir_seepage_array.size());
  log_count_if("turbines", sys.turbine_array.size());
  log_count_if("pumps", sys.pump_array.size());
  log_count_if("flow_rights", sys.flow_right_array.size());
  log_count_if("volume_rights", sys.volume_right_array.size());
  log_count_if("user_constraints", sys.user_constraint_array.size());
  log_count_if("user_params", sys.user_param_array.size());
  spdlog::info("=== Simulation ===");
  log_count("blocks", sim.block_array.size());
  log_count("stages", sim.stage_array.size());
  log_count("scenarios", sim.scenario_array.size());
  spdlog::info("=== Key options ===");
  const auto& mo = plan_opts.model_options;
  const auto log_kv = [](std::string_view label, std::string_view value)
  { spdlog::info("  {:<18} : {}", label, value); };
  log_kv("use_kirchhoff", mo.use_kirchhoff.value_or(false) ? "true" : "false");
  log_kv("use_single_bus",
         mo.use_single_bus.value_or(false) ? "true" : "false");
  log_kv("scale_objective",
         std::format("{}", mo.scale_objective.value_or(1'000.0)));
  log_kv("scale_theta",
         mo.scale_theta.has_value() ? std::format("{:.6g}", *mo.scale_theta)
                                    : "auto (median reactance)");
  log_kv(
      "equilibration",
      std::format("{}{}",
                  enum_name(effective_equilibration_method(planning)),
                  plan_opts.lp_matrix_options.equilibration_method.has_value()
                      ? ""
                      : " (default)"));
  log_kv("demand_fail_cost",
         std::format("{}", mo.demand_fail_cost.value_or(0.0)));
  log_kv("input_directory", plan_opts.input_directory.value_or("(default)"));
  log_kv("output_directory", plan_opts.output_directory.value_or("(default)"));
  log_kv("output_format",
         plan_opts.output_format
             ? std::string {enum_name(*plan_opts.output_format)}
             : std::string {"(default)"});
}

/// Aggregate solver activity counters across every (scene, phase) LP.
///
/// Walks the whole grid so the summary covers monolithic runs
/// (one solve per cell) and SDDP/cascade runs (many solves per cell
/// plus elastic-clone retries folded back via `merge_solver_stats`).
[[nodiscard]] SolverStats aggregate_solver_stats(const PlanningLP& planning_lp)
{
  SolverStats agg;
  for (const auto& phase_systems : planning_lp.systems()) {
    for (const auto& sys : phase_systems) {
      agg += sys.solver_stats();
    }
  }
  return agg;
}

/// Emit a per-cell breakdown at DEBUG level.  Kept behind spdlog's
/// level gate so normal info-level runs stay concise — the aggregate
/// summary at INFO level is the expected end-user view.
void log_per_cell_solver_stats(const PlanningLP& planning_lp)
{
  if (!spdlog::should_log(spdlog::level::debug)) {
    return;
  }
  SPDLOG_DEBUG("  per-cell solver stats:");
  for (const auto& phase_systems : planning_lp.systems()) {
    for (const auto& sys : phase_systems) {
      [[maybe_unused]] const auto& s = sys.solver_stats();
      SPDLOG_DEBUG(
          "    scene={} phase={}: load_problem={} solves={} (init={}, "
          "resolve={}, fallback={}, crossover={}) infeas={} time={:.3f}s "
          "kappa={:.3g}",
          sys.scene().uid(),
          sys.phase().uid(),
          s.load_problem_calls,
          s.total_solve_calls(),
          s.initial_solve_calls,
          s.resolve_calls,
          s.fallback_solves,
          s.crossover_solves,
          s.infeasible_count,
          s.total_solve_time_s,
          s.max_kappa);
    }
  }
}

/// Post-validation: flag a mismatch between the configured low_memory
/// mode and the actual load_problem / solve counts captured by
/// SolverStats.
///
/// Heuristic (per-cell reasoning, summed across scene × phase):
///
///   * `low_memory == off`      → exactly one `load_problem` call per
///     cell; `total_backend_solves > num_cells` (each solve reuses the
///     resident backend).  If `load_problem_calls > num_cells`, some
///     backend was reconstructed unexpectedly.
///
///   * `low_memory != off`      → the backend is released between
///     solves, so every backend solve requires a matching
///     reconstruction.  Expect
///     `load_problem_calls ≈ total_backend_solves ≥ num_cells`.  If
///     `load_problem_calls ≤ num_cells`, the low-memory option was
///     configured but never exercised (no release / rebuild happened).
void validate_low_memory_usage(const PlanningLP& planning_lp,
                               const SolverStats& agg)
{
  const auto mode = planning_lp.options().sddp_low_memory();
  std::size_t num_cells = 0;
  for (const auto& phase_systems : planning_lp.systems()) {
    num_cells += phase_systems.size();
  }
  if (num_cells == 0) {
    return;
  }
  const auto load_calls = agg.load_problem_calls;
  const auto backend_solves = agg.total_backend_solves();

  if (mode == LowMemoryMode::off) {
    spdlog::info(
        "  low_memory      : off ({} load_problem for {} cells, {} solves)",
        load_calls,
        num_cells,
        backend_solves);
    if (load_calls > num_cells) {
      spdlog::warn(
          "low_memory=off but load_problem={} exceeds num_cells={}: "
          "backend was reconstructed {} extra time(s) — unexpected rebuild",
          load_calls,
          num_cells,
          load_calls - num_cells);
    }
  } else {
    spdlog::info(
        "  low_memory      : {} ({} load_problem for {} cells, {} solves)",
        enum_name(mode),
        load_calls,
        num_cells,
        backend_solves);
    if (load_calls <= num_cells) {
      spdlog::warn(
          "low_memory={} was requested but not effective: "
          "load_problem={} ≤ num_cells={} — the backend was never "
          "reconstructed between solves (expected load_problem ≈ "
          "backend_solves={})",
          enum_name(mode),
          load_calls,
          num_cells,
          backend_solves);
    } else if (backend_solves > load_calls + num_cells) {
      spdlog::warn(
          "low_memory={} ratio unexpected: {} load_problem for {} "
          "backend solves — some solves reused a live backend",
          enum_name(mode),
          load_calls,
          backend_solves);
    }
  }
}

/// Log post-solve solution statistics.
void log_post_solve_stats(const PlanningLP& planning_lp, bool optimal)
{
  spdlog::info("=== Solution statistics ===");
  spdlog::info("  Status          : {}", optimal ? "optimal" : "non-optimal");

  if (optimal && !planning_lp.systems().empty()
      && !planning_lp.systems().front().empty())
  {
    const auto& lp_if =
        planning_lp.systems().front().front().linear_interface();
    const double obj_scaled = lp_if.get_obj_value_raw();
    const double obj_unscaled = lp_if.get_obj_value();
    spdlog::info("  LP variables    : {}", lp_if.get_numcols());
    spdlog::info("  LP constraints  : {}", lp_if.get_numrows());
    spdlog::info("  Obj (scaled)    : {:.6g}", obj_scaled);
    spdlog::info("  Obj (unscaled)  : {:.6g}", obj_unscaled);
  }

  // Aggregated solver-activity counters.  Printed for every run (not
  // just optimal ones) so failures still expose where the backend spent
  // its time — infeasible counts, load_problem rebuilds under
  // low-memory mode, fallback retries, etc.
  const auto agg = aggregate_solver_stats(planning_lp);
  spdlog::info("  load_problem    : {}", agg.load_problem_calls);
  spdlog::info("  solves          : {} (initial={}, resolve={})",
               agg.total_solve_calls(),
               agg.initial_solve_calls,
               agg.resolve_calls);
  if (agg.fallback_solves != 0 || agg.crossover_solves != 0) {
    spdlog::info("  solve retries   : fallback={} crossover={}",
                 agg.fallback_solves,
                 agg.crossover_solves);
  }
  if (agg.infeasible_count != 0) {
    spdlog::info("  infeasible      : {} (primal={}, dual={})",
                 agg.infeasible_count,
                 agg.primal_infeasible,
                 agg.dual_infeasible);
  }
  if (agg.total_solve_calls() != 0) {
    spdlog::info("  avg LP size     : {:.0f} vars, {:.0f} rows",
                 agg.avg_ncols(),
                 agg.avg_nrows());
  }
  if (const auto n = agg.total_solve_calls(); n != 0) {
    spdlog::info("  solve wall time : {:.3f}s (avg {:.3f}s / {} solves)",
                 agg.total_solve_time_s,
                 agg.total_solve_time_s / static_cast<double>(n),
                 n);
  } else {
    spdlog::info("  solve wall time : {:.3f}s", agg.total_solve_time_s);
  }
  if (agg.max_kappa > 0.0) {
    spdlog::info("  Solver kappa    : {:.6g} (max across grid)", agg.max_kappa);
  }

  validate_low_memory_usage(planning_lp, agg);

  log_per_cell_solver_stats(planning_lp);
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
          .num_vars = li.get_numcols(),
          .num_constraints = li.get_numrows(),
          .stats_nnz = li.lp_stats_nnz(),
          .stats_zeroed = li.lp_stats_zeroed(),
          .stats_max_abs = li.lp_stats_max_abs(),
          .stats_min_abs = li.lp_stats_min_abs(),
          .stats_max_col = li.lp_stats_max_col(),
          .stats_min_col = li.lp_stats_min_col(),
          .stats_max_col_name = li.lp_stats_max_col_name(),
          .stats_min_col_name = li.lp_stats_min_col_name(),
          .row_type_stats =
              [&]
          {
            auto view = li.lp_row_type_stats()
                | std::views::transform(
                            [](const auto& e) -> RowTypeStats
                            {
                              return {
                                  .type = e.type,
                                  .count = e.count,
                                  .nnz = e.nnz,
                                  .max_abs = e.max_abs,
                                  .min_abs = e.min_abs,
                              };
                            });
            return std::vector<RowTypeStats>(view.begin(), view.end());
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
  // --lp-file and --lp-debug require all col+row names to be generated so
  // the solver can write a readable .lp dump.
  const bool enable_names =
      opts.lp_file.has_value() || opts.lp_debug.value_or(false);
  const auto eq_method = effective_equilibration_method(planning);
  auto flat_opts = make_lp_matrix_options(
      enable_names, opts.matrix_eps, do_stats, opts.solver, eq_method);

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
  spdlog::info("  Optimization time {:.3f}s", solve_elapsed);

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
      "  Solver options used:"
      " algorithm={}, threads={}, presolve={},"
      " optimal_eps={}, feasible_eps={}, barrier_eps={},"
      " log_level={}",
      solver_opts.algorithm,
      solver_opts.threads,
      solver_opts.presolve,
      eps_str(solver_opts.optimal_eps),
      eps_str(solver_opts.feasible_eps),
      eps_str(solver_opts.barrier_eps),
      solver_opts.log_level);
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

  spdlog::info("  Write output time {:.3f}s", out_sw.elapsed().count());
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

    // When running cascade, pre-apply the effective level-0 model overrides
    // to planning.options.model_options so the initial PlanningLP matches
    // what level 0 will solve — avoids a wasted first build that cascade
    // would throw away and rebuild.
    if (planning.options.method.value_or(MethodType::monolithic)
        == MethodType::cascade)
    {
      const auto& co = planning.options.cascade_options;
      planning.options.model_options.merge(co.model_options);
      if (!co.level_array.empty() && co.level_array.front().model_options) {
        planning.options.model_options.merge(
            *co.level_array.front().model_options);
      }
    }

    spdlog::info("=== Building LP model ===");
    const spdlog::stopwatch build_sw;
    PlanningLP planning_lp {std::move(planning),  // NOLINT
                            flat_opts};

    // Log the active solver backend (with version) once.  The
    // "Building LP done in ..." line from planning_lp already covers
    // the build wall time, so a redundant "Build lp time ..." line is
    // intentionally not emitted here.
    if (!planning_lp.systems().empty()
        && !planning_lp.systems().front().empty())
    {
      const auto& li = planning_lp.systems().front().front().linear_interface();
      spdlog::info("  Build lp time {:.3f}s — solver={}",
                   build_sw.elapsed().count(),
                   li.solver_id());
    } else {
      spdlog::info("  Build lp time {:.3f}s", build_sw.elapsed().count());
    }

    const bool want_lp_only =
        opts.lp_only.value_or(false) || planning_lp.options().lp_only();

    // lp_only is now SDDP-independent: under rebuild / compress modes
    // the PlanningLP ctor no longer eagerly flattens every cell, so
    // for the dump-and-exit path we explicitly drive a parallel
    // "build all LPs" pass here before writing / exiting.  SDDPMethod
    // is never instantiated under lp_only.
    if (want_lp_only) {
      planning_lp.build_all_lps_eagerly();
    }

    if (opts.lp_file) {
      planning_lp.write_lp(opts.lp_file.value());
    }

    if (do_stats) {
      log_lp_coefficient_stats(planning_lp);
    }

    if (want_lp_only) {
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
