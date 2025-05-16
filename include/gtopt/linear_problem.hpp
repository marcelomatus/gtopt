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

#include <string>
#include <vector>

#include <gtopt/fmap.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

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
  using index_map_t = flat_map<std::string_view, index_t>;

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
  using index_t = FlatLinearProblem::index_t;
  using SparseVector = flat_map<index_t, double>;
  using SparseMatrix = std::vector<SparseVector>;

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
  index_t add_col(SparseCol&& col) noexcept
  {
    const auto index = static_cast<index_t>(cols.size());

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
  index_t add_row(SparseRow&& row) noexcept
  {
    const auto index = static_cast<index_t>(rows.size());

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
  [[nodiscard]] index_t get_numrows() const
  {
    return static_cast<index_t>(rows.size());
  }

  /**
   * @return Number of columns (variables) in the problem
   */
  [[nodiscard]] index_t get_numcols() const
  {
    return static_cast<index_t>(cols.size());
  }

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
