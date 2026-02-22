/**
 * @file      test_array_index_traits.cpp
 * @brief     Unit tests for the refactored array_index_traits helpers
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Directly tests the now-public helper functions:
 *   gtopt::build_table_path   – pure path construction
 *   gtopt::csv_read_table     – CSV reader (success + error paths)
 *   gtopt::parquet_read_table – Parquet reader (success + error paths)
 *   gtopt::try_read_table     – format-dispatch with fallback
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/// Write a minimal single-column CSV to @p path (no extension).
void write_csv(const std::filesystem::path& path, std::string_view content)
{
  std::ofstream ofs(path.string() + ".csv");
  ofs << content;
}

/// Write a minimal single-column Parquet file to @p path (no extension).
void write_parquet(const std::filesystem::path& path)
{
  // Build a one-column double table with two rows
  arrow::DoubleBuilder builder;
  REQUIRE(builder.Append(1.0).ok());
  REQUIRE(builder.Append(2.0).ok());
  std::shared_ptr<arrow::Array> arr;
  REQUIRE(builder.Finish(&arr).ok());

  auto schema = arrow::schema({arrow::field("val", arrow::float64())});
  auto table = arrow::Table::Make(schema, {arr});

  const auto fname = path.string() + ".parquet";
  auto ostream = arrow::io::FileOutputStream::Open(fname);
  REQUIRE(ostream.ok());
  REQUIRE(parquet::arrow::WriteTable(
              *table, arrow::default_memory_pool(), ostream.ValueOrDie(), 1024)
              .ok());
  REQUIRE(ostream.ValueOrDie()->Close().ok());
}

std::filesystem::path tmp_path(const std::string& name)
{
  return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// ---------------------------------------------------------------------------
// build_table_path
// ---------------------------------------------------------------------------

TEST_CASE("build_table_path - normal cname/fname")
{
  const auto p = build_table_path("/input", "Generator", "capacity");
  CHECK(p == std::filesystem::path("/input/Generator/capacity"));
}

TEST_CASE("build_table_path - fname with '@' separator")
{
  // "OtherClass@myfield" → input_dir / "OtherClass" / "myfield"
  const auto p = build_table_path("/data", "Generator", "OtherClass@myfield");
  CHECK(p == std::filesystem::path("/data/OtherClass/myfield"));
}

TEST_CASE("build_table_path - empty input_dir")
{
  const auto p = build_table_path("", "Bus", "theta");
  CHECK(p == std::filesystem::path("Bus/theta"));
}

// ---------------------------------------------------------------------------
// csv_read_table
// ---------------------------------------------------------------------------

TEST_CASE("csv_read_table - success")
{
  const auto stem = tmp_path("ait_csv_ok");
  write_csv(stem, "scenario,stage,block,val\n1,1,1,3.14\n1,1,2,2.71\n");

  auto result = csv_read_table(stem);
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 2);
  CHECK((*result)->num_columns() == 4);
}

TEST_CASE("csv_read_table - file not found returns error")
{
  const auto stem = tmp_path("ait_csv_notfound_xyz");
  auto result = csv_read_table(stem);
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

// ---------------------------------------------------------------------------
// parquet_read_table
// ---------------------------------------------------------------------------

TEST_CASE("parquet_read_table - success")
{
  const auto stem = tmp_path("ait_parquet_ok");
  write_parquet(stem);

  auto result = parquet_read_table(stem);
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 2);
  CHECK((*result)->num_columns() == 1);
}

TEST_CASE("parquet_read_table - file not found returns error")
{
  const auto stem = tmp_path("ait_parquet_notfound_xyz");
  auto result = parquet_read_table(stem);
  CHECK_FALSE(result.has_value());
  CHECK(!result.error().empty());
}

// ---------------------------------------------------------------------------
// try_read_table
// ---------------------------------------------------------------------------

TEST_CASE("try_read_table - csv format, csv file present")
{
  const auto stem = tmp_path("ait_try_csv");
  write_csv(stem, "val\n1.0\n2.0\n");

  auto result = try_read_table(stem, "csv");
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 2);
}

TEST_CASE("try_read_table - parquet format, parquet file present")
{
  const auto stem = tmp_path("ait_try_parquet");
  write_parquet(stem);

  auto result = try_read_table(stem, "parquet");
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 2);
}

TEST_CASE("try_read_table - csv format, fallback to parquet succeeds")
{
  // Only the parquet file exists; CSV read should fail then fall back
  const auto stem = tmp_path("ait_try_fallback_parquet");
  write_parquet(stem);

  auto result = try_read_table(stem, "csv");
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 2);
}

TEST_CASE("try_read_table - parquet format, fallback to csv succeeds")
{
  // Only the CSV file exists; parquet read should fail then fall back
  const auto stem = tmp_path("ait_try_fallback_csv");
  write_csv(stem, "val\n9.9\n");

  auto result = try_read_table(stem, "parquet");
  REQUIRE(result.has_value());
  CHECK((*result)->num_rows() == 1);
}

TEST_CASE("try_read_table - both formats missing returns error")
{
  const auto stem = tmp_path("ait_try_both_missing_xyz");
  auto result = try_read_table(stem, "csv");
  CHECK_FALSE(result.has_value());

  auto result2 = try_read_table(stem, "parquet");
  CHECK_FALSE(result2.has_value());
}
