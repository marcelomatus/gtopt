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
