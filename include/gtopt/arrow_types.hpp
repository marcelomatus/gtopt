#pragma once

#include <arrow/api.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <arrow/type_traits.h>

namespace gtopt
{

using ArrowIndex = int64_t;
using ArrowField = std::shared_ptr<arrow::Field>;
using ArrowArray = std::shared_ptr<arrow::Array>;
using ArrowTable = std::shared_ptr<arrow::Table>;

using ArrowChunkedArray = std::shared_ptr<arrow::ChunkedArray>;

template<typename Uid>
struct ArrowTraits
{
};

template<>
struct ArrowTraits<int>
{
  using Type = arrow::Int32Type;
  static constexpr auto type() { return arrow::int32(); }
};

template<>
struct ArrowTraits<int16_t>
{
  using Type = arrow::Int16Type;
  static constexpr auto type() { return arrow::int16(); }
};

template<>
struct ArrowTraits<double>
{
  using Type = arrow::DoubleType;
  static constexpr auto type() { return arrow::float64(); }
};

}  // namespace gtopt

#define GTOPT_ARROW_ASSING_OR_RAISE(lhs, rexpr, msg) \
  { \
    auto&& __name = (rexpr); \
    { \
      if (not((__name).ok())) { \
        throw std::runtime_error((msg)); \
      } \
    } \
    (lhs) = std ::move((__name)).ValueUnsafe(); \
  }
