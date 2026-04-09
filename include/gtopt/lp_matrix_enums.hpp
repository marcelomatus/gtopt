/**
 * @file      lp_matrix_enums.hpp
 * @brief     Named enum types for LP matrix options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by LpMatrixOptions.  Extracted from
 * lp_matrix_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- LpNamesLevel -----------------------------------------------------------

/**
 * @brief LP variable/constraint naming level for matrix assembly.
 *
 * Controls how much naming information is generated during LP construction.
 * Higher levels provide better diagnostics but consume more memory.
 *
 * - `minimal`:      State-variable column names only (for internal use,
 *                   e.g. cascade solver state transfer).  Smallest footprint.
 * - `only_cols`:    All column names + name-to-index maps.
 * - `cols_and_rows`: Column + row names + maps.  Warns on duplicate names.
 */
enum class LpNamesLevel : int8_t
{
  none = -1,  ///< No names generated (lowest memory, default)
  minimal = 0,  ///< State-variable column names only
  only_cols = 1,  ///< All column names + name maps
  cols_and_rows = 2,  ///< Column + row names + maps + warn on duplicates
};

inline constexpr auto lp_names_level_entries =
    std::to_array<EnumEntry<LpNamesLevel>>({
        {.name = "none", .value = LpNamesLevel::none},
        {.name = "minimal", .value = LpNamesLevel::minimal},
        {.name = "only_cols", .value = LpNamesLevel::only_cols},
        {.name = "cols_and_rows", .value = LpNamesLevel::cols_and_rows},
    });

constexpr auto enum_entries(LpNamesLevel /*tag*/) noexcept
{
  return std::span {lp_names_level_entries};
}

// --- LpEquilibrationMethod --------------------------------------------------

/**
 * @brief LP matrix equilibration method for numerical conditioning.
 *
 * Controls how the constraint matrix is scaled before being sent to the
 * LP solver.  Better-conditioned matrices produce more accurate solutions
 * and reduce solver iterations.
 *
 * - `none`:    No equilibration (default).
 * - `row_max`: Per-row max-abs normalization.  Each row is divided by its
 *              largest |coefficient| so that every row's infinity-norm
 *              becomes 1.0.
 * - `ruiz`:    Ruiz geometric-mean iterative scaling.  Alternately
 *              normalizes rows and columns by sqrt(infinity-norm) until
 *              convergence.  Produces a better-conditioned matrix than
 *              single-pass row_max, especially for problems with
 *              heterogeneous variable scales.
 *              Ref: Ruiz, D. (2001) "A scaling algorithm to equilibrate
 *              both rows and columns norms in matrices".
 */
enum class LpEquilibrationMethod : uint8_t
{
  none = 0,  ///< No equilibration (default)
  row_max = 1,  ///< Per-row max-abs normalization
  ruiz = 2,  ///< Ruiz geometric-mean iterative scaling
};

inline constexpr auto lp_equilibration_method_entries =
    std::to_array<EnumEntry<LpEquilibrationMethod>>({
        {.name = "none", .value = LpEquilibrationMethod::none},
        {.name = "row_max", .value = LpEquilibrationMethod::row_max},
        {.name = "ruiz", .value = LpEquilibrationMethod::ruiz},
    });

constexpr auto enum_entries(LpEquilibrationMethod /*tag*/) noexcept
{
  return std::span {lp_equilibration_method_entries};
}

// --- FastSqrtMethod ---------------------------------------------------------

/**
 * @brief Fast approximate sqrt method for Ruiz equilibration.
 *
 * Controls which sqrt implementation is used inside the Ruiz scaling
 * loop.  Since Ruiz is iterative and self-correcting, an approximate
 * sqrt only affects convergence speed, not final accuracy.
 *
 * - `ieee_halve`:  IEEE 754 exponent halving (~2-3% accuracy, ~1 cycle).
 * - `newton1`:     ieee_halve + one Newton-Raphson step (~0.1% accuracy).
 * - `std_sqrt`:    Standard library std::sqrt (exact, ~10-20 cycles).
 */
enum class FastSqrtMethod : uint8_t
{
  ieee_halve = 0,  ///< IEEE 754 exponent halving (default)
  newton1 = 1,  ///< ieee_halve + one Newton-Raphson refinement
  std_sqrt = 2,  ///< Standard library std::sqrt
};

inline constexpr auto fast_sqrt_method_entries =
    std::to_array<EnumEntry<FastSqrtMethod>>({
        {.name = "ieee_halve", .value = FastSqrtMethod::ieee_halve},
        {.name = "newton1", .value = FastSqrtMethod::newton1},
        {.name = "std_sqrt", .value = FastSqrtMethod::std_sqrt},
    });

constexpr auto enum_entries(FastSqrtMethod /*tag*/) noexcept
{
  return std::span {fast_sqrt_method_entries};
}

}  // namespace gtopt
