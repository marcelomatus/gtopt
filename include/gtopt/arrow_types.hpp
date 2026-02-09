#pragma once

#ifdef HAVE_ARROW
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
#else
// Stub types when Arrow is not available
namespace gtopt
{
using ArrowIndex = int64_t;

// Stub types that don't have the actual Arrow implementation
struct ArrowField {};
struct ArrowArray {};
struct ArrowTable {};
struct ArrowChunkedArray {};
struct ArrowColumnResult {};

template<typename Uid>
struct ArrowTraits {};

template<>
struct ArrowTraits<int>
{
  // Provide dummy Type for compilation with distinct type_id
  struct Type { static constexpr int type_id = 1; static constexpr const char* type_name() { return "int32"; } };
  static constexpr auto type() { return nullptr; }
};

template<>
struct ArrowTraits<int16_t>
{
  struct Type { static constexpr int type_id = 2; static constexpr const char* type_name() { return "int16"; } };
  static constexpr auto type() { return nullptr; }
};

template<>
struct ArrowTraits<int8_t>
{
  struct Type { static constexpr int type_id = 3; static constexpr const char* type_name() { return "int8"; } };
  static constexpr auto type() { return nullptr; }
};

template<>
struct ArrowTraits<double>
{
  struct Type { static constexpr int type_id = 4; static constexpr const char* type_name() { return "float64"; } };
  static constexpr auto type() { return nullptr; }
};
}
#endif
