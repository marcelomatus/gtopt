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

#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <gtopt/label_maker.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/variable_scale.hpp>

namespace gtopt
{

/**
 * @struct FlatLpMeta
 * @brief Scene-invariant label metadata, bundled behind a `shared_ptr`.
 *
 * `col_labels_meta` / `row_labels_meta` carry the structural
 * `(class_name, variable/constraint_name, uid, context)` tuples that
 * `LabelMaker` needs to synthesise human-readable labels on demand.  They
 * depend only on the LP *structure* (which columns/rows exist), never on the
 * per-scenario numeric values, so all `(scene)` cells of a phase with an
 * identical structural fingerprint can share **one** copy.  In compress mode
 * these are the dominant resident cost (the name strings), preserved
 * uncompressed by `compress_flat_lp`; sharing them across a phase's scenes is
 * the point of `PlanningLP`'s cross-scene dedup.  Held via
 * `shared_ptr<const FlatLpMeta>` so the bundle is immutable once built.
 */
struct FlatLpMeta
{
  std::vector<SparseColLabel> col_labels_meta;
  std::vector<SparseRowLabel> row_labels_meta;
};

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
  /// Type-2 Special-Ordered-Sets (SOS2) collected during LP build via
  /// ``LinearProblem::add_sos2``.  Each inner vector lists the column
  /// indices participating in one SOS2 in geometric order; weights are
  /// not stored explicitly (backends default to ``1, 2, …, N``).
  /// Consumed by ``LinearInterface::load_flat`` which forwards each
  /// inner vector to ``SolverBackend::add_sos2``.  Empty for the vast
  /// majority of LPs; populated only by features that need SOS2
  /// (issue #504 line-loss L-secant chord).
  std::vector<std::vector<index_t>> sos2_sets;
  std::vector<double> col_scales;  ///< Per-column physical-to-LP scale factors
                                   ///< (physical = LP × scale; default 1.0)
  std::vector<double> row_scales;  ///< Per-row equilibration scale factors
                                   ///< (max |coeff| per row).
                                   ///< dual_physical = dual_LP / row_scale.
                                   ///< Empty when row equilibration is off.
  /// Equilibration method selected at flatten() time.  Persisted so the
  /// `LinearInterface::add_row` path can apply the same per-row
  /// equilibration to post-build cut rows.
  LpEquilibrationMethod equilibration_method {LpEquilibrationMethod::none};
  double scale_objective {1.0};  ///< Global objective divisor applied during
                                 ///< flatten().  obj_physical = obj_LP ×
                                 ///< scale_objective.

  /// Constant offset added to `get_obj_value_raw()` by
  /// `LinearInterface`.  Stored on the *raw* (LP-scaled) cost scale
  /// — i.e. the source `LinearProblem::m_obj_constant_` (physical
  /// scale) divided by `scale_objective` at `flatten()` time.  This
  /// makes both views compose naturally without an extra constant
  /// term in `get_obj_value()`:
  ///
  ///   get_obj_value_raw() = solver_raw + obj_constant_raw
  ///   get_obj_value()     = get_obj_value_raw() × scale_objective
  ///
  /// Pre-substitution tests that assert raw-value magnitudes
  /// continue to hold across formulations because the raw view
  /// already includes the substituted constant in LP-scaled units.
  ///
  /// Accumulated by `LinearProblem::add_obj_constant(c)` (physical
  /// scale) whenever a model rewrite folds a *dispatch-side* variable
  /// away via substitution (e.g. the P0 demand-failure variable
  /// rewrite: `fail = lmax − load` leaves the objective with a
  /// `+fail_cost × lmax` constant term that the LP solver knows
  /// nothing about, yet that constant IS dispatch cost and belongs
  /// in every consumer of `get_obj_value()`).
  ///
  /// Note: cost-to-go style shifts (e.g. the boundary-cut α-rebase
  /// `α' = α − c̄` in `sddp_boundary_cuts.cpp`) are handled at the
  /// SDDP level — they shift cut RHSs in-place and SDDPMethod adds
  /// `c̄_scene` to UB/LB at compute time.  Those do NOT use
  /// `obj_constant_raw`; the field is exclusively for dispatch-side
  /// substitutions.
  double obj_constant_raw {0.0};

