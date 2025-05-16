/**
 * @file      sparse_row.hpp
 * @brief     Sparse row representation for linear programming constraints
 * @date      Thu May 15 19:28:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the SparseRow class which represents constraints in a
 * linear program, including their bounds and sparse coefficient storage.
 * The implementation uses efficient flat_map storage and supports compile-time
 * construction of problems through constexpr methods.
 *
 * Key features:
 * - Sparse storage of constraint coefficients
 * - Fluent interface for constraint bounds
 * - Conversion to flat formats for solver interfaces
 * - Compile-time evaluation support
 */

#pragma once

#include <cmath>  // for std::abs
#include <limits>
#include <string>
#include <utility>  // for std::pair
#include <vector>

#include <gtopt/fmap.hpp>

namespace gtopt
{

/**
 * @class SparseRow
 * @brief Represents a constraint row in a linear program with sparse
 * coefficients
 *
 * This class provides a sparse representation of a linear constraint row,
 * storing only non-zero coefficients. It supports various constraint types
 * through fluent interface methods and can convert to dense or flat
 * representations.
 *x
 * @note All methods are constexpr where possible to enable compile-time
 * evaluation
 */
struct SparseRow
{
  static constexpr double const CoinDblMax = std::numeric_limits<double>::max();

  using cmap_t = flat_map<size_t, double>;  ///< Type for coefficient storage
  using size_type = typename cmap_t::size_type;

  std::string name;  ///< Row/constraint name (empty for anonymous constraints)
  double lowb {0};  ///< Lower bound (default: 0)
  double uppb {0};  ///< Upper bound (default: 0)
  cmap_t cmap {};  ///< Sparse coefficient map

  /**
   * Sets both lower and upper bounds for the constraint
   * @param lb Lower bound value
   * @param ub Upper bound value
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& bound(double lb, double ub) noexcept
  {
    lowb = lb;
    uppb = ub;
    return *this;
  }

  /**
   * Sets an upper bound (<= constraint)
   * @param ub Upper bound value
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& less_equal(double ub) noexcept
  {
    return bound(-CoinDblMax, ub);
  }

  /**
   * Sets a lower bound (>= constraint)
   * @param lb Lower bound value
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& greater_equal(double lb) noexcept
  {
    return bound(lb, CoinDblMax);
  }

  /**
   * Sets an equality constraint (=)
   * @param rhs Right-hand side value (default: 0)
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& equal(double rhs = 0) noexcept
  {
    return bound(rhs, rhs);
  }

  /**
   * Gets a coefficient value
   * @param key Column index
   * @return Coefficient value (0 if not found)
   */
  [[nodiscard]] constexpr double get_coeff(size_t key) const noexcept
  {
    const auto iter = cmap.find(key);
    return (iter != cmap.end()) ? iter->second : 0.0;
  }

  /**
   * Sets a coefficient value
   * @param c Column index
   * @param e Coefficient value
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& set_coeff(size_t c, double e) noexcept
  {
    cmap[c] = e;
    return *this;
  }

  /**
   * Gets or sets a coefficient (non-const version)
   * @param key Column index
   * @return Reference to coefficient value
   */
  constexpr double& operator[](size_t key) noexcept { return cmap[key]; }

  /**
   * Gets a coefficient (const version)
   * @param key Column index
   * @return Coefficient value (0 if not found)
   */
  [[nodiscard]] constexpr double operator[](size_t key) const noexcept
  {
    return get_coeff(key);
  }

  /**
   * Gets the number of non-zero coefficients
   * @return Number of non-zero coefficients
   */
  [[nodiscard]] constexpr size_type size() const noexcept
  {
    return cmap.size();
  }

  /**
   * Reserves space for coefficients
   * @param n Number of coefficients to reserve space for
   */
  constexpr void reserve(size_type n) noexcept { cmap.reserve(n); }

  /**
   * Converts to flat representation for solver interfaces
   * @tparam Int Index type (default: size_t)
   * @tparam Dbl Value type (default: double)
   * @tparam KVec Key vector type (default: std::vector<Int>)
   * @tparam VVec Value vector type (default: std::vector<Dbl>)
   * @param eps Epsilon for zero comparison (values < eps are treated as 0)
   * @return Pair of vectors containing indices and values
   */
  template<typename Int = size_t,
           typename Dbl = double,
           typename KVec = std::vector<Int>,
           typename VVec = std::vector<Dbl>>
  [[nodiscard]] auto to_flat(double eps = 0.0) const -> std::pair<KVec, VVec>
  {
    using key_t = typename KVec::value_type;
    using value_t = typename VVec::value_type;

    const auto msize = cmap.size();
    KVec keys;
    keys.reserve(msize);
    VVec vals;
    vals.reserve(msize);

    for (const auto& [key, value] : cmap) {
      if (std::abs(value) >= eps) {
        keys.push_back(static_cast<key_t>(key));
        vals.push_back(static_cast<value_t>(value));
      }
    }

    return {std::move(keys), std::move(vals)};
  }
};

}  // namespace gtopt
