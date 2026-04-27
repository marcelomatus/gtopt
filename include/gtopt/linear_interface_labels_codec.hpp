/**
 * @file      linear_interface_labels_codec.hpp
 * @brief     Byte-level (de)serializer for LP column / row label metadata.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Internal helpers used by the low-memory snapshot pipeline in
 * @ref gtopt::LinearInterface to compress/decompress the per-cell label
 * metadata (`SparseColLabel`, `SparseRowLabel`).  Extracted from the
 * anonymous namespace of `source/linear_interface.cpp` so the codec can
 * be unit-tested in isolation.
 *
 * Layout, per entry:
 * @code
 *   u16  class_name_len
 *   N    class_name bytes
 *   u16  second_string_len   (variable_name or constraint_name)
 *   M    second_string bytes
 *   8    variable_uid (memcpy of Uid)
 *   sizeof(LpContext)  context bytes (memcpy of std::variant<...>)
 * @endcode
 *
 * `LpContext` is a `std::variant` of tuples of strong-int-typed UIDs —
 * trivially copyable, so a direct memcpy round-trips correctly.  The
 * string_view content is copied inline so the compressed form is
 * self-contained (no dangling pointers into the original string storage).
 * Decompression re-materialises each string into a per-LinearInterface
 * `string_pool` whose stable addresses back the revived string_views;
 * the pool MUST be reserved to at least `2 * num_entries` before calling
 * `deserialize_labels_meta` so push_back never reallocates.
 */

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/uid.hpp>

namespace gtopt
{

/// True for a metadata entry that carries no identifying fields.
/// Entries with no class_name / variable_name / unknown uid /
/// monostate context are skipped by the duplicate-detection maps —
/// several internal paths (structural tests, unnamed bootstrap
/// bookkeeping) legitimately create unlabelled cols/rows and they
/// must not trip the detector.
[[nodiscard]] constexpr bool is_empty_col_label(
    const SparseColLabel& l) noexcept
{
  return l.class_name.empty() && l.variable_name.empty()
      && l.variable_uid == unknown_uid
      && std::holds_alternative<std::monostate>(l.context);
}

[[nodiscard]] constexpr bool is_empty_row_label(
    const SparseRowLabel& l) noexcept
{
  return l.class_name.empty() && l.constraint_name.empty()
      && l.variable_uid == unknown_uid
      && std::holds_alternative<std::monostate>(l.context);
}

/// Serialize a vector of column labels into a self-contained byte buffer.
[[nodiscard]] std::vector<char> serialize_labels_meta(
    const std::vector<SparseColLabel>& labels);

/// Serialize a vector of row labels into a self-contained byte buffer.
[[nodiscard]] std::vector<char> serialize_labels_meta(
    const std::vector<SparseRowLabel>& labels);

/// Inverse of `serialize_labels_meta` for column labels.  Re-materialises
/// strings into `string_pool` (which MUST be reserved to at least
/// `2 * num_entries` before calling, so string_views into pool entries
/// stay valid — push_back must not reallocate).
[[nodiscard]] std::vector<SparseColLabel> deserialize_col_labels_meta(
    std::span<const char> data,
    std::size_t num_entries,
    std::vector<std::string>& string_pool);

/// Inverse of `serialize_labels_meta` for row labels.
[[nodiscard]] std::vector<SparseRowLabel> deserialize_row_labels_meta(
    std::span<const char> data,
    std::size_t num_entries,
    std::vector<std::string>& string_pool);

}  // namespace gtopt
