/**
 * @file      label_maker.hpp
 * @brief     Generates LP column/row name strings honoring LpNamesLevel.
 * @date      Sun Jun 22 15:59:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * LabelMaker is the single place in gtopt where LP column/row name strings
 * are produced.  It consumes `SparseCol` / `SparseRow` directly (their
 * class_name / variable_name / variable_uid / context metadata fields) and
 * returns empty strings at low naming levels so callers never need to
 * wrap label calls in `if (names_level >= ...)` guards.
 *
 * Naming semantics (`LpNamesLevel`):
 *   - none:          no labels at all
 *   - minimal:       column labels only for `SparseCol::is_state == true`
 *                    (state variables need names for cascade/SDDP cut I/O)
 *   - only_cols:     all column labels + all row labels
 *   - cols_and_rows: like `only_cols`, but duplicates throw instead of warn
 *                    (consumed by LinearInterface; not this class's concern)
 *
 * LabelMaker is a value type (1 byte).  Copy it freely; it carries no
 * mutable buffers.  It is constructed once per solve, where
 * `LpMatrixOptions::lp_names_level` is first available, and then handed
 * to `LinearProblem` and travels with `FlatLinearProblem` into
 * `LinearInterface`.
 */

#pragma once

#include <string>

#include <gtopt/lp_matrix_enums.hpp>

namespace gtopt
{

struct SparseCol;
struct SparseRow;

class LabelMaker
{
public:
  constexpr LabelMaker() noexcept = default;

  constexpr explicit LabelMaker(LpNamesLevel level) noexcept
      : m_level_(level)
  {
  }

  [[nodiscard]] constexpr LpNamesLevel names_level() const noexcept
  {
    return m_level_;
  }

  /// True iff any column labels should be generated.
  [[nodiscard]] constexpr bool col_names_enabled() const noexcept
  {
    return m_level_ >= LpNamesLevel::minimal;
  }

  /// True iff row labels should be generated (level >= cols_and_rows).
  [[nodiscard]] constexpr bool row_names_enabled() const noexcept
  {
    return m_level_ >= LpNamesLevel::cols_and_rows;
  }

  /// True iff all column labels (not just state variables) are generated.
  [[nodiscard]] constexpr bool all_col_names_enabled() const noexcept
  {
    return m_level_ >= LpNamesLevel::only_cols;
  }

  /// True iff duplicate col/row names must raise an error rather than warn.
  [[nodiscard]] constexpr bool duplicates_are_errors() const noexcept
  {
    return m_level_ >= LpNamesLevel::cols_and_rows;
  }

  /// Generate the label for a column, honoring the current names level.
  ///
  /// Returns an empty string when the column should be unnamed:
  ///   - level == none                                       → ""
  ///   - level == minimal && !col.is_state                   → ""
  ///   - col.class_name empty && col.name empty              → ""
  ///   - col.context is monostate                            → uses
  ///   class+var+uid
  ///
  /// An explicit, caller-provided `col.name` always wins whenever
  /// `col_names_enabled()` is true.
  [[nodiscard]] std::string make_col_label(const SparseCol& col) const;

  /// Generate the label for a row, honoring the current names level.
  ///
  /// Returns an empty string when the row should be unnamed:
  ///   - level < cols_and_rows                               → ""
  ///   - row.class_name empty                                → ""
  ///   - row.context is monostate                            → uses
  ///   class+var+uid
  [[nodiscard]] std::string make_row_label(const SparseRow& row) const;

  /// Unconditional column label (ignores the names level).  Used by
  /// debug/diagnostic paths that need a label regardless of solver settings
  /// (e.g. log messages, stored cut names).
  [[nodiscard]] static std::string force_col_label(const SparseCol& col);

  /// Unconditional row label (ignores the names level).  Used by
  /// debug/diagnostic paths (e.g. `user_constraint_lp` debug logging,
  /// `benders_cut` rescale warnings, `sddp_cut_store` cut naming).
  [[nodiscard]] static std::string force_row_label(const SparseRow& row);

private:
  LpNamesLevel m_level_ {LpNamesLevel::none};
};

}  // namespace gtopt
