/**
 * @file      monolithic_options.hpp
 * @brief     Monolithic solver configuration parameters
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/monolithic_enums.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

// ─── MipStartOptions struct ─────────────────────────────────────────────────

/**
 * @brief Initial-MIP-solution (warm-start) configuration.
 *
 * Two independently-toggleable stages run before the monolithic MIP solve:
 *
 *   A. **Relaxation analysis** — solve the LP relaxation (under its own
 *      `relax_solver_options`), check feasibility, and (optionally) report the
 *      saturated / binding constraints.  On infeasibility, `on_infeasible`
 *      selects stop / warn / feasopt-diagnose.
 *   B. **Integer injection** — when `method != none`, compute a starting
 *      integer solution from the relaxation and inject it as a backend MIP
 *      start with the given `effort`.
 *
 * Stage A runs whenever `method != none` (the generator needs the relaxation)
 * or `relax_check` is explicitly true (diagnosis-only, no injection).  All
 * fields optional; defaults applied at the consume site.
 */
struct MipStartOptions
{
  // ── Stage B: integer-solution generator & injection ───────────────────────
  /** @brief Generator strategy: none (default), lp_round, relax_fix */
  std::optional<MipStartMethod> method {};
  /** @brief Threshold for rounding a relaxed binary up to 1 (default 0.5) */
  OptReal round_threshold {};
  /** @brief Backend effort for the injected start (default `repair` — the
   * rounded+repaired commitment is near-feasible but usually still nicks a few
   * Pmin/UC/reserve rows, so the solver's repair heuristic mends them and lands
   * the optimum at the root; `check_feasibility` only accepts an
   * already-feasible start). */
  std::optional<MipStartEffort> effort {};
  /** @brief Optional path to an externally-computed start, read by the `file`
   * generator: a dump produced by a previous solve's `dump_file`. */
  OptName file {};
  /** @brief Optional path to DUMP this solve's integer solution to, after the
   * MIP solve completes.  A later run replays it via `method=file` — enables a
   * cross-solver hand-off (e.g. HiGHS dumps a feasible incumbent, cuOpt
   * replays it as its start). */
  OptName dump_file {};
  /** @brief Optional SCIP repair STAGE (default false).  When true, the
   * candidate produced by `method` (round + electric rules, or file) is handed
   * to SCIP, whose completesol/repair heuristics turn it into a genuinely
   * feasible integer solution before injection.  Composable with ANY base
   * method and ANY active solver (cuOpt/HiGHS/CPLEX); requires the SCIP plugin
   * (self-skips, keeping the pre-SCIP candidate, when absent). */
  OptBool scip_repair {};

  // ── Stage 2 add-on: peak-injection domain rule ────────────────────────────
  /** @brief Seed storage / hydro injection units to be committed ON during the
   * relevant window in the rounded integer start (the `PeakInjectionRule`
   * domain rule).  Two unit classes, distinguished by topology:
   *   - reservoir-fed HYDRO (any turbine-linked generator): commit ON across
   *     the evening PEAK window `[peak_start_hour, peak_end_hour)` so it
   * injects when the system is most stressed.
   *   - BATTERY discharge (any converter's discharge generator): seed the unit
   *     ACTIVE across BOTH the SOLAR window `[solar_start_hour,
   * solar_end_hour)` (charge) and the evening PEAK window (discharge),
   * approximating a single daily charge→discharge cycle.  NOTE: an
   * `expand_batteries` battery exposes a SINGLE shared activity binary (the
   * `uc_<bat>_gen` commitment `u`), not separate charge/discharge binaries, so
   * the binary marks "active" in both windows while the continuous
   * charge/discharge columns (which the MIP optimizes) pick the direction.
   * Opt-in (default false): the bias encodes a power-system heuristic and
   * relies on a stage-relative hour-of-day mapping, so it is enabled
   * deliberately. Conservative — only ever flips a status binary toward ON
   * inside a window (never OFF, never outside), so the MIP still validates and
   * re-optimizes. */
  OptBool peak_injection {};
  /** @brief Inclusive start hour-of-day of the evening peak / discharge window
   * `[0, 24)` for `peak_injection` (default 18).  Hour-of-day is
   * `(stage.timeinit + intra-stage cumulative duration) mod 24`, i.e. the
   * planning horizon starts at hour 0.  Only chronological stages carry
   * commitment binaries. */
  OptReal peak_start_hour {};
  /** @brief Exclusive end hour-of-day of the evening peak / discharge window
   * `[0, 24]` for `peak_injection` (default 23).  A block falls in the window
   * when its start hour-of-day h satisfies
   * `peak_start_hour <= h < peak_end_hour`. */
  OptReal peak_end_hour {};
  /** @brief Inclusive start hour-of-day of the BATTERY solar-charge window
   * `[0, 24)` (default 9).  Batteries are seeded active across this window to
   * approximate daytime charging.  A fixed fallback window — robust per-block
   * solar-profile detection is a follow-up; set equal to `solar_end_hour` to
   * disable the charge-window seed. */
  OptReal solar_start_hour {};
  /** @brief Exclusive end hour-of-day of the BATTERY solar-charge window
   * `[0, 24]` (default 17). */
  OptReal solar_end_hour {};

