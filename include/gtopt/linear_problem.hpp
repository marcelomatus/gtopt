/**
 * @file      linear_problem.hpp
 * @brief     Header defining data structures for sparse linear planning
 * problems
 * @date      Sun Mar 23 14:50:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides representations for sparse linear planning problems,
 * including data structures for rows, columns, and matrices, as well as
 * conversion utilities for different problem formats.
 */

#pragma once

#include <limits>
#include <string>
#include <vector>

#include <gtopt/fmap.hpp>

namespace gtopt
{

/**
 * Maximum representable double value used for unbounded constraints
 */
constexpr double const CoinDblMax = std::numeric_limits<double>::max();

/**
 * Sparse vector representation using flat_map to store index-value pairs
 */
using SparseVector = gtopt::flat_map<size_t, double>;

/**
 * Sparse matrix representation as a vector of sparse vectors
 */
using SparseMatrix = std::vector<SparseVector>;

/**
 * @class SparseRow
 * @brief Represents a constraint row in a linear program with sparse
 * coefficients
 *
 * This class stores the constraint name, bounds, and non-zero coefficients.
 */
struct SparseRow
{
  using cmap_t = gtopt::flat_map<size_t, double>;

  std::string name;  ///< Row/constraint name
  double lowb {0};  ///< Lower bound of the constraint
  double uppb {0};  ///< Upper bound of the constraint
  cmap_t cmap {};  ///< Sparse coefficient map (column index → coeff value)

  /**
   * Sets both lower and upper bounds for the constraint
   * @param lb Lower bound
   * @param ub Upper bound
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& bound(const double lb, const double ub)
  {
    lowb = lb;
    uppb = ub;
    return *this;
  }

  /**
   * Sets the constraint as a less-than-or-equal inequality (x ≤ ub)
   * @param ub Upper bound
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& less_equal(const double ub)
  {
    return bound(-CoinDblMax, ub);
  }

  /**
   * Sets the constraint as a greater-than-or-equal inequality (x ≥ lb)
   * @param lb Lower bound
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& greater_equal(const double lb)
  {
    return bound(lb, CoinDblMax);
  }

  /**
   * Sets the constraint as an equality (x = rhs)
   * @param rhs Right-hand side value (default 0)
   * @return Reference to this row for method chaining
   */
  constexpr SparseRow& equal(const double rhs = 0) { return bound(rhs, rhs); }

  /**
   * Gets a coefficient for a given column index
   * @param key Column index
   * @return Coefficient value, or 0.0 if not present
   */
  [[nodiscard]] auto get_coeff(const size_t key) const
  {
    auto&& iter = cmap.find(key);
    return (iter != cmap.end()) ? iter->second : 0.0;
  }

  /**
   * Sets a coefficient for a given column index
   * @param c Column index
   * @param e Coefficient value
   * @return Reference to the set coefficient
   */
  auto& set_coeff(const size_t c, const double e) { return cmap[c] = e; }

  /**
   * Accessor for coefficients (mutable)
   */
  auto& operator[](const size_t key) { return cmap[key]; }

  /**
   * Accessor for coefficients (const)
   */
  auto operator[](const size_t key) const { return get_coeff(key); }

  /**
   * @return Number of non-zero coefficients in this row
   */
  [[nodiscard]] auto size() const { return cmap.size(); }

  /**
   * Reserve space for coefficients
   * @param n Number of coefficients to reserve space for
   */
  void reserve(const size_t n) { cmap.reserve(n); }

  /**
   * Converts the sparse representation to flat vectors of keys and values
   * @tparam Int Index type for keys
   * @tparam Dbl Value type for coefficients
   * @tparam KVec Vector type for keys
   * @tparam VVec Vector type for values
   * @param eps Tolerance for considering a value as non-zero
   * @return Pair of vectors (keys, values) containing the non-zero coefficients
   */
  template<typename Int = size_t,
           typename Dbl = double,
           typename KVec = std::vector<Int>,
           typename VVec = std::vector<Dbl>>
  auto to_flat(const double eps = 0.0) const -> std::pair<KVec, VVec>
  {
    using key_t = typename KVec::value_type;
    using value_t = typename VVec::value_type;

    const auto msize = cmap.size();
    KVec keys;
    keys.reserve(msize);
    VVec vals;
    vals.reserve(msize);

    for (auto&& [key, value] : cmap) {
      if (std::abs(value) > eps) {
        keys.push_back(static_cast<key_t>(key));
        vals.push_back(static_cast<value_t>(value));
      }
    }

    return {std::move(keys), std::move(vals)};
  }
};

/**
 * @class SparseCol
 * @brief Represents a variable column in a linear program
 *
 * This class stores variable information including bounds, objective
 * coefficient, and variable type (continuous or integer).
 */
struct SparseCol
{
  std::string name;  ///< Variable name
  double lowb {0};  ///< Lower bound (default: 0)
  double uppb {CoinDblMax};  ///< Upper bound (default: +infinity)
  double cost {0};  ///< Objective coefficient
  bool is_integer {false};  ///< Whether the variable is integer-constrained

  /**
   * Sets the variable to be fixed at a specific value
   * @param value The fixed value
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& equal(const double value)
  {
    lowb = value;
    uppb = value;
    return *this;
  }

  /**
   * Sets the variable to be unbounded (free)
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& free()
  {
    lowb = -CoinDblMax;
    uppb = CoinDblMax;
    return *this;
  }

  /**
   * Sets the variable to be integer-constrained
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& integer()
  {
    is_integer = true;
    return *this;
  }
};

/**
 * @struct FlatLinearProblem
 * @brief Represents a linear problem in column-major flat representation
 *
 * This format is commonly used by solver interfaces like COIN-OR, CPLEX, etc.
 */
struct FlatLinearProblem
{
  using index_t = int;
  using name_vec_t = std::vector<std::string>;
  using index_map_t = gtopt::flat_map<std::string_view, index_t>;

