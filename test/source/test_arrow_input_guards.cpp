// SPDX-License-Identifier: BSD-3-Clause
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <doctest/doctest.h>
#include <gtopt/arrow_input_guards.hpp>

using namespace gtopt;

namespace test_arrow_input_guards_ns
{
namespace
{

[[nodiscard]] std::shared_ptr<arrow::Table> make_table(
    const std::vector<double>& values)
{
  arrow::DoubleBuilder builder;
  REQUIRE(builder.AppendValues(values).ok());
  std::shared_ptr<arrow::Array> array;
  REQUIRE(builder.Finish(&array).ok());
  auto schema = arrow::schema({arrow::field("v", arrow::float64())});
  return arrow::Table::Make(schema, {array});
}

[[nodiscard]] std::shared_ptr<arrow::Table> make_float_table(
    const std::vector<float>& values)
{
  arrow::FloatBuilder builder;
  REQUIRE(builder.AppendValues(values).ok());
  std::shared_ptr<arrow::Array> array;
  REQUIRE(builder.Finish(&array).ok());
  auto schema = arrow::schema({arrow::field("v32", arrow::float32())});
  return arrow::Table::Make(schema, {array});
}

}  // namespace

TEST_CASE("arrow_nan_guard: accepts finite, infinity, and empty Float64")
{
  SUBCASE("all-finite passes")
  {
    auto table = make_table({1.0, -2.0, 3.14, 0.0});
    CHECK_NOTHROW(reject_nan_in_float_columns(*table, "test.parquet"));
  }

  SUBCASE("positive and negative infinity pass")
  {
    constexpr auto inf = std::numeric_limits<double>::infinity();
    auto table = make_table({inf, -inf, 1.0});
    CHECK_NOTHROW(reject_nan_in_float_columns(*table, "test.parquet"));
  }

  SUBCASE("empty column passes")
  {
    auto table = make_table({});
    CHECK_NOTHROW(reject_nan_in_float_columns(*table, "test.parquet"));
  }
}

TEST_CASE("arrow_nan_guard: throws on Float64 NaN with file/column/row context")
{
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  auto table = make_table({1.0, 2.0, nan, 4.0});

  std::string what;
  try {
    reject_nan_in_float_columns(*table, "my_input.parquet");
    FAIL("expected std::runtime_error");
  } catch (const std::runtime_error& e) {
    what = e.what();
  }

  CHECK(what.find("my_input.parquet") != std::string::npos);
  CHECK(what.find("'v'") != std::string::npos);
  CHECK(what.find("row 2") != std::string::npos);
  CHECK(what.find("NaN") != std::string::npos);
}

TEST_CASE("arrow_nan_guard: detects Float32 NaN too")
{
  constexpr auto nan_f = std::numeric_limits<float>::quiet_NaN();
  auto table = make_float_table({0.5F, nan_f});

  CHECK_THROWS_AS(reject_nan_in_float_columns(*table, "f32.parquet"),
                  std::runtime_error);
}

TEST_CASE("arrow_nan_guard: ignores non-floating-point columns (int64, string)")
{
  arrow::Int64Builder ib;
  REQUIRE(ib.AppendValues({1, 2, 3}).ok());
  std::shared_ptr<arrow::Array> iarr;
  REQUIRE(ib.Finish(&iarr).ok());

  arrow::StringBuilder sb;
  REQUIRE(sb.AppendValues({"a", "b", "c"}).ok());
  std::shared_ptr<arrow::Array> sarr;
  REQUIRE(sb.Finish(&sarr).ok());

  auto schema = arrow::schema({
      arrow::field("i", arrow::int64()),
      arrow::field("s", arrow::utf8()),
  });
  auto table = arrow::Table::Make(schema, {iarr, sarr});

  CHECK_NOTHROW(reject_nan_in_float_columns(*table, "noints.parquet"));
}

TEST_CASE("arrow_nan_guard: scans multi-chunk Float64 columns")
{
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  // Build chunk 1: [1.0, 2.0], chunk 2: [3.0, nan, 5.0]
  arrow::DoubleBuilder b1;
  REQUIRE(b1.AppendValues({1.0, 2.0}).ok());
  std::shared_ptr<arrow::Array> a1;
  REQUIRE(b1.Finish(&a1).ok());

  arrow::DoubleBuilder b2;
  REQUIRE(b2.AppendValues({3.0, nan, 5.0}).ok());
  std::shared_ptr<arrow::Array> a2;
  REQUIRE(b2.Finish(&a2).ok());

  auto chunked =
      std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector {a1, a2});
  auto schema = arrow::schema({arrow::field("v", arrow::float64())});
  auto table = arrow::Table::Make(schema, {chunked});

