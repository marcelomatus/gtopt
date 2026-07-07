// SPDX-License-Identifier: BSD-3-Clause
#include <cstdint>
#include <vector>

#include <arrow/api.h>
#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>

using namespace gtopt;

namespace
{

[[nodiscard]] auto lip_i32(const std::vector<int32_t>& v) -> ArrowArray
{
  arrow::Int32Builder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

[[nodiscard]] auto lip_f64(const std::vector<double>& v) -> ArrowArray
{
  arrow::DoubleBuilder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

// Fetch a wide column by name and widen it to double for comparison.
// Handles both the double value columns (uid:N) and the int32 index columns.
[[nodiscard]] auto lip_col_doubles(const ArrowTable& t, const std::string& name)
    -> std::vector<double>
{
  auto col = t->GetColumnByName(name);
  REQUIRE(col != nullptr);
  const auto chunk = col->chunk(0);
  std::vector<double> out;
  if (auto darr = cast_to_double_array(chunk)) {
    out.reserve(static_cast<std::size_t>(darr->length()));
    for (int64_t i = 0; i < darr->length(); ++i) {
      out.push_back(darr->Value(i));
    }
    return out;
  }
  auto iarr = cast_to_int32_array(chunk);
  REQUIRE(iarr != nullptr);
  out.reserve(static_cast<std::size_t>(iarr->length()));
  for (int64_t i = 0; i < iarr->length(); ++i) {
    out.push_back(static_cast<double>(iarr->Value(i)));
  }
  return out;
}

}  // namespace

TEST_CASE("is_long_layout sniff")  // NOLINT
{
  SUBCASE("long table has bare uid + value")
  {
    auto t = arrow::Table::Make(
        arrow::schema({
            arrow::field("stage", arrow::int32()),
            arrow::field("uid", arrow::int32()),
            arrow::field("value", arrow::float64()),
        }),
        {lip_i32({1, 1}), lip_i32({10, 20}), lip_f64({1.5, 2.5})});
    CHECK(is_long_layout(t));
  }

  SUBCASE("wide table (uid:N columns) is not long")
  {
    auto t = arrow::Table::Make(arrow::schema({
                                    arrow::field("block", arrow::int32()),
                                    arrow::field("uid:10", arrow::float64()),
                                }),
                                {lip_i32({1, 2}), lip_f64({1.5, 3.5})});
    CHECK_FALSE(is_long_layout(t));
  }
}

TEST_CASE("pivot_long_to_wide basic")  // NOLINT
{
  // stage/block index, two uids, dense.
  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("stage", arrow::int32()),
                                  arrow::field("block", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::float64()),
                              }),
                              {
                                  lip_i32({1, 1, 1, 1}),
                                  lip_i32({1, 1, 2, 2}),
                                  lip_i32({10, 20, 10, 20}),
                                  lip_f64({1.5, 2.5, 3.5, 4.5}),
                              });

  auto wide = pivot_long_to_wide(t);
  REQUIRE(wide != nullptr);

  // Index columns + one column per uid, no bare uid/value.
  CHECK(wide->GetColumnByName("stage") != nullptr);
  CHECK(wide->GetColumnByName("block") != nullptr);
  CHECK(wide->GetColumnByName("uid") == nullptr);
  CHECK(wide->GetColumnByName("value") == nullptr);
  CHECK(wide->num_rows() == 2);  // (1,1) and (1,2)

  // Row order follows first-seen key order: (1,1) then (1,2).
  CHECK(lip_col_doubles(wide, "uid:10") == std::vector<double> {1.5, 3.5});
  CHECK(lip_col_doubles(wide, "uid:20") == std::vector<double> {2.5, 4.5});
  CHECK(lip_col_doubles(wide, "block") == std::vector<double> {1.0, 2.0});
}

TEST_CASE("pivot_long_to_wide zero-fills missing cells")  // NOLINT
{
  // Single stage index; uid 20 missing at stage 2 → must zero-fill.
  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("stage", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::float64()),
                              }),
                              {
                                  lip_i32({1, 1, 2}),
                                  lip_i32({10, 20, 10}),
                                  lip_f64({1.5, 2.5, 3.5}),
                              });

  auto wide = pivot_long_to_wide(t);
  REQUIRE(wide != nullptr);
  CHECK(wide->num_rows() == 2);  // stage 1 and stage 2
  CHECK(lip_col_doubles(wide, "uid:10") == std::vector<double> {1.5, 3.5});
  // stage 2 / uid 20 absent in long → 0.0 in wide.
  CHECK(lip_col_doubles(wide, "uid:20") == std::vector<double> {2.5, 0.0});
}

TEST_CASE("pivot_long_to_wide preserves integer value type")  // NOLINT
{
  // Line/active-style int field: long value column is int32.
  arrow::Int32Builder vb;
  REQUIRE(vb.AppendValues(std::vector<int32_t> {1, 0}).ok());
  std::shared_ptr<arrow::Array> varr;
  REQUIRE(vb.Finish(&varr).ok());

  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("block", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::int32()),
                              }),
                              {lip_i32({1, 2}), lip_i32({7, 7}), varr});

  auto wide = pivot_long_to_wide(t);
  REQUIRE(wide != nullptr);
  auto col = wide->GetColumnByName("uid:7");
  REQUIRE(col != nullptr);
  CHECK(col->type()->id() == arrow::Type::INT32);
  auto arr = std::static_pointer_cast<arrow::Int32Array>(col->chunk(0));
  REQUIRE(arr->length() == 2);
  CHECK(arr->Value(0) == 1);
  CHECK(arr->Value(1) == 0);
}