  index_t ncols {};  ///< Number of columns (variables)
  index_t nrows {};  ///< Number of rows (constraints)

  std::vector<index_t> matbeg;  ///< Column start indices in the sparse matrix
  std::vector<index_t> matind;  ///< Row indices for non-zero coefficients
  std::vector<double> matval;  ///< Values of non-zero coefficients
  std::vector<double> collb;  ///< Lower bounds for variables
  std::vector<double> colub;  ///< Upper bounds for variables
  std::vector<double> objval;  ///< Objective coefficients
  std::vector<double> rowlb;  ///< Lower bounds for constraints
  std::vector<double> rowub;  ///< Upper bounds for constraints
  std::vector<index_t> colint;  ///< Indices of integer variables

  name_vec_t colnm;  ///< Variable names
  name_vec_t rownm;  ///< Constraint names
  index_map_t colmp;  ///< Map from variable names to indices
  index_map_t rowmp;  ///< Map from constraint names to indices

  std::string name;  ///< Problem name
};

/**
 * @struct FlatOptions
 * @brief Configuration options for converting to flat representation
 */
struct FlatOptions
{
  constexpr static auto default_reserve_factor = 1.25;

  double eps {0};  ///< Coefficient epsilon (if negative, don't check)
  bool col_with_names {false};  ///< Include column names
  bool row_with_names {false};  ///< Include row names
  bool col_with_name_map {false};  ///< Include column name mapping
  bool row_with_name_map {false};  ///< Include row name mapping
  bool move_names {true};  ///< Move instead of copy names
  bool reserve_matrix {true};  ///< Pre-reserve matrix memory
  double reserve_factor {
      default_reserve_factor};  ///< Memory reservation factor
};

/**
 * @class LinearProblem
 * @brief Main class for building and manipulating linear planning problems
 *
 * This class provides functionality to construct a linear problem by adding
 * variables (columns) and constraints (rows), setting coefficients, and
 * converting to solver-ready formats.
 */
class LinearProblem
{
public:
  using index_t = size_t;

  constexpr static auto default_reserve_size = 1024;

  /**
   * Constructs a new linear problem
   * @param name Problem name
   * @param rsize Initial reserve size for rows/columns
   */
  [[nodiscard]] explicit LinearProblem(std::string name = {},
                                       size_t rsize = default_reserve_size);

  /**
   * Adds a new variable to the problem
   * @param col Column (variable) definition
   * @return Index of the added column
   */
  index_t add_col(SparseCol&& col)
  {
    const index_t index = cols.size();

    if (col.is_integer) {
      ++colints;
    }

    cols.emplace_back(std::move(col));
    return index;
  }

  /**
   * Adds a new constraint to the problem
   * @param row Row (constraint) definition
   * @return Index of the added row
   */
  index_t add_row(SparseRow&& row)
  {
    const index_t index = rows.size();

    ncoeffs += row.size();
    rows.emplace_back(std::move(row));
    return index;
  }

  /**
   * Gets a reference to a column by index
   * @param index Column index
   * @return Reference to the column
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& col_at(this Self&& self, index_t index)
  {
    return std::forward<Self>(self).cols.at(index);
  }

  /**
   * Gets the lower bound of a column
   * @param index Column index
   * @return Lower bound value
   */
  [[nodiscard]] auto get_col_lowb(index_t index) const
  {
    return cols.at(index).lowb;
  }

  /**
   * Gets the upper bound of a column
   * @param index Column index
   * @return Upper bound value
   */
  [[nodiscard]] auto get_col_uppb(index_t index) const
  {
    return cols.at(index).uppb;
  }

  /**
   * Gets a reference to a row by index
   * @param index Row index
   * @return Reference to the row
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& row_at(this Self&& self, index_t index)
  {
    return std::forward<Self>(self).rows.at(index);
  }

  /**
   * @return Number of rows (constraints) in the problem
   */
  [[nodiscard]] index_t get_numrows() const { return rows.size(); }

  /**
   * @return Number of columns (variables) in the problem
   */
  [[nodiscard]] index_t get_numcols() const { return cols.size(); }

  /**
   * Sets a coefficient in the constraint matrix
   * @param row Row index
   * @param col Column index
   * @param coeff Coefficient value
   * @param eps Epsilon value for zero comparison
   */
  void set_coeff(index_t row, index_t col, double coeff, double eps = 0.0);

  /**
   * Gets a coefficient from the constraint matrix
   * @param row Row index
   * @param col Column index
   * @return Coefficient value
   */
  [[nodiscard]] double get_coeff(index_t row, index_t col) const;

  /**
   * Converts the problem to a flat representation
   * @param opts Conversion options
   * @return Flat representation of the problem
   */
  [[nodiscard]] FlatLinearProblem to_flat(const FlatOptions& opts = {});

private:
  using cols_t = std::vector<SparseCol>;
  using rows_t = std::vector<SparseRow>;

  std::string pname;  ///< Problem name
  cols_t cols;  ///< Variables (columns)
  rows_t rows;  ///< Constraints (rows)
  size_t ncoeffs {};  ///< Total number of non-zero coefficients
  size_t colints {};  ///< Number of integer variables
};

}  // namespace gtopt
