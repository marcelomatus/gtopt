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
class AsciiNameCache;

class LabelMaker
{
public:
  constexpr LabelMaker() noexcept = default;

  constexpr explicit LabelMaker(LpNamesLevel level) noexcept
      : m_level_(level)
  {
  }

  /// Full ctor (issue #508): selects label render style and an optional
  /// `AsciiNameCache` consulted under `LpLabelStyle::extended`.
  ///
  /// Under `compact` the cache pointer is ignored (and stays `nullptr`
  /// by default) — the render path is byte-identical to today.  Under
  /// `extended` a non-null cache produces the human-readable label
  /// form; a null cache or an unknown `(class, uid)` falls back to
  /// compact per-label (preserves anonymous synthetic columns).
  constexpr LabelMaker(LpNamesLevel level,
                       LpLabelStyle style,
                       const AsciiNameCache* cache = nullptr) noexcept
      : m_level_(level)
      , m_style_(style)
      , m_cache_(cache)
  {
  }

  [[nodiscard]] constexpr LpNamesLevel names_level() const noexcept
  {
    return m_level_;
  }

  /// Currently configured label render style (issue #508).
  [[nodiscard]] constexpr LpLabelStyle label_style() const noexcept
  {
    return m_style_;
  }

  /// ASCII-name cache used by extended renders.  `nullptr` under
  /// `compact` and at default construction; non-null only when a
  /// label consumer (e.g. `write_lp`) wires the cache in.
  [[nodiscard]] constexpr const AsciiNameCache* ascii_name_cache()
      const noexcept
  {
    return m_cache_;
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
  LpLabelStyle m_style_ {LpLabelStyle::compact};
  const AsciiNameCache* m_cache_ {nullptr};
};

}  // namespace gtopt
