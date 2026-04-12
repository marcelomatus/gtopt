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
#include <unordered_map>
#include <vector>

#include <gtopt/label_maker.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/variable_scale.hpp>

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
  using index_t = int32_t;  ///< Type for indices (row/column indices)
  using name_vec_t = std::vector<std::string>;
  using index_map_t = std::unordered_map<std::string_view, index_t>;

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
  std::vector<double> col_scales;  ///< Per-column physical-to-LP scale factors
                                   ///< (physical = LP × scale; default 1.0)
  std::vector<double> row_scales;  ///< Per-row equilibration scale factors
                                   ///< (max |coeff| per row).
                                   ///< dual_physical = dual_LP / row_scale.
                                   ///< Empty when row equilibration is off.
  double scale_objective {1.0};  ///< Global objective divisor applied during
                                 ///< flatten().  obj_physical = obj_LP ×
                                 ///< scale_objective.

  name_vec_t colnm;  ///< Variable names (dense; populated when names enabled)
  name_vec_t rownm;  ///< Constraint names (dense; populated when names enabled)
  index_map_t colmp;  ///< Map from variable names to indices
  index_map_t rowmp;  ///< Map from constraint names to indices

  std::string name;  ///< Problem name

  /// @name Coefficient statistics (populated when
  /// LpMatrixOptions::compute_stats)
  /// @{
  size_t stats_nnz {};  ///< Non-zero count for the constraint matrix A
  size_t stats_zeroed {};  ///< Count of non-zero entries filtered out by eps
  double stats_max_abs {};  ///< Largest  |coefficient| in constraint matrix A
  double stats_min_abs {
      ///< Smallest |coefficient| in filtered A
      std::numeric_limits<double>::max(),
  };

  index_t stats_max_col {-1};  ///< Column index of the largest |coefficient|
  index_t stats_min_col {-1};  ///< Column index of the smallest |coefficient|

  std::string stats_max_col_name {};  ///< Name of column with largest |coeff|
  std::string stats_min_col_name {};  ///< Name of column with smallest |coeff|

  /// Per-row-type coefficient statistics (populated when both compute_stats
  /// and row_with_names are enabled).
  struct RowTypeStatsEntry
  {
    std::string type {};
    size_t count {};
    size_t nnz {};
    double max_abs {};
    double min_abs {std::numeric_limits<double>::max()};
  };
  std::vector<RowTypeStatsEntry> row_type_stats {};

  /// Coefficient ratio max/min (1.0 when empty, no valid min, or all equal).
  [[nodiscard]] constexpr double stats_coeff_ratio() const noexcept
  {
    if (stats_min_abs <= 0.0 || stats_min_col < 0
        || stats_min_abs >= std::numeric_limits<double>::max()
        || stats_min_abs == stats_max_abs || stats_nnz == 0)
    {
      return 1.0;
    }
    return stats_max_abs / stats_min_abs;
  }
  /// @}

  /// VariableScaleMap copied from LinearProblem during flatten().
  /// LinearInterface picks this up in load_flat() so dynamically
  /// added columns can be auto-scaled.
  VariableScaleMap variable_scale_map {};

  /// LabelMaker copied from LinearProblem during flatten().
  /// LinearInterface picks this up in load_flat() so dynamically
  /// added columns and rows generate labels consistent with the
  /// original names_level configuration.
  LabelMaker label_maker {};
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
  // Constants
  static constexpr double DblMax = gtopt::DblMax;

  // Type aliases for indices and sparse structures
  using index_t = FlatLinearProblem::index_t;
  using SparseVector = flat_map<ColIndex, double>;
  using SparseMatrix = std::vector<SparseVector>;
  using cols_t = std::vector<SparseCol>;
  using rows_t = std::vector<SparseRow>;

  /**
   * Constructs a new linear problem
   * @param name Problem name
   */
  [[nodiscard]]
  constexpr explicit LinearProblem(std::string name = {}) noexcept
      : pname(std::move(name))
  {
  }

  /**
   * Sets the infinity value used for bound normalization.
   *
   * When set to a value smaller than DblMax (e.g. from the solver backend's
   * infinity()), add_col()/add_row() will clamp DblMax bounds to ±infinity.
   * This ensures that flattened LP vectors never contain raw DblMax values,
   * avoiding noisy solver warnings (e.g. HiGHS "bounds >= 1e20 treated as
   * +Infinity").
   *
   * @param inf Target infinity value (e.g. LinearInterface::infinity())
   */
  constexpr void set_infinity(double inf) noexcept { m_infinity_ = inf; }

  /// Current infinity value (DblMax if not explicitly set).
  [[nodiscard]] constexpr double infinity() const noexcept
  {
    return m_infinity_;
  }

  /// Set the VariableScaleMap used for automatic scale resolution in add_col.
  void set_variable_scale_map(VariableScaleMap map) { m_vsm_ = std::move(map); }

  /// Get the VariableScaleMap (empty if not set).
  [[nodiscard]] const VariableScaleMap& variable_scale_map() const noexcept
  {
    return m_vsm_;
  }

  /// Set the LabelMaker used to generate column/row labels during flatten().
  /// The LabelMaker is copied into FlatLinearProblem so LinearInterface can
  /// continue generating labels for dynamically added columns/rows.
  void set_label_maker(LabelMaker lm) noexcept { m_label_maker_ = lm; }

  /// Get the LabelMaker (default-constructed = names off if not set).
  [[nodiscard]] const LabelMaker& label_maker() const noexcept
  {
    return m_label_maker_;
  }

  /**
   * Pre-reserves capacity for columns, rows, and coefficients.
   * Call before the build loop to avoid repeated reallocations.
   * @param est_cols Estimated number of columns (variables)
   * @param est_rows Estimated number of rows (constraints)
   */
  constexpr void reserve(size_t est_cols, size_t est_rows)
  {
    cols.reserve(est_cols);
    rows.reserve(est_rows);
  }

  /**
   * Adds a new variable to the problem
   * @param col Column (variable) definition
   * @return Index of the added column
   */
  template<typename SparseCol = gtopt::SparseCol>
  [[nodiscard]]
  constexpr ColIndex add_col(SparseCol&& col)
  {
    const auto index = ColIndex {static_cast<Index>(cols.size())};

    if (col.is_integer) {
      ++colints;
    }

    // Normalize DblMax bounds to the configured infinity.
    normalize_bound(col.lowb);
    normalize_bound(col.uppb);

    // Auto-resolve scale from VariableScaleMap when the caller provided
    // class_name metadata.  The map entry overrides any pre-computed scale
    // (including auto_scale) — only per-element fields that avoid setting
    // class_name metadata are immune.  When the map has no entry for this
    // (class, variable, uid), the pre-set scale (or default 1.0) is kept.
    if (!col.class_name.empty() && !m_vsm_.empty()) {
      const auto resolved =
          m_vsm_.lookup(col.class_name, col.variable_name, col.variable_uid);
      if (resolved != 1.0) {
        col.scale = resolved;
      }
    }

    cols.emplace_back(std::forward<SparseCol>(col));
    return index;
  }

  /**
   * Adds a new constraint to the problem
   * @param row Row (constraint) definition
   * @return Index of the added row
   */
  template<typename SparseRow = gtopt::SparseRow>
  [[nodiscard]]
  constexpr RowIndex add_row(SparseRow&& row)
  {
    const auto index = RowIndex {static_cast<Index>(rows.size())};

    // Normalize DblMax bounds to the configured infinity.
    normalize_bound(row.lowb);
    normalize_bound(row.uppb);

    ncoeffs += row.size();
    rows.emplace_back(std::forward<SparseRow>(row));
    return index;
  }

  /**
   * Gets a reference to a column by index
   * @param self Deduced object reference
   * @param index Column index
   * @return Reference to the column
   */
  template<typename Self>
  [[nodiscard]]
  constexpr auto&& col_at(this Self&& self, ColIndex index)
  {
    return std::forward<Self>(self).cols.at(index);
  }

  /**
   * Gets the lower bound of a column
   * @param index Column index
   * @return Lower bound value
   */
  [[nodiscard]] constexpr auto get_col_lowb(ColIndex index) const
  {
    return cols.at(index).lowb;
  }

  /**
   * Gets the upper bound of a column
   * @param index Column index
   * @return Upper bound value
   */
  [[nodiscard]] constexpr auto get_col_uppb(ColIndex index) const
  {
    return cols.at(index).uppb;
  }

  /**
   * Gets the physical-to-LP scale factor of a column.
   * physical_value = LP_value × scale.
   * @param index Column index
   * @return Scale factor (1.0 = no scaling)
   */
  [[nodiscard]] constexpr auto get_col_scale(ColIndex index) const
  {
    return cols.at(index).scale;
  }

  /**
   * Gets a reference to a row by index
   * @param self Deduced object reference
   * @param index Row index
   * @return Reference to the row
   */
  template<typename Self>
  [[nodiscard]]
  constexpr auto&& row_at(this Self&& self, RowIndex index)
  {
    return std::forward<Self>(self).rows.at(index);
  }

  /**
   * @return Number of rows (constraints) in the problem
   */
  [[nodiscard]] constexpr index_t get_numrows() const
  {
    return static_cast<index_t>(rows.size());
  }

  /**
   * @return Number of columns (variables) in the problem
   */
  [[nodiscard]] constexpr index_t get_numcols() const
  {
    return static_cast<index_t>(cols.size());
  }

  /**
   * Sets a coefficient in the constraint matrix
   * @param row Row index
   * @param col Column index
   * @param coeff Coefficient value
   */
  constexpr void set_coeff(RowIndex row, ColIndex col, double coeff)
  {
    auto& row_cmap = rows[row].cmap;
    auto it = row_cmap.find(col);
    if (it != row_cmap.end()) {
      it->second = coeff;
    } else {
      row_cmap.emplace(col, coeff);
      ++ncoeffs;
    }
  }

  /**
   * Gets a coefficient from the constraint matrix
   * @param row Row index
   * @param col Column index
   * @return Coefficient value
   */
  [[nodiscard]] constexpr double get_coeff(RowIndex row, ColIndex col) const
  {
    return rows[row][col];
  }

  /// Read-only access to the raw column vector (for fingerprinting).
  [[nodiscard]] constexpr const cols_t& get_cols() const noexcept
  {
    return cols;
  }

  /// Read-only access to the raw row vector (for fingerprinting).
  [[nodiscard]] constexpr const rows_t& get_rows() const noexcept
  {
    return rows;
  }

  /**
   * Builds the flat (column-major) LP representation
   * @param opts LP build options
   * @return Flat representation of the problem
   */
  [[nodiscard]] FlatLinearProblem flatten(const LpMatrixOptions& opts = {});

private:
  /// Clamp a bound in-place: DblMax → +infinity, -DblMax → -infinity.
  constexpr void normalize_bound(double& value) const noexcept
  {
    if (value >= DblMax) {
      value = m_infinity_;
    } else if (value <= -DblMax) {
      value = -m_infinity_;
    }
  }

  std::string pname;  ///< Problem name
  cols_t cols;  ///< Variables (columns)
  rows_t rows;  ///< Constraints (rows)
  size_t ncoeffs {};  ///< Total number of coefficients
  size_t colints {};  ///< Number of integer variables
  double m_infinity_ {DblMax};  ///< Target infinity for bound normalization
  VariableScaleMap m_vsm_ {};  ///< Auto-scale map (owned copy)
  LabelMaker m_label_maker_ {};  ///< Label generator (default = names off)
};

}  // namespace gtopt
