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
inline auto GetColumn(const ArrowTable& table, std::string_view name)
    -> ArrowColumnResult
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
  static auto type() { return arrow::int32(); }
};

template<>
struct ArrowTraits<int16_t>
{
  using Type = arrow::Int32Type;
  static auto type() { return arrow::int32(); }
};

template<>
struct ArrowTraits<int8_t>
{
  using Type = arrow::Int32Type;
  static auto type() { return arrow::int32(); }
};

template<>
struct ArrowTraits<double>
{
  using Type = arrow::DoubleType;
  static auto type() { return arrow::float64(); }
};

/**
 * @brief Check if an Arrow type is a compatible integer type that can be
 *        cast to int32
 * @param type_id The Arrow type id to check
 * @return true if the type is int8, int16, int32, or int64
 */
constexpr auto is_compatible_int32_type(arrow::Type::type type_id) -> bool
{
  return type_id == arrow::Type::INT8 || type_id == arrow::Type::INT16
      || type_id == arrow::Type::INT32 || type_id == arrow::Type::INT64;
}

/**
 * @brief Check if an Arrow type is a compatible floating-point type that can
 *        be cast to double
 * @param type_id The Arrow type id to check
 * @return true if the type is float or double
 */
constexpr auto is_compatible_double_type(arrow::Type::type type_id) -> bool
{
  return type_id == arrow::Type::FLOAT || type_id == arrow::Type::DOUBLE;
}

/**
 * @brief Cast an Arrow array chunk to Int32Array, handling int8/int16/int64
 *        conversion
 * @param chunk The Arrow array chunk to cast
 * @return A shared pointer to an Int32Array, or nullptr if the type is
 *         incompatible
 */
namespace detail
{

template<typename SourceArrayType>
inline auto widen_to_int32_array(const arrow::Array& chunk)
    -> std::shared_ptr<arrow::Int32Array>
{
  const auto& src = static_cast<const SourceArrayType&>(chunk);  // NOLINT
  arrow::Int32Builder builder;
  auto st = builder.Reserve(chunk.length());
  if (!st.ok()) {
    return nullptr;
  }
  for (int64_t i = 0; i < chunk.length(); ++i) {
    if (src.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(static_cast<int32_t>(src.Value(i)));
    }
  }
  std::shared_ptr<arrow::Array> result;
  st = builder.Finish(&result);
  if (!st.ok()) {
    return nullptr;
  }
  return std::static_pointer_cast<arrow::Int32Array>(result);
}

template<typename SourceArrayType>
inline auto widen_to_double_array(const arrow::Array& chunk)
    -> std::shared_ptr<arrow::DoubleArray>
{
  const auto& src = static_cast<const SourceArrayType&>(chunk);  // NOLINT
  arrow::DoubleBuilder builder;
  auto st = builder.Reserve(chunk.length());
  if (!st.ok()) {
    return nullptr;
  }
  for (int64_t i = 0; i < chunk.length(); ++i) {
    if (src.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(static_cast<double>(src.Value(i)));
    }
  }
  std::shared_ptr<arrow::Array> result;
  st = builder.Finish(&result);
  if (!st.ok()) {
    return nullptr;
  }
  return std::static_pointer_cast<arrow::DoubleArray>(result);
}

}  // namespace detail

inline auto cast_to_int32_array(const std::shared_ptr<arrow::Array>& chunk)
    -> std::shared_ptr<arrow::Int32Array>
{
  if (!chunk) {
    return nullptr;
  }

  const auto tid = chunk->type_id();

  if (tid == arrow::Type::INT32) {
    return std::static_pointer_cast<arrow::Int32Array>(chunk);
  }

  if (tid == arrow::Type::INT16) {
    return detail::widen_to_int32_array<arrow::Int16Array>(*chunk);
  }

  if (tid == arrow::Type::INT8) {
    return detail::widen_to_int32_array<arrow::Int8Array>(*chunk);
  }

  if (tid == arrow::Type::INT64) {
    return detail::widen_to_int32_array<arrow::Int64Array>(*chunk);
  }

  return nullptr;
}

/**
 * @brief Cast an Arrow array chunk to DoubleArray, handling float32 widening
 * @param chunk The Arrow array chunk to cast
 * @return A shared pointer to a DoubleArray, or nullptr if the type is
 *         incompatible
 */
inline auto cast_to_double_array(const std::shared_ptr<arrow::Array>& chunk)
    -> std::shared_ptr<arrow::DoubleArray>
{
  if (!chunk) {
    return nullptr;
  }

  const auto tid = chunk->type_id();

  if (tid == arrow::Type::DOUBLE) {
    return std::static_pointer_cast<arrow::DoubleArray>(chunk);
  }

  if (tid == arrow::Type::FLOAT) {
    return detail::widen_to_double_array<arrow::FloatArray>(*chunk);
  }

  return nullptr;
}

}  // namespace gtopt
