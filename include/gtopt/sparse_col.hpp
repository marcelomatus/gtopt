/**
 * @file      sparse_col.hpp
 * @brief     Header of
 * @date      Thu May 15 19:15:07 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/sparse_row.hpp>

namespace gtopt
{

/**
 * @class SparseCol
 * @brief Represents a variable column in a linear program
 */
struct SparseCol
{
  std::string name;  ///< Variable name
  double lowb {0};  ///< Lower bound (default: 0)
  double uppb {CoinDblMax};  ///< Upper bound (default: +infinity)
  double cost {0};  ///< Objective coefficient
  bool is_integer {false};  ///< Whether the variable is integer-constrained

  constexpr SparseCol& equal(const double value)
  {
    lowb = value;
    uppb = value;
    return *this;
  }

  constexpr SparseCol& free()
  {
    lowb = -CoinDblMax;
    uppb = CoinDblMax;
    return *this;
  }

  constexpr SparseCol& integer()
  {
    is_integer = true;
    return *this;
  }
};

}  // namespace gtopt