  std::string what;
  try {
    reject_nan_in_float_columns(*table, "chunked.parquet");
    FAIL("expected std::runtime_error");
  } catch (const std::runtime_error& e) {
    what = e.what();
  }
  // chunk 1 has 2 rows; NaN is at chunk 2 row 1 → global row 3
  CHECK(what.find("row 3") != std::string::npos);
}

// ============================================================================
// reject_non_numeric_columns — CSV strict-type guard
// ============================================================================

TEST_CASE("arrow_input_guards: reject_non_numeric_columns accepts int + float")
{
  arrow::Int64Builder ib;
  REQUIRE(ib.AppendValues({1, -5, 100}).ok());
  std::shared_ptr<arrow::Array> iarr;
  REQUIRE(ib.Finish(&iarr).ok());

  arrow::DoubleBuilder db;
  REQUIRE(db.AppendValues({1.0, 1.5, -2.5}).ok());
  std::shared_ptr<arrow::Array> darr;
  REQUIRE(db.Finish(&darr).ok());

  auto schema = arrow::schema({
      arrow::field("idx", arrow::int64()),
      arrow::field("v", arrow::float64()),
  });
  auto table = arrow::Table::Make(schema, {iarr, darr});

  CHECK_NOTHROW(reject_non_numeric_columns(*table, "ok.csv"));
}

TEST_CASE(
    "arrow_input_guards: reject_non_numeric_columns fails on string column")
{
  arrow::StringBuilder sb;
  REQUIRE(sb.AppendValues({"1", "foo", "3"}).ok());
  std::shared_ptr<arrow::Array> sarr;
  REQUIRE(sb.Finish(&sarr).ok());

  auto schema = arrow::schema({arrow::field("value", arrow::utf8())});
  auto table = arrow::Table::Make(schema, {sarr});

  std::string what;
  try {
    reject_non_numeric_columns(*table, "mixed.csv");
    FAIL("expected std::runtime_error");
  } catch (const std::runtime_error& e) {
    what = e.what();
  }

  CHECK(what.find("mixed.csv") != std::string::npos);
  CHECK(what.find("'value'") != std::string::npos);
  CHECK(what.find("not") != std::string::npos);
  CHECK(what.find("numeric") != std::string::npos);
}

TEST_CASE(
    "arrow_input_guards: reject_non_numeric_columns accepts all integer widths")
{
  arrow::Int8Builder b8;
  REQUIRE(b8.AppendValues({int8_t {1}, int8_t {2}}).ok());
  std::shared_ptr<arrow::Array> a8;
  REQUIRE(b8.Finish(&a8).ok());

  arrow::UInt32Builder bu32;
  REQUIRE(bu32.AppendValues({uint32_t {10}, uint32_t {20}}).ok());
  std::shared_ptr<arrow::Array> au32;
  REQUIRE(bu32.Finish(&au32).ok());

  auto schema = arrow::schema({
      arrow::field("i8", arrow::int8()),
      arrow::field("u32", arrow::uint32()),
  });
  auto table = arrow::Table::Make(schema, {a8, au32});

  CHECK_NOTHROW(reject_non_numeric_columns(*table, "ints.csv"));
}

}  // namespace test_arrow_input_guards_ns
