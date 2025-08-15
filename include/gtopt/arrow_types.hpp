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
using ArrowColumnResult = arrow::Result<ArrowChunkedArray>;

/// Get column by name with explicit error checking
inline auto GetColumn(const ArrowTable& table, std::string_view name) -> ArrowColumnResult
{
    return table->GetColumnByName(std::string(name));
}

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
  using Type = arrow::Int32Type;
  static constexpr auto type() { return arrow::int16(); }
};

template<>
struct ArrowTraits<int8_t>
{
  using Type = arrow::Int8Type;
  static constexpr auto type() { return arrow::int8(); }
};

template<>
struct ArrowTraits<double>
{
  using Type = arrow::DoubleType;
  static constexpr auto type() { return arrow::float64(); }
};

}  // namespace gtopt