  // ── Stage A: relaxation analysis & diagnosis ──────────────────────────────
  /** @brief Run the LP-relaxation feasibility analysis even when
   * `method == none` (diagnosis-only).  When `method != none` the relaxation
   * is solved regardless. Default false. */
  OptBool relax_check {};
  /** @brief Policy when the LP relaxation is infeasible: stop (default),
   * warn, or feasopt (diagnose then stop). */
  std::optional<RelaxInfeasibleAction> on_infeasible {};
  /** @brief Print the saturated / binding constraints (nonzero dual) from the
   * LP relaxation. Default false. */
  OptBool report_saturated {};
  /** @brief Solver options (algorithm, params, time limit, …) applied to the
   * LP-relaxation solve only — overlaid on the monolithic solver options so
   * the relaxation can use a different algorithm than the MIP. */
  std::optional<SolverOptions> relax_solver_options {};

  void merge(MipStartOptions&& opts)
  {
    merge_opt(method, opts.method);
    merge_opt(round_threshold, opts.round_threshold);
    merge_opt(effort, opts.effort);
    merge_opt(file, std::move(opts.file));
    merge_opt(dump_file, std::move(opts.dump_file));
    merge_opt(scip_repair, opts.scip_repair);
    merge_opt(peak_injection, opts.peak_injection);
    merge_opt(peak_start_hour, opts.peak_start_hour);
    merge_opt(peak_end_hour, opts.peak_end_hour);
    merge_opt(solar_start_hour, opts.solar_start_hour);
    merge_opt(solar_end_hour, opts.solar_end_hour);
    merge_opt(relax_check, opts.relax_check);
    merge_opt(on_infeasible, opts.on_infeasible);
    merge_opt(report_saturated, opts.report_saturated);
    if (opts.relax_solver_options.has_value()) {
      if (relax_solver_options.has_value()) {
        relax_solver_options->merge(*opts.relax_solver_options);
      } else {
        relax_solver_options = opts.relax_solver_options;
      }
    }

    auto _ = std::move(opts);
  }
};

// ─── MonolithicOptions struct ───────────────────────────────────────────────

/**
 * @brief Monolithic solver configuration parameters
 *
 * Groups monolithic-solver-specific options into a single sub-object for
 * clearer JSON organization.  All fields are optional — defaults are
 * applied via `PlanningOptionsLP`.
 */
struct MonolithicOptions
{
  /** @brief Solve mode: monolithic (default) or sequential */
  std::optional<SolveMode> solve_mode {};
  /** @brief CSV file with boundary (future-cost) cuts.
   *
   * When non-empty, the monolithic solver loads boundary cuts from this
   * file before solving.  The cuts approximate the expected future cost
   * beyond the planning horizon (analogous to SDDP boundary cuts).
   */
  OptName boundary_cuts_file {};
  /** @brief Boundary cuts load mode: noload, separated (default),
   * or combined */
  std::optional<BoundaryCutsMode> boundary_cuts_mode {};
  /** @brief Terminal-α sharing for boundary cuts: per_scene (default),
   * shared, or multicut.
   *
   * Mirrors `SddpOptions::boundary_cut_sharing_mode`.  Under `multicut` the
   * terminal phase carries N α columns (varphi_0..N-1) and each scenario's
   * boundary cut is routed to its own varphi_s in every scene-LP. */
  std::optional<BoundaryCutSharingMode> boundary_cut_sharing_mode {};
  /** @brief Maximum iterations to load from boundary cuts file (0 = all) */
  OptInt boundary_max_iterations {};

  // ── LP solver options (per-method override) ────────────────────────────────
  /** @brief Optional LP solver configuration for monolithic.
   *
   * When set, these options are merged with (and override) the global
   * `Options::solver_options`.  Allows the monolithic solver to use a
   * different time limit or algorithm than SDDP.
   */
  std::optional<SolverOptions> solver_options {};

  // ── Initial-MIP-solution (warm-start) options ──────────────────────────────
  /** @brief Optional initial-MIP-solution generator configuration.
   *
   * When `method` is set to something other than `none`, the monolithic
   * solver computes a starting integer solution and injects it into the
   * backend before branch-and-cut (see `MipStartOptions`).
   */
  std::optional<MipStartOptions> mip_start {};

  void merge(MonolithicOptions&& opts)
  {
    merge_opt(solve_mode, opts.solve_mode);
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, opts.boundary_cuts_mode);
    merge_opt(boundary_cut_sharing_mode, opts.boundary_cut_sharing_mode);
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    if (opts.mip_start.has_value()) {
      if (mip_start.has_value()) {
        mip_start->merge(std::move(*opts.mip_start));
      } else {
        mip_start = std::move(opts.mip_start);
      }
    }
    if (opts.solver_options.has_value()) {
      if (solver_options.has_value()) {
        solver_options->merge(*opts.solver_options);
      } else {
        solver_options = opts.solver_options;
      }
    }

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
