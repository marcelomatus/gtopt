/**
 * @file      simulation.hpp
 * @brief     Header of Simulation class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Simulation class which contains all time-structure
 * elements: blocks, stages, scenarios, phases, and scenes. Together these
 * define the multi-stage, multi-scenario optimization horizon.
 *
 * ### Time-structure hierarchy
 * ```text
 * Scenario  (probability_factor)
 *   └─ Phase
 *        └─ Stage  (discount_factor, first_block, count_block)
 *             └─ Block  (duration [h])
 * ```
 *
 * @see Block, Stage, Scenario, Phase, Scene for element definitions
 */

#pragma once

#include <gtopt/aperture.hpp>
#include <gtopt/block.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Complete time-structure of a planning simulation
 *
 * A Simulation bundles all temporal elements.  When a field is absent from
 * the input JSON, gtopt uses a default single scenario, single stage/block
 * configuration so that simple single-snapshot cases require minimal input.
 *
 * @note `phase_array` and `scene_array` default to empty.  When they are
 * empty, `SimulationLP` automatically falls back to a single default `Phase{}`
 * / `Scene{}` so the LP can always be assembled.  Provide explicit entries in
 * these arrays only when you need multiple phases or scenes.
 *
 * Multiple JSON files can be merged with `Planning::merge()`, allowing the
 * time structure to be split across files (e.g. blocks in one file, stages
 * in another).
 */
struct Simulation
{
  Array<Block> block_array {};  ///< Ordered list of time blocks
  Array<Stage> stage_array {};  ///< Ordered list of planning stages
  Array<Scenario> scenario_array {};  ///< List of stochastic scenarios
  Array<Phase> phase_array {};  ///< List of planning phases
  Array<Scene> scene_array {};  ///< List of scene combinations
  Array<Aperture> aperture_array {};  ///< Aperture definitions for SDDP
                                      ///< backward pass (optional)
  Array<Iteration> iteration_array {};  ///< Per-iteration solver control
                                        ///< (optional, keyed by index)

  // ── Discounting and boundary conditions ───────────────────────────────────

  /** @brief Annual discount rate for present-value cost calculations.
   *
   * Applied per-stage as ``exp(-ln(1+r) × t / 8766)`` where ``t`` is the
   * cumulative hours from the start of the horizon.  Combined with each
   * stage's ``discount_factor`` to form the effective discount factor.
   * Default: 0.0 (no annual discounting).
   */
  OptReal annual_discount_rate {};

  /** @brief CSV file with boundary (future-cost) cuts for the last phase.
   *
   * External optimality cuts that approximate the expected future cost
   * beyond the planning horizon.  Used by both SDDP and monolithic
   * solvers.  If empty, no boundary cuts are loaded.
   */
  OptName boundary_cuts_file {};

  /** @brief Path to a parquet/csv file mapping blocks to calendar hours.
   *
   * Metadata reference produced by plp2gtopt (from ``indhor.csv``) and
   * consumed by downstream post-processing tools (e.g. ``ts2gtopt``'s
   * ``build_hour_block_map``) to reconstruct hourly series from
   * block-granularity solver output.  Not used by the LP solver itself.
   */
  OptName block_hour_map {};

  /** @brief Valuation basis for boundary cut coefficients and RHS.
   *
   * - `end_of_horizon` (default): no discounting applied.
   * - `present_value`: apply the last-stage effective discount factor.
   */
  std::optional<BoundaryCutsValuation> boundary_cuts_valuation {};

  /** @brief When to rescale scenario/scene probabilities to sum 1.0.
   *
   * - `none`:    warn only, no rescaling.
   * - `build`:   rescale at build/validation time.
   * - `runtime`: rescale at build time and at runtime (default).
   *
   * @see ProbabilityRescaleMode
   */
  std::optional<ProbabilityRescaleMode> probability_rescale {};

  /** @brief What to do when an LP solve produces a high condition number.
   *
   * - `none`:    no checking.
   * - `warn`:    log a warning (default).
   * - `save_lp`: warn and save the LP file.
   *
   * @see KappaWarningMode
   */
  std::optional<KappaWarningMode> kappa_warning {};

  /** @brief Condition-number threshold above which kappa warnings fire.
   *
   * Default: 1e9.  Only used when `kappa_warning` is not `none`.
   */
  OptReal kappa_threshold {};

  constexpr void merge(Simulation&& sim)
  {
    gtopt::merge(block_array, std::move(sim.block_array));
    gtopt::merge(stage_array, std::move(sim.stage_array));
    gtopt::merge(scenario_array, std::move(sim.scenario_array));
    gtopt::merge(phase_array, std::move(sim.phase_array));
    gtopt::merge(scene_array, std::move(sim.scene_array));
    gtopt::merge(aperture_array, std::move(sim.aperture_array));
    gtopt::merge(iteration_array, std::move(sim.iteration_array));
    merge_opt(annual_discount_rate, sim.annual_discount_rate);
    merge_opt(boundary_cuts_file, std::move(sim.boundary_cuts_file));
    merge_opt(block_hour_map, std::move(sim.block_hour_map));
    merge_opt(boundary_cuts_valuation, sim.boundary_cuts_valuation);
    merge_opt(probability_rescale, sim.probability_rescale);
    merge_opt(kappa_warning, sim.kappa_warning);
    merge_opt(kappa_threshold, sim.kappa_threshold);

    auto _ = std::move(sim);  // move/clear sim
  }
};

}  // namespace gtopt
