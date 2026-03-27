/**
 * @file      sparse_col.hpp
 * @brief     Sparse column representation for linear programming variables
 * @date      Thu May 15 19:15:07 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the SparseCol class which represents variables in a
 * linear program, including their bounds, objective coefficients, and
 * integrality constraints.
 */

#pragma once

#include <limits>
#include <string>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/// Maximum representable double value used for unbounded variable bounds.
constexpr double DblMax = std::numeric_limits<double>::max();

/**
 * @class SparseCol
 * @brief Represents a variable column in a linear program
 *
 * This class provides a fluent interface for setting variable properties
 * including bounds, objective coefficients, integrality constraints, and
 * a physical-to-LP scale factor.
 *
 * The @c scale field records the relationship between the LP variable and
 * the corresponding physical quantity:
 *
 *   physical_value = LP_value × scale
 *
 * For example, a voltage angle column with `scale_theta = 1000` stores
 * `scale = 1.0 / 1000 = 0.001` because `theta_physical = theta_LP / 1000`.
 * A reservoir energy column with `energy_scale = 100000` stores
 * `scale = 100000` because `volume_physical = volume_LP × 100000`.
 *
 * The scale is used by:
 * - Output rescaling: primal `x_phys = x_LP × scale`, reduced cost
 *   `rc_phys = rc_LP / scale`.
 * - User constraints (PAMPL): `LinearProblem::get_col_scale()` returns the
 *   scale so that constraint coefficients are correctly adjusted.
 * - Cross-element references (e.g. ReservoirSeepageLP reading reservoir volume
 *   scale to convert seepage slope to LP units).
 *
 * All methods are constexpr to enable compile-time construction of problems.
 */
struct SparseCol
{
  std::string name;  ///< Variable name (empty for anonymous variables)
  double lowb {0.0};  ///< Lower bound (default: 0.0)
  double uppb {DblMax};  ///< Upper bound (default: +infinity)
  double cost {0.0};  ///< Objective coefficient (default: 0.0)
  bool is_integer {false};  ///< is integer-constrained (default: false)
  double scale {1.0};  ///< Physical-to-LP scale: physical_value = LP_value ×
                       ///< scale (default: 1.0 = no scaling)

  /**
   * Sets variable to a fixed value (equality constraint)
   * @param value Fixed value to set
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& equal(double value) noexcept
  {
    lowb = value;
    uppb = value;
    return *this;
  }

  /**
   * Sets variable to be free (unbounded in both directions)
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& free() noexcept
  {
    lowb = -DblMax;
    uppb = DblMax;
    return *this;
  }

  /**
   * Marks variable as integer-constrained
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& integer() noexcept
  {
    is_integer = true;
    return *this;
  }
};

using ColIndex = StrongIndexType<SparseCol>;  ///< Type alias for column index

// ── Scale-aware output helpers ───────────────────────────────────────────────

/// Returns a lambda that rescales LP primal values to physical units.
/// physical_value = LP_value × scale.
///
/// Usage in add_to_output:
///   out.add_col_sol(cname, "theta", pid, theta_cols, col_scale_sol(scale));
[[nodiscard]] constexpr auto col_scale_sol(double scale) noexcept
{
  return [scale](double lp_value) noexcept { return lp_value * scale; };
}

/// Returns a lambda that rescales LP reduced costs to physical units.
/// rc_physical = rc_LP / scale  (inverse of primal rescaling).
///
/// Usage in add_to_output:
///   out.add_col_cost(cname, "theta", pid, theta_cols, col_scale_cost(scale));
[[nodiscard]] constexpr auto col_scale_cost(double scale) noexcept
{
  return [inv = 1.0 / scale](double lp_cost) noexcept { return lp_cost * inv; };
}

}  // namespace gtopt