  name_vec_t colnm;  ///< Variable names (dense; populated when names enabled)
  name_vec_t rownm;  ///< Constraint names (dense; populated when names enabled)
  index_map_t colmp;  ///< Map from variable names to indices
  index_map_t rowmp;  ///< Map from constraint names to indices

  /// Lightweight label metadata, populated unconditionally at flatten.
  /// Carries just the `(class_name, variable_name / constraint_name,
  /// uid, context)` fields that `LabelMaker` needs to synthesise a
  /// human-readable label on demand.  `LinearInterface` copies these
  /// at `load_flat` time so `write_lp` can produce real gtopt names
  /// even when `colnm` / `rownm` are empty (flatten ran without
  /// `col_with_names` / `row_with_names`).
  ///
  /// Held behind a `shared_ptr<const FlatLpMeta>` (see `FlatLpMeta`) so a
  /// phase's scenes can share one immutable copy.  Null on a default-
  /// constructed `FlatLinearProblem`; always set by `flatten()`.  Read
  /// through the `col_labels_meta()` / `row_labels_meta()` accessors,
  /// which fall back to an empty vector when the bundle is absent.
  std::shared_ptr<const FlatLpMeta> label_meta;

  /// Accessor for the shared column labels (empty when `label_meta` null).
  [[nodiscard]] const std::vector<SparseColLabel>& col_labels_meta()
      const noexcept
  {
    static const std::vector<SparseColLabel> kEmpty;
    return label_meta ? label_meta->col_labels_meta : kEmpty;
  }

  /// Accessor for the shared row labels (empty when `label_meta` null).
  [[nodiscard]] const std::vector<SparseRowLabel>& row_labels_meta()
      const noexcept
  {
    static const std::vector<SparseRowLabel> kEmpty;
    return label_meta ? label_meta->row_labels_meta : kEmpty;
  }

  /// Per-column / per-row objective time-basis, captured from
  /// `SparseCol::cost_scale_type` / `SparseRow::cost_scale_type` at
  /// `flatten()`.  Consumed by `OutputContext` (via `LinearInterface`) to
  /// select the inverse cost-factor family when reading reduced costs / duals
  /// back to physical units (Power → ÷duration, Energy → no ÷duration, Raw →
  /// face value).  Empty / out-of-range indices default to `Power`.
  std::vector<ConstraintScaleType> col_cost_scale_types;
  std::vector<ConstraintScaleType> row_cost_scale_types;

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

