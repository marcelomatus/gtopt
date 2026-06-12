/**
 * @file      lp_equilibration.hpp
 * @brief     Per-row equilibration primitives shared by the bulk LP build
 *            path and the post-build (cut) row-insertion path.
 * @date      2026-04-20
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The bulk `apply_row_max_equilibration` / `apply_ruiz_scaling` routines
 * in `linear_problem.cpp` scale the whole CSC matrix at once during
 * `LinearProblem::flatten()`.  After the LP has been built, any row
 * added via `LinearInterface::add_row` (notably every SDDP Benders
 * optimality cut) enters the matrix at its raw magnitude — bypassing the
 * equilibration that normalised the structural rows.  Over a hundred
 * iterations this is a large source of kappa drift.
 *
 * This header factors out the per-row math so the add_row path can
 * apply *exactly the same* row_max or Ruiz-style scaling to each new
 * cut as the original equilibration pass did to structural rows.  The
 * bulk routines are refactored internally to call these primitives so
 * there is a single source of truth for the scaling math.
 */

#pragma once

#include <span>

namespace gtopt
{

/// Normalise a single row in place so `max|coeff| == 1`.
///
/// Mirrors the per-row loop body of `apply_row_max_equilibration` in
/// linear_problem.cpp, intended for both bulk and one-row callers.
/// Safe on empty rows and all-zero rows (no-op, returns 1.0).
///
/// @param elements Coefficients of the row (mutated in place).
/// @param rowlb    Lower bound (mutated in place unless ±infinity).
/// @param rowub    Upper bound (mutated in place unless ±infinity).
/// @param infinity Solver infinity sentinel — bounds matching ±infinity
///                 are left untouched.
/// @return The divisor applied: `max|coeff|` before normalisation, or
///         1.0 for empty/zero rows.  Callers compose this with any
///         pre-existing row scale and store it via
///         `LinearInterface::set_row_scale` so dual recovery
///         (`dual_physical = dual_LP × row_scale`) stays correct.
[[nodiscard]] double equilibrate_row_in_place(std::span<double> elements,
                                              double& rowlb,
                                              double& rowub,
                                              double infinity) noexcept;

/// One-shot Ruiz-style scaling for a single row against an already-
/// converged column-scale profile.
///
/// Applies `elements[k] *= col_scales[columns[k]]` first (folding the
/// equilibrated column factors into the new row) and then calls
/// `equilibrate_row_in_place` for the row-max normalisation.  Intended
/// for user-space cuts that arrive in physical units (e.g. boundary
/// cuts loaded from CSV).  The accumulated Ruiz `col_scales` are frozen
/// after the initial build — we do not re-iterate, just bring the new
/// row onto the same footing.
///
/// @param columns   Column indices for the row's non-zeros.
/// @param elements  Coefficients; mutated in place.
/// @param col_scales  Per-column scale vector from the initial build
///                  (`LinearInterface::get_col_scales()`).  Indices
///                  outside its range are treated as scale 1.0.
/// @param rowlb     Mutated in place unless ±infinity.
/// @param rowub     Mutated in place unless ±infinity.
/// @param infinity  Solver infinity sentinel.
/// @return Combined divisor: the row-max after column scaling.
[[nodiscard]] double equilibrate_row_ruiz_in_place(
    std::span<const int> columns,
    std::span<double> elements,
    std::span<const double> col_scales,
    double& rowlb,
    double& rowub,
    double infinity) noexcept;

}  // namespace gtopt
