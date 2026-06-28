/**
 * @file      monolithic_enums.hpp
 * @brief     Named enum types for monolithic solver options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by MonolithicOptions.  Extracted from
 * monolithic_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- SolveMode --------------------------------------------------------------

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

[[nodiscard]] constexpr auto enum_entries(SolveMode /*tag*/) noexcept
{
  return std::span {solve_mode_entries};
}

// --- MipStartMethod ---------------------------------------------------------

/**
 * @brief Strategy used to compute an initial MIP solution (warm incumbent)
 *        injected into the solver before branch-and-cut.
 *
 * The monolithic MIP exhibits an "incumbent cliff": the solver's node-0
 * heuristic finds a costly integer-feasible point (soft slacks make every
 * commitment feasible) while the root LP bound is near-optimal, so the gap
 * sits high for thousands of nodes.  Supplying a good starting commitment
 * bypasses the cliff.  Generators are pluggable and storage-safe (whole
 * horizon, no time chunking).
 *
 * - `none` (default): no MIP-start is computed; legacy behaviour.
 * - `lp_round`: solve the LP relaxation, round the integer columns, and feed
 *   the rounded vector as the start.
 * - `relax_fix`: as `lp_round`, then pin every binary at once and solve one
 *   full-horizon economic-dispatch LP, injecting the validated dispatch.
 *   Storage-safe — the whole horizon is fixed simultaneously.
 * - `file`: replay an integer solution dumped by a previous solve
 *   (`mip_start.dump_file`) — overlay those integer-column values onto this
 *   solver's own LP-relaxation base.  Enables a CROSS-SOLVER hand-off: e.g.
 *   solver A (good MIP heuristics) finds a feasible incumbent and dumps it,
 *   then solver B replays it as its start.  Both runs build the identical
 *   flat LP (deterministic), so raw column indices match 1:1.
 */
enum class MipStartMethod : uint8_t
{
  none = 0,  ///< No initial MIP solution (default)
  lp_round = 1,  ///< Round the LP relaxation
  relax_fix = 2,  ///< Fix all binaries + full-horizon ED-LP
  file = 3,  ///< Replay an integer solution dumped by a previous solve
};

inline constexpr auto mip_start_method_entries =
    std::to_array<EnumEntry<MipStartMethod>>({
        {.name = "none", .value = MipStartMethod::none},
        {.name = "lp_round", .value = MipStartMethod::lp_round},
        {.name = "relax_fix", .value = MipStartMethod::relax_fix},
        {.name = "file", .value = MipStartMethod::file},
    });

[[nodiscard]] constexpr auto enum_entries(MipStartMethod /*tag*/) noexcept
{
  return std::span {mip_start_method_entries};
}

// --- RelaxInfeasibleAction ---------------------------------------------------

/**
 * @brief What to do when the initial LP relaxation is infeasible.
 *
 * The MIP-start pipeline solves the LP relaxation first (stage A).  If that
 * relaxation is itself infeasible, the MIP cannot have a feasible solution
 * either, and this selects the response.
 *
 * - `stop` (default): report the infeasibility and abort the solve.
 * - `warn`: log a warning and proceed to the MIP solve anyway (let the
 *   solver's own infeasibility handling / soft slacks take over).
 * - `feasopt`: run the backend's infeasibility diagnostic (CPLEX FeasOpt /
 *   conflict refiner) to identify the minimal conflicting constraints, print
 *   them, then abort.
 */
enum class RelaxInfeasibleAction : uint8_t
{
  stop = 0,  ///< Report and abort (default)
  warn = 1,  ///< Warn and proceed to the MIP solve
  feasopt = 2,  ///< Diagnose the conflict, then abort
};

inline constexpr auto relax_infeasible_action_entries =
    std::to_array<EnumEntry<RelaxInfeasibleAction>>({
        {.name = "stop", .value = RelaxInfeasibleAction::stop},
        {.name = "warn", .value = RelaxInfeasibleAction::warn},
        {.name = "feasopt", .value = RelaxInfeasibleAction::feasopt},
        {.name = "diagnose",
         .value = RelaxInfeasibleAction::feasopt,
         .is_alias = true},
    });

[[nodiscard]] constexpr auto enum_entries(
    RelaxInfeasibleAction /*tag*/) noexcept
{
  return std::span {relax_infeasible_action_entries};
}

}  // namespace gtopt