  /// Column index of the largest / smallest |coefficient| in A,
  /// engaged only when `compute_stats` populated the sweep.
  std::optional<ColIndex> stats_max_col {};
  std::optional<ColIndex> stats_min_col {};

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
    if (stats_min_abs <= 0.0 || !stats_min_col
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

/// Classify rows by constraint type (the second `_`-delimited token of the
/// row name, or the whole name when it has no separator) and accumulate
/// per-type count / nnz / min/max |coefficient|, sorted by descending
/// per-type max/min ratio.  Pure helper extracted from
/// `LinearProblem::flatten()` so the classification is unit-testable
/// without building an LP.  `matind` holds the row index of each CSC entry
/// in `matval`; `rownm` must have `nrows` entries.
[[nodiscard]] std::vector<FlatLinearProblem::RowTypeStatsEntry>
compute_row_type_stats(std::span<const double> matval,
                       std::span<const FlatLinearProblem::index_t> matind,
                       std::span<const std::string> rownm,
                       std::size_t nrows);

/// Result of `scan_bound_envelope` (LP_QUALITY Phase 3b): counts of
/// pathological post-flatten bounds plus the first offender of each kind.
struct BoundEnvelopeReport
{
  static constexpr auto npos = std::numeric_limits<std::size_t>::max();

  std::size_t free_cols {0};  ///< Columns with BOTH bounds at ±infinity
  std::size_t first_free_col {npos};
  std::size_t big_cols {0};  ///< Columns with a large-but-finite bound
  std::size_t first_big_col {npos};
  std::size_t big_rows {0};  ///< Rows with a large-but-finite bound
  std::size_t first_big_row {npos};

  [[nodiscard]] constexpr bool any() const noexcept
  {
    return free_cols > 0 || big_cols > 0 || big_rows > 0;
  }
};

/// Pure LP_QUALITY scan over the post-flatten bound vectors: counts FREE
/// columns (both bounds at the ±`infinity` sentinel — no box for a
/// first-order solver to project onto) and large-but-finite
/// (|b| >= `large_bound`) column/row bounds, keeping the first offender
/// of each kind.  Extracted from `LinearProblem::flatten()` so the
/// classification is unit-testable without building an LP; the caller
/// owns the logging.
[[nodiscard]] BoundEnvelopeReport scan_bound_envelope(
    std::span<const double> collb,
    std::span<const double> colub,
    std::span<const double> rowlb,
    std::span<const double> rowub,
    double infinity,
    double large_bound) noexcept;

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

  /// Accumulate a physical-scale constant into the objective.
  ///
  /// Use this when a model rewrite folds a variable away by
  /// substitution.  Example — demand-failure substitution:
  /// the algebraic identity `fail = lmax − load` lets us drop both
  /// the `fail` column and the `lcol + fcol = lmax` balance row, at
  /// the cost of:
  ///   1.  shifting `lcol`'s objective coefficient by `−fail_cost`
  ///       (the linear-in-load part of the substituted cost), and
  ///   2.  carrying `+fail_cost × lmax` as an LP-external constant
  ///       (the constant part of the substitution).
  ///
  /// `c` is interpreted on the *physical* cost scale (same units as
  /// the original `cost` fields of `SparseCol`).  It is NOT divided
  /// by `scale_objective` — the solver never sees this term.  Final
  /// reporting via `LinearInterface::get_obj_value()` combines:
  ///   `physical_obj = solver_raw_obj × scale_objective + obj_constant`
  /// so the obj-constant flows through SDDP cuts, forward-pass cost
  /// accumulation, and standalone obj-value reporting at full
  /// fidelity regardless of how aggressive `scale_objective` is.
  ///
  /// Calls accumulate (additive); pass a negative `c` to subtract.
  constexpr void add_obj_constant(double c) noexcept { m_obj_constant_ += c; }

  /// Current accumulated objective constant (physical units).  Zero
  /// in every existing call site that does not opt into the
  /// substitution rewrite.  Named with the `get_` prefix to match
  /// the other LP-side accessors (`get_obj_value` family).
  [[nodiscard]] constexpr double get_obj_constant() const noexcept
  {
    return m_obj_constant_;
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
  ColIndex add_col(SparseCol&& col)
  {
    const auto index = col_index_size(cols);

    if (col.is_integer) {
      ++colints;
    }

    // Normalize DblMax bounds to the configured infinity.
    normalize_bound(col.lowb);
    normalize_bound(col.uppb);

    // Integer / binary columns must never be rescaled by the auto-scaling
    // machinery: a non-unit column scale turns the physical bound 1 into a
    // non-integer LP upper bound (e.g. 1 / 0.0861 = 11.6189), and the
    // backend's integer enforcement then has no LP value that maps back to
    // physical 1.  Concretely: Commitment.status declared with bounds
    // [0, 1] and is_integer=true would get its LP bound rescaled to a
    // non-integer when KVL rows pull the column-scale away from 1, leaving
    // the MIP unable to represent physical u = 1 (only u ∈ {0, 0.086, …,
    // 0.947}).  Pinning scale = 1.0 here is the single source of truth for
    // that invariant and intentionally overrides any VariableScaleMap
    // entry — there is no legitimate use case for scaling a discrete
    // variable.
    //
    // The ``pin_scale`` flag extends the same exemption to LP-relax-of-
    // integer columns and never-integer-declared [0, 1] semantically-
    // binary variables (startup / shutdown) — see task #50.
    if (col.is_integer || col.pin_scale) [[unlikely]] {
      col.scale = 1.0;
    } else if (!col.class_name.empty() && !m_vsm_.empty()) {
      // Auto-resolve scale from VariableScaleMap when the caller provided
      // class_name metadata.  The map entry overrides any pre-computed
      // scale (including auto_scale) — only per-element fields that
      // avoid setting class_name metadata are immune.  When the map has
      // no entry for this (class, variable, uid), the pre-set scale (or
      // default 1.0) is kept.
      const auto resolved =
          m_vsm_.lookup(col.class_name, col.variable_name, col.variable_uid);
      if (resolved != 1.0) {
        col.scale = resolved;
      }
    }

    // Labelled columns must carry a concrete `variable_uid`.  Leaving
    // it at `unknown_uid = -1` serialises the column label as
    // `<class>_<var>_-1_…`, whose `-` is rejected by CoinLpIO's name
    // validator and causes CBC to strip every col/row label from
    // the written LP file (master #426 / a8a0e452, PR #429).  Throw
    // on the addition path so the offending callsite is flagged at
    // build time rather than surfacing as an opaque LP-file audit
    // failure.
    if (!col.class_name.empty() && col.variable_uid == unknown_uid) {
      throw std::invalid_argument(
          std::format("LinearProblem::add_col: labelled column (class='{}', "
                      "var='{}') has variable_uid = unknown_uid — the LP "
                      "label serialises to `…_-1_…` and CoinLpIO rejects "
                      "it (all labels dropped on write_lp).  Set a concrete "
                      "variable_uid, or leave class_name empty for an "
                      "anonymous column.",
                      col.class_name,
                      col.variable_name));
    }

    // Eager duplicate detection: two SparseCols with the same
    // (class_name, variable_name, variable_uid, context) 4-tuple
    // would produce the same LP label and silently overwrite each
    // other at lookup — always a bug.  Columns whose metadata is
    // fully empty (no identity at all) are skipped so structural
    // tests that build unnamed LP cells don't trip the detector.
    //
    // The check piggybacks on the map's `try_emplace` so there is
    // no extra lookup pass.
    if (!(col.class_name.empty() && col.variable_name.empty()
          && col.variable_uid == unknown_uid
          && std::holds_alternative<std::monostate>(col.context)))
    {
      auto [it, inserted] = m_col_meta_index_.try_emplace(
          SparseColLabel {
              .class_name = col.class_name,
              .variable_name = col.variable_name,
              .variable_uid = col.variable_uid,
              .context = col.context,
          },
          index);
      if (!inserted) {
        throw std::runtime_error(
            std::format("Duplicate LP column metadata: class='{}' var='{}' "
                        "uid={} (first at col {}, duplicate at col {})",
                        col.class_name,
                        col.variable_name,
                        col.variable_uid,
                        it->second,
                        index));
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
  RowIndex add_row(SparseRow&& row)
  {
    const auto index = row_index_size(rows);

    // Normalize DblMax bounds to the configured infinity.
    normalize_bound(row.lowb);
    normalize_bound(row.uppb);

    // Eager duplicate detection — see `add_col` for rationale.
    if (!(row.class_name.empty() && row.constraint_name.empty()
          && row.variable_uid == unknown_uid
          && std::holds_alternative<std::monostate>(row.context)))
    {
      auto [it, inserted] = m_row_meta_index_.try_emplace(
          SparseRowLabel {
              .class_name = row.class_name,
              .constraint_name = row.constraint_name,
              .variable_uid = row.variable_uid,
              .context = row.context,
          },
          index);
      if (!inserted) {
        throw std::runtime_error(
            std::format("Duplicate LP row metadata: class='{}' cons='{}' "
                        "uid={} (first at row {}, duplicate at row {})",
                        row.class_name,
                        row.constraint_name,
                        row.variable_uid,
                        it->second,
                        index));
      }
    }

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
   * Sets the lower bound of a column
   * @param index Column index
   * @param lowb New lower bound value
   */
  constexpr void set_col_lowb(ColIndex index, double lowb)
  {
    cols.at(index).lowb = lowb;
  }

  /**
   * Sets the upper bound of a column
   * @param index Column index
   * @param uppb New upper bound value
   */
  constexpr void set_col_uppb(ColIndex index, double uppb)
  {
    cols.at(index).uppb = uppb;
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

  /// Declare a Type-2 Special-Ordered-Set (SOS2) over the given column
  /// indices.  Stored on the LinearProblem until ``flatten()`` copies
  /// it into ``FlatLinearProblem::sos2_sets``; ``LinearInterface::
  /// load_flat`` then forwards each set to
  /// ``SolverBackend::add_sos2``.  See ``SolverBackend::add_sos2``
  /// for the supported-backend matrix.  Indices MUST already be valid
  /// column indices (call AFTER the relevant ``add_col`` calls).
  ///
  /// Vacuous sets (size < 2) are dropped silently — SOS2 over a
  /// single column has no effect, and an empty span is most likely
  /// an LP-build edge case that the caller would prefer not to see
  /// surface as a structured error in ``SolverBackend::add_sos2``.
  ///
  /// @param columns Column indices in geometric breakpoint order.
  void add_sos2(std::span<const ColIndex> columns)
  {
    if (columns.size() < 2) {
      return;
    }
    std::vector<index_t> idx;
    idx.reserve(columns.size());
    for (const auto& c : columns) {
      idx.push_back(static_cast<index_t>(c));
    }
    sos2_sets.push_back(std::move(idx));
  }

  /// Convenience overload accepting an ``initializer_list`` of column
  /// indices.  Forwards to the span-based ``add_sos2``.  Reserved for
  /// callers that build a small SOS2 inline (e.g. ``lp.add_sos2({c1,
  /// c2, c3})`` in a unit test) and have no pre-allocated vector to
  /// hand a span over.  Production callers in ``line_losses.cpp``
  /// (issue #504) use the span form because they construct the
  /// column list iteratively per (line, block).
  void add_sos2(std::initializer_list<ColIndex> columns)
  {
    add_sos2(std::span<const ColIndex> {columns.begin(), columns.size()});
  }

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
  /// SOS2 sets accumulated via ``add_sos2``.  Each inner vector is a
  /// list of column indices in geometric breakpoint order; forwarded
  /// to ``FlatLinearProblem::sos2_sets`` at ``flatten()`` and to
  /// ``SolverBackend::add_sos2`` at ``load_flat``.
  std::vector<std::vector<index_t>> sos2_sets;
  double m_infinity_ {DblMax};  ///< Target infinity for bound normalization
  /// Physical-scale objective constant accumulated via
  /// `add_obj_constant`.  Forwarded into `FlatLinearProblem` by
  /// `flatten()`; never touched by the solver.
  double m_obj_constant_ {0.0};
  VariableScaleMap m_vsm_ {};  ///< Auto-scale map (owned copy)
  LabelMaker m_label_maker_ {};  ///< Label generator (default = names off)

  /// Eager duplicate-detection maps for columns and rows, keyed on the
  /// `(class_name, variable_name/constraint_name, variable_uid,
  /// context)` 4-tuple that uniquely identifies an LP label source.
  /// Populated in `add_col` / `add_row`; collision throws
  /// `std::runtime_error` with both indices.  See
  /// `check_unique_col_label` / `check_unique_row_label` for the hook
  /// logic and `LinearInterface::m_post_flatten_col_meta_index_` for
  /// the post-flatten counterpart that guards dynamic insertions.
  std::unordered_map<SparseColLabel, ColIndex, SparseColLabelHash>
      m_col_meta_index_ {};
  std::unordered_map<SparseRowLabel, RowIndex, SparseRowLabelHash>
      m_row_meta_index_ {};
};

}  // namespace gtopt
