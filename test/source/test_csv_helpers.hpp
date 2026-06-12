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

namespace gtopt::test_helpers  // NOLINT(cert-dcl59-cpp)
{

/// Extract the first value from an Arrow chunk as double (handles int64 and
/// double column types that Arrow's CSV reader auto-detects).
inline auto chunk_first_value(const std::shared_ptr<arrow::Array>& chunk)
    -> double
{
  if (!chunk || chunk->length() == 0) {
    return 0.0;
  }
  if (chunk->type_id() == arrow::Type::DOUBLE) {
    return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(0);
  }
  if (chunk->type_id() == arrow::Type::INT64) {
    return static_cast<double>(
        std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(0));
  }
  return 0.0;
}

/// Read single-row uid:1..uid:count values from a CSV output table.
///
/// The @p path should omit the `.csv` extension (csv_read_table appends
/// it).  `PlanningLP::write_out` shards CSV tables per (scene, phase)
/// with a `_s<N>_p<M>` suffix — since the IEEE original cases are
/// single-scene / single-phase, try the plain `{path}.csv` first and
/// fall back to `{path}_s0_p0.csv` when the plain name is absent.
inline auto read_uid_values(const std::filesystem::path& path, int count)
    -> std::vector<double>
{
  std::vector<double> out;
  auto tbl = csv_read_table(path);
  if (!tbl.has_value()) {
    auto sharded = path;
    sharded += "_s0_p0";
    tbl = csv_read_table(sharded);
  }
  if (!tbl.has_value()) {
    return out;
  }
  for (int uid = 1; uid <= count; ++uid) {
    const auto col_name = std::format("uid:{}", uid);
    auto col = (*tbl)->GetColumnByName(col_name);
    if (!col || col->num_chunks() == 0) {
      out.push_back(0.0);
      continue;
    }
    out.push_back(chunk_first_value(col->chunk(0)));
  }
  return out;
}

}  // namespace gtopt::test_helpers
