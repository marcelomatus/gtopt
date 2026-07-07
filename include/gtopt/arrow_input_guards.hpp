/**
 * @file      arrow_input_guards.hpp
 * @brief     Fail-hard guards for Arrow tables loaded from Parquet / CSV
 * @date      2026-06-06
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * gtopt's LP build cannot tolerate ``NaN`` in input field values — a
 * ``NaN`` coefficient propagates into the matrix and every solver
 * (CPLEX, HiGHS, CLP, CBC) either refuses to start or returns garbage.
 * ``±Infinity`` is acceptable (the LP layer's ``is_infinity()`` guard
 * clamps anything ``≥ solver_infinity`` to the backend's own sentinel,
 * so the row/column bound becomes "unbounded"), but ``NaN`` has no
 * meaningful LP interpretation.
 *
 * It also cannot tolerate non-numeric columns from CSV input — Arrow's
 * default type inference silently falls back to a string column when a
 * value fails to parse as a number (e.g. ``"1,foo,3"``), masking the
 * data error.  gtopt rejects any non-integer / non-floating CSV column.
 *
 * This header exposes:
 *
 *   * :func:`gtopt::reject_nan_in_float_columns`  — called by the
 *     parquet readers in ``array_index_traits.cpp`` and
 *     ``sddp_cut_parquet.cpp`` immediately after Arrow materialises
 *     the table.
 *   * :func:`gtopt::reject_non_numeric_columns`  — called by the CSV
 *     reader in ``array_index_traits.cpp`` after Arrow's type-inference
 *     pass.  Integer columns are accepted (they implicitly convert to
 *     float downstream), as are Float32 / Float64 columns.  Any other
 *     column type triggers a hard fail with file / column context.
 *
 * Both throw ``std::runtime_error`` on the first violation so the user
 * gets a precise diagnosis instead of an opaque solver failure later.
 */

#pragma once

#include <cmath>
#include <format>
#include <stdexcept>
#include <string_view>

#include <arrow/api.h>
#include <arrow/type_traits.h>

namespace gtopt
{

/// Scan every Float32 / Float64 column of @p table for NaN cells; throw
/// ``std::runtime_error`` on the first NaN encountered.  Infinity is
/// allowed — only NaN is rejected.
///
/// @param table  Arrow table to scan (must not be ``nullptr``).
/// @param path   Source label (file path, table name, …) — included
///               verbatim in the error message so the user can locate
///               the offending input.
/// @throws std::runtime_error  on the first NaN cell, with the column
///         name and chunked-array row index (relative to column start).
///
/// @note Pointer arithmetic into Arrow's contiguous chunk buffers is
///       suppressed for clang-tidy — Arrow's ``raw_values()`` contract
///       guarantees a valid ``length()``-sized array, which is the
///       interpretation the bounds-pointer-arithmetic check cannot
///       prove on its own.
inline void reject_nan_in_float_columns(const arrow::Table& table,
                                        std::string_view path)
{
  const auto& schema = *table.schema();
  for (int col_i = 0; col_i < table.num_columns(); ++col_i) {
    const auto& field = *schema.field(col_i);
    const auto type_id = field.type()->id();
    if (type_id != arrow::Type::DOUBLE && type_id != arrow::Type::FLOAT) {
      continue;
    }

    const auto& column = *table.column(col_i);
    int64_t row_offset = 0;
    for (const auto& chunk : column.chunks()) {
      const auto length = chunk->length();
      if (type_id == arrow::Type::DOUBLE) {
        const auto* data =
            std::static_pointer_cast<arrow::DoubleArray>(chunk)->raw_values();
        for (int64_t i = 0; i < length; ++i) {
          if (std::isnan(data[i])) {
            throw std::runtime_error(std::format(
                "Parquet input '{}' column '{}' row {}: NaN values are "
                "not allowed (gtopt rejects NaN inputs; ±Infinity is "
                "acceptable and clamped to solver infinity)",
                path,
                field.name(),
                row_offset + i));
          }
        }
      } else {
        const auto* data =
            std::static_pointer_cast<arrow::FloatArray>(chunk)->raw_values();
        for (int64_t i = 0; i < length; ++i) {
          if (std::isnan(data[i])) {
            throw std::runtime_error(std::format(
                "Parquet input '{}' column '{}' row {}: NaN values are "
                "not allowed (gtopt rejects NaN inputs; ±Infinity is "
                "acceptable and clamped to solver infinity)",
                path,
                field.name(),
                row_offset + i));
          }
        }
      }
      row_offset += length;
    }
  }
}

/// Scan every column of @p table for non-numeric types; throw
/// ``std::runtime_error`` if any column has a type that is neither
/// integer nor floating-point.  Integer columns (Int8 … Int64,
/// UInt8 … UInt64) are accepted because they implicitly convert to
/// double downstream, satisfying the user-stated rule that ``"1, 1.0,
/// -5"`` is a valid mixed numeric column.
///
/// Use this on tables returned by Arrow's CSV ``TableReader::Read``,
/// whose type-inference pass silently falls back to ``String`` whenever
/// a single non-numeric token appears (e.g. ``"1, foo, 3"``) — exactly
/// the failure mode we want surfaced loudly rather than absorbed into
/// the LP build.
///
/// @param table  Arrow table to scan (must not be ``nullptr``).
/// @param path   Source label (file path, table name, …) — included
///               verbatim in the error message.
/// @throws std::runtime_error  on the first non-numeric column, naming
///         the column and its inferred Arrow type.
inline void reject_non_numeric_columns(const arrow::Table& table,
                                       std::string_view path)
{
  const auto& schema = *table.schema();
  for (int col_i = 0; col_i < table.num_columns(); ++col_i) {
    const auto& field = *schema.field(col_i);
    const auto& type = *field.type();
    if (arrow::is_integer(type.id()) || arrow::is_floating(type.id())) {
      continue;
    }
    throw std::runtime_error(std::format(
        "CSV input '{}' column '{}': inferred Arrow type '{}' is not "
        "numeric.  gtopt requires every CSV column to parse as integer "
        "or float (e.g. '1, 1.0, -5' OK, '1, foo, 3' rejected).  Check "
        "the source CSV for stray non-numeric tokens.",
        path,
        field.name(),
        type.ToString()));
  }
}

}  // namespace gtopt
