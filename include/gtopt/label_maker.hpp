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
 * returns empty strings when naming is disabled so callers never need to
 * guard label calls.
 *
 * Naming semantics (`LpNamesLevel`):
 *   - none: no labels at all (default, lowest memory)
 *   - all:  all column + row labels + name-to-index maps; duplicates throw
 *
 * LabelMaker is a value type (1 byte).  Copy it freely; it carries no
 * mutable buffers.  It is constructed at the specific call sites that need
 * names (lp_debug, lp_file) and handed to `LinearProblem` and travels
 * with `FlatLinearProblem` into `LinearInterface`.
 */

#pragma once

#include <string>
#include <string_view>

#include <gtopt/lp_context.hpp>
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

  /// True iff dense column name vectors / name-maps should be generated.
  /// State variable I/O uses the StateVariable map (ColIndex-based)
  /// directly and does not need column name strings.
  [[nodiscard]] constexpr bool col_names_enabled() const noexcept
  {
    return m_level_ == LpNamesLevel::all;
  }

  /// True iff row labels should be generated.
  [[nodiscard]] constexpr bool row_names_enabled() const noexcept
  {
    return m_level_ == LpNamesLevel::all;
  }

  /// True iff all column labels (not just state variables) are generated.
  [[nodiscard]] constexpr bool all_col_names_enabled() const noexcept
  {
    return m_level_ == LpNamesLevel::all;
  }

  /// True iff duplicate col/row names must raise an error rather than warn.
  [[nodiscard]] constexpr bool duplicates_are_errors() const noexcept
  {
    return m_level_ == LpNamesLevel::all;
  }

  /// Generate the label for a column.
  /// Returns an empty string when naming is disabled or class_name is empty.
  [[nodiscard]] std::string make_col_label(const SparseCol& col) const;

  /// Generate the label for a row.
  /// Returns an empty string when naming is disabled or class_name is empty.
  [[nodiscard]] std::string make_row_label(const SparseRow& row) const;

private:
  LpNamesLevel m_level_ {LpNamesLevel::none};
};

}  // namespace gtopt
