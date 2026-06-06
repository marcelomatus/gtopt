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
  /** @brief Maximum iterations to load from boundary cuts file (0 = all) */
  OptInt boundary_max_iterations {};
  /** @brief Derive a terminal soft cost for the state variables named in the
   * boundary cut from that cut's coefficients (none | min | avg | max).
   *
   * Mirrors `SddpOptions::boundary_cut_soft_cost`.  When `min`/`avg`/`max`,
   * each reservoir / battery named in the cut gets `efin_cost` set to the
   * negated statistic of its cut coefficient (the marginal water value),
   * softening `vol_end >= efin`.  `none` (default) leaves soft costs alone.
   */
  std::optional<BoundaryCutSoftCost> boundary_cut_soft_cost {};

  // ── LP solver options (per-method override) ────────────────────────────────
  /** @brief Optional LP solver configuration for monolithic.
   *
   * When set, these options are merged with (and override) the global
   * `Options::solver_options`.  Allows the monolithic solver to use a
   * different time limit or algorithm than SDDP.
   */
  std::optional<SolverOptions> solver_options {};

  void merge(MonolithicOptions&& opts)
  {
    merge_opt(solve_mode, opts.solve_mode);
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, opts.boundary_cuts_mode);
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    merge_opt(boundary_cut_soft_cost, opts.boundary_cut_soft_cost);
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
