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

/**
 * Maximum representable double value used for unbounded constraints
 */
constexpr double const CoinDblMax = std::numeric_limits<double>::max();

/**
 * @class SparseCol
 * @brief Represents a variable column in a linear program
 *
 * This class provides a fluent interface for setting variable properties
 * including bounds, objective coefficients, and integrality constraints.
 * All methods are constexpr to enable compile-time construction of problems.
 */
struct SparseCol
{
  std::string name;  ///< Variable name (empty for anonymous variables)
  double lowb {0.0};  ///< Lower bound (default: 0.0)
  double uppb {CoinDblMax};  ///< Upper bound (default: +infinity)
  double cost {0.0};  ///< Objective coefficient (default: 0.0)
  bool is_integer {
      false};  ///< Whether variable is integer-constrained (default: false)

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
    lowb = -CoinDblMax;
    uppb = CoinDblMax;
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

}  // namespace gtopt
