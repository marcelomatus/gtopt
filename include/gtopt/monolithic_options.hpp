/**
 * @file      monolithic_options.hpp
 * @brief     Monolithic solver configuration parameters
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

// ─── SolveMode ──────────────────────────────────────────────────────────────

/**
 * @brief Monolithic solver execution mode.
 */
enum class SolveMode : uint8_t
{
  monolithic = 0,  ///< Solve all phases in a single LP (default)
  sequential = 1,  ///< Solve phases sequentially
};

inline constexpr auto solve_mode_entries = std::to_array<EnumEntry<SolveMode>>({
    {.name = "monolithic", .value = SolveMode::monolithic},
    {.name = "sequential", .value = SolveMode::sequential},
});

constexpr auto enum_entries(SolveMode /*tag*/) noexcept
{
  return std::span {solve_mode_entries};
}

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
