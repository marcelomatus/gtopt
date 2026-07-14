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

// ─── MipStartOptions: staged warm-start configuration ───────────────────────
//
// The initial-MIP-solution (warm-start) pipeline runs as a sequence of cleanly
// separated stages immediately before the monolithic MIP solve:
//
//   relax → round → seed → [scip_repair] → inject
//
// `enabled` is the master on/off switch (replaces the old `method != none`).
// Stage 0 (relax) ALSO runs when `relax.check` is true on its own, for a
// diagnosis-only pass with no injection.  `from_file` swaps the round+rules
// candidate source for a replayed dump (cross-solver hand-off).  Every field
// is optional; defaults are applied at the consume site.

/// STAGE 0: LP-relaxation analysis & diagnosis.  Runs whenever the pipeline is
/// `enabled` (the generator needs the relaxation) or `check` is explicitly true
/// (diagnosis-only, no injection).
struct MipStartRelax
{
  /** @brief Solver options (algorithm, params, time limit, …) applied to the
   * LP-relaxation solve only — overlaid on the monolithic solver options so
   * the relaxation can use a different algorithm than the MIP. */
  std::optional<SolverOptions> solver_options {};
  /** @brief Run the LP-relaxation feasibility analysis even when the pipeline
   * is not `enabled` (diagnosis-only). Default false. */
  OptBool check {};
  /** @brief Print the saturated / binding constraints (nonzero dual) from the
   * LP relaxation. Default false. */
  OptBool report_saturated {};
  /** @brief Policy when the LP relaxation is infeasible: stop (default),
   * warn, or feasopt (diagnose then stop). */
  std::optional<RelaxInfeasibleAction> on_infeasible {};

  void merge(MipStartRelax&& opts)
  {
    if (opts.solver_options.has_value()) {
      if (solver_options.has_value()) {
        solver_options->merge(*opts.solver_options);
      } else {
        solver_options = opts.solver_options;
      }
    }
    merge_opt(check, opts.check);
    merge_opt(report_saturated, opts.report_saturated);
    merge_opt(on_infeasible, opts.on_infeasible);
    auto _ = std::move(opts);
  }
};

/// STAGE 1: round the relaxation's integer columns.
struct MipStartRound
{
  /** @brief Threshold for rounding a relaxed binary up to 1 (default 0.5). */
  OptReal threshold {};

  void merge(MipStartRound&& opts)
  {
    merge_opt(threshold, opts.threshold);
    auto _ = std::move(opts);
  }
};

/// STAGE 4 (optional): SCIP feasibility repair.  When `enabled`, the
/// round+rules (or file) candidate is handed to SCIP, whose completesol/repair
/// heuristics turn it into a genuinely feasible integer solution before
/// injection.  Composable with ANY active solver; requires the SCIP plugin
/// (self-skips, keeping the pre-SCIP candidate, when absent).  Default false.
struct MipStartScipRepair
{
  OptBool enabled {};

  void merge(MipStartScipRepair&& opts)
  {
    merge_opt(enabled, opts.enabled);
    auto _ = std::move(opts);
  }
};

/// STAGE 5: inject the candidate as a backend MIP start.
struct MipStartInject
{
  /** @brief Backend effort for the injected start (default `repair` — the
   * rounded+repaired commitment is near-feasible but usually still nicks a few
   * Pmin/UC/reserve rows, so the solver's repair heuristic mends them and
   * lands the optimum at the root; `check_feasibility` only accepts an
   * already-feasible start). */
  std::optional<MipStartEffort> effort {};

  void merge(MipStartInject&& opts)
  {
    merge_opt(effort, opts.effort);
    auto _ = std::move(opts);
  }
};

