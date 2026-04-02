/**
 * @file      element_column_resolver.hpp
 * @brief     Resolve LP column indices for user-constraint element references
 * @date      Mon Mar 24 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Provides functions that map symbolic element references (from parsed
 * user-constraint expressions) to concrete LP column indices.  These
 * are used by `UserConstraintLP` to build constraint rows.
 */

#pragma once

#include <optional>

#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

class SystemContext;
class ScenarioLP;
class StageLP;
class BlockLP;
struct ElementRef;
struct SumElementRef;

/// A resolved LP column together with its physical-to-LP scale factor.
///
/// The @c scale field satisfies:  physical_value = LP_value * scale.
///
/// When assembling a user constraint  `coeff * physical_var [op] RHS`  the
/// LP-level coefficient is  `coeff * scale`  so that the constraint remains
/// dimensionally correct regardless of internal LP scaling choices (e.g.
/// reservoir volume in Gm3, theta in milli-radians, ...).
struct ResolvedCol
{
  ColIndex col;
  double scale {1.0};
};

/**
 * @brief Try to look up the LP `ColIndex` for one element reference.
 *
 * Returns `std::nullopt` when the element is not found, the block is not
 * active in the requested (scenario, stage), or the attribute is unknown.
 *
 * The returned @c ResolvedCol::scale converts the LP variable to physical
 * units so that the caller can build correctly-scaled constraint rows.
 *
 * When a column has a non-unit scale stored in SparseCol::scale (set at
 * variable creation time), the scale is retrieved via
 * `lp.get_col_scale(col)` -- providing a uniform mechanism that works for
 * all current and future scaled variables without hardcoding per-element
 * logic.
 */
[[nodiscard]] std::optional<ResolvedCol> resolve_single_col(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref,
    const LinearProblem& lp);

/**
 * @brief Try to look up a data parameter value for one element reference.
 *
 * Returns `std::nullopt` when the element is not found or the attribute is
 * not a known parameter.  Parameters are fixed data values (not LP columns)
 * such as `pmax`, `gcost`, `fmax`, `emin`, etc.
 *
 * For schedule-valued parameters the value is resolved for the given
 * (scenario, stage, block) context.  If the schedule has no value for the
 * given context, `std::nullopt` is returned.
 */
[[nodiscard]] std::optional<double> resolve_single_param(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref);

/**
 * @brief Collect (coefficient, ColIndex) pairs for a `SumElementRef`.
 *
 * When `sum_ref.all_elements` is true, iterates over every element in the
 * collection of the matching type.  Otherwise iterates over the explicit ID
 * list.  The base_coeff is multiplied into each term's coefficient.
 */
void collect_sum_cols(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      const BlockLP& block,
                      const SumElementRef& sum_ref,
                      double base_coeff,
                      SparseRow& row,
                      const LinearProblem& lp);

}  // namespace gtopt
