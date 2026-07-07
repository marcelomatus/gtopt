/**
 * @file      test_csv_helpers.hpp
 * @brief     Shared CSV output-reading helpers for integration tests
 * @date      2026-04-17
 * @copyright BSD-3-Clause
 *
 * Provides helpers to read single-row CSV output tables produced by
 * PlanningLP::write_out().  Arrow's CSV reader auto-detects column types,
 * so integer values (e.g. "250") become int64 columns, not double.
 * The helpers handle both cases transparently.
 *
 * This header is meant for inclusion in test .cpp files only.
 */

#pragma once

#include <filesystem>
#include <format>
#include <memory>
#include <vector>

#include <arrow/array.h>
#include <gtopt/array_index_traits.hpp>

namespace gtopt::test_helpers
{

/// Extract the value at row @p i from an Arrow chunk as double (handles the
/// double / float / int64 / int32 column types Arrow's CSV reader
/// auto-detects).
inline auto chunk_value_at(const std::shared_ptr<arrow::Array>& chunk,
                           int64_t i) -> double
{
  if (!chunk || i < 0 || i >= chunk->length()) {
    return 0.0;
  }
  switch (chunk->type_id()) {
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(i);
    case arrow::Type::FLOAT:
      return static_cast<double>(
          std::static_pointer_cast<arrow::FloatArray>(chunk)->Value(i));
    case arrow::Type::INT64:
      return static_cast<double>(
          std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(i));
    case arrow::Type::INT32:
      return static_cast<double>(
          std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(i));
    default:
      return 0.0;
  }
}

/// Read per-uid values for uid 1..count from a LONG-layout output table
/// (columns `scenario, stage, block, uid, value`).  The first row seen for
/// each uid wins, and uids with no row resolve to 0 — long output drops exact
/// zeros, so an all-zero field is a header-only (or unwritten) table that
/// reads back as every uid == 0.  Intended for single-period OPF cases.
///
/// `csv_read_table` is the strict input reader: a header-only CSV has
/// inferred-`null` columns that it rejects by throwing.  That is exactly the
/// "all uids zero" case here, so the read is wrapped to swallow it.
inline auto read_uid_values_long(const std::filesystem::path& path, int count)
    -> std::vector<double>
{
  std::vector<double> out(static_cast<std::size_t>(count), 0.0);

  std::shared_ptr<arrow::Table> table;
  try {
    auto tbl = csv_read_table(path);
    if (!tbl.has_value()) {
      auto sharded = path;
      sharded += "_s0_p0";
      tbl = csv_read_table(sharded);
    }
    if (!tbl.has_value()) {
      return out;  // unwritten dataset → every uid is 0
    }
    table = *tbl;
  } catch (const std::exception&) {
    return out;  // header-only all-zero field → every uid is 0
  }

  auto uid_col = table->GetColumnByName("uid");
  auto val_col = table->GetColumnByName("value");
  if (!uid_col || !val_col || uid_col->num_chunks() == 0
      || val_col->num_chunks() == 0)
  {
    return out;
  }
  const auto uchunk = uid_col->chunk(0);
  const auto vchunk = val_col->chunk(0);
  std::vector<bool> seen(static_cast<std::size_t>(count) + 1, false);
  const int64_t n = std::min(uchunk->length(), vchunk->length());
  for (int64_t i = 0; i < n; ++i) {
    const auto uid = static_cast<int>(chunk_value_at(uchunk, i));
    if (uid < 1 || uid > count || seen[static_cast<std::size_t>(uid)]) {
      continue;
    }
    seen[static_cast<std::size_t>(uid)] = true;
    out[static_cast<std::size_t>(uid - 1)] = chunk_value_at(vchunk, i);
  }
  return out;
}

}  // namespace gtopt::test_helpers
