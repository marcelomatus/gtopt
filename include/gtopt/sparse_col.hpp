#pragma once

#include <limits>
#include <string>

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