/// Initial-MIP-solution (warm-start) configuration: a staged pipeline.
struct MipStartOptions
{
  /** @brief Master on/off switch for the warm-start pipeline.  Replaces the
   * legacy `method != none`.  When false the pipeline still runs Stage 0 iff
   * `relax.check` is true (diagnosis-only). Default false. */
  OptBool enabled {};
  /** @brief STAGE 0 — LP relaxation analysis & diagnosis. */
  MipStartRelax relax {};
  /** @brief STAGE 1 — round the relaxation's integer columns. */
  MipStartRound round {};
  /** @brief STAGE 4 (optional) — SCIP feasibility repair. */
  MipStartScipRepair scip_repair {};
  /** @brief STAGE 5 — inject the candidate as a backend MIP start. */
  MipStartInject inject {};
  /** @brief Optional path to an externally-computed start to REPLAY instead of
   * the round+rules candidate: a dump produced by a previous solve's
   * `dump_file`.  A complete dump (integers AND continuous) reconstructs a
   * consistent start — replaying an optimal dump is accepted verbatim under
   * `inject.effort=check_feasibility`.  Enables a cross-solver hand-off (e.g.
   * HiGHS dumps a feasible incumbent, cuOpt replays it as its start).  Pair
   * with `skip_relaxation` to inject without solving the throwaway Stage-0
   * relaxation. */
  OptName from_file {};
  /** @brief Optional path to DUMP this solve's COMPLETE solution to (every
   * column, integer and continuous), after the MIP solve completes.  A later
   * run replays it via `from_file`. */
  OptName dump_file {};
  /** @brief Optional path to an external commitment SEED — a CSV
   * `generator_uid,block_uid,u` consumed by `SeedCommitmentRule` (registered
   * FIRST in the domain pipeline).  Its (generator, block) statuses seed the
   * rounded commitment for the elements it matches; unmatched elements keep the
   * rounded-relaxation value, and the standard rules then repair v/w and
   * run-lengths.  The generic loader — any strategy (previous-day PLEXOS /
   * gtopt, nearest-historical, ML predictor) simply produces this CSV. */
  OptName seed_solution_file {};
  /** @brief Skip the Stage-0 LP relaxation solve when a `seed_solution_file`
   * fully specifies the integer commitment or a `from_file` dump is being
   * replayed.  The relaxation exists only to fill the rounded candidate; a
   * seed/dump overrides it, so the relaxation is a throwaway LP solve that
   * the MIP's own root then repeats.  With this set, the start is built
   * directly (seed: zero base + domain rules; file: the dumped complete
   * solution) and injected; the backend completes any missing continuous
   * dispatch as the single warm root LP (effort defaults to `solve_fixed` →
   * dual simplex off the fixed integers, no crossover). Default false. */
  OptBool skip_relaxation {};
  /** @brief Optional path to a cached ROOT LP basis (Experiment A).
   *
   * The full-model root LP relaxation is IDENTICAL across warm-start runs (the
   * integer MIP-start only feeds the incumbent heuristic, not the relaxation),
   * yet CPLEX recomputes a cold barrier+crossover on every run.  When set:
   *   - file MISSING → solve cold as usual, then capture the root LP basis
   *     (`get_basis`) and `save_basis` it here for later runs.
   *   - file PRESENT → `load_basis` and install it (`set_basis`, reconciled to
   *     current dims) before the MIP solve, switching the MIP root LP to primal
   *     simplex + Advance so CPLEX enters at the optimal vertex (≈0 simplex
   *     iterations), skipping the barrier.  Barrier (`LPMethod=4`) IGNORES an
   *     installed basis, so a loaded basis MUST force the root to primal
   *     simplex — done via `SolverOptions::advanced_basis` (which the CPLEX
   *     backend maps to `ADVIND=1` + primal `LPMethod`/`StartAlgorithm`).
   * Only honored by the CPLEX backend; other backends ignore it (no basis I/O
   * path).  Default unset (no caching; legacy behaviour). */
  OptName root_basis_cache_file {};

  void merge(MipStartOptions&& opts)
  {
    merge_opt(enabled, opts.enabled);
    relax.merge(std::move(opts.relax));
    round.merge(std::move(opts.round));
    scip_repair.merge(std::move(opts.scip_repair));
    inject.merge(std::move(opts.inject));
    merge_opt(from_file, std::move(opts.from_file));
    merge_opt(dump_file, std::move(opts.dump_file));
    merge_opt(seed_solution_file, std::move(opts.seed_solution_file));
    merge_opt(skip_relaxation, opts.skip_relaxation);
    merge_opt(root_basis_cache_file, std::move(opts.root_basis_cache_file));

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
   * When `mip_start.enabled` is true, the monolithic solver computes a starting
   * integer solution (relax → round → seed → [scip_repair]) and injects
   * it into the backend before branch-and-cut (see `MipStartOptions`).
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
