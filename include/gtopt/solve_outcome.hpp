/**
 * @file      solve_outcome.hpp
 * @brief     Tri-state outcome of a solver invocation
 * @date      2026-05-21
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Distinguishes the three result bands the `gtopt_lp_runner` layer
 * has to act on differently:
 *
 *  * `Optimal`  — solver returned an optimal solution.  Full write_out.
 *  * `Partial`  — solver did not reach optimal but the cell still holds
 *                 a usable solution (time-limit + feasible incumbent,
 *                 MIP gap reached, SDDP / cascade sentinel-stop after
 *                 iterations completed).  The runner should still
 *                 invoke `write_solution_output` so the user can
 *                 inspect / resume / use the partial result.
 *  * `Failed`   — hard failure (infeasibility, internal error, bad
 *                 input).  write_out would either throw or emit
 *                 garbage; skip it.
 *
 * The bridge from `std::expected<int, Error>` to `SolveOutcome` lives
 * here in a small pure function so unit tests can exercise the
 * classification logic without standing up a real solver.
 */
#pragma once

#include <cstdint>
#include <expected>

#include <gtopt/error.hpp>

namespace gtopt
{

enum class SolveOutcome : std::uint8_t
{
  Optimal = 0,
  Partial = 1,
  Failed = 2,
};

/// Map a solver `resolve()` return value to a `SolveOutcome`.
///
/// `ErrorCode::SolverError` is the documented "soft" error band —
/// every other code is treated as a hard failure.  The result type is
/// the same `std::expected<int, Error>` that `PlanningLP::resolve()`
/// returns; the `int` payload is the solve status (unused here).
[[nodiscard]] constexpr SolveOutcome classify_solve_outcome(
    const std::expected<int, Error>& result) noexcept
{
  if (result.has_value()) {
    return SolveOutcome::Optimal;
  }
  return result.error().code == ErrorCode::SolverError ? SolveOutcome::Partial
                                                       : SolveOutcome::Failed;
}

}  // namespace gtopt
