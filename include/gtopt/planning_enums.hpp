/**
 * @file      planning_enums.hpp
 * @brief     Named enum types for planning options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by PlanningOptions.  Extracted from
 * planning_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- MethodType -------------------------------------------------------------

/**
 * @brief Top-level solver selection: monolithic LP, SDDP, or cascade.
 */
enum class MethodType : uint8_t
{
  monolithic = 0,  ///< Single monolithic LP/MIP (default)
  sddp = 1,  ///< Stochastic Dual Dynamic Programming decomposition
  cascade = 2,  ///< Cascade: Benders -> guided SDDP -> free SDDP
};

inline constexpr auto method_type_entries =
    std::to_array<EnumEntry<MethodType>>({
        {.name = "monolithic", .value = MethodType::monolithic},
        {.name = "sddp", .value = MethodType::sddp},
        {.name = "cascade", .value = MethodType::cascade},
    });

constexpr auto enum_entries(MethodType /*tag*/) noexcept
{
  return std::span {method_type_entries};
}

// --- DataFormat -------------------------------------------------------------

/**
 * @brief File format for input/output data files.
 */
enum class DataFormat : uint8_t
{
  parquet = 0,  ///< Apache Parquet columnar format (default)
  csv = 1,  ///< Comma-separated values
};

inline constexpr auto data_format_entries =
    std::to_array<EnumEntry<DataFormat>>({
        {.name = "parquet", .value = DataFormat::parquet},
        {.name = "csv", .value = DataFormat::csv},
    });

constexpr auto enum_entries(DataFormat /*tag*/) noexcept
{
  return std::span {data_format_entries};
}

// --- CompressionCodec -------------------------------------------------------

/**
 * @brief Compression codec for output files (Parquet / CSV).
 */
enum class CompressionCodec : uint8_t
{
  uncompressed = 0,  ///< No compression
  gzip = 1,  ///< gzip compression
  zstd = 2,  ///< Zstandard compression (default)
  lz4 = 3,  ///< LZ4 compression
  bzip2 = 4,  ///< bzip2 compression
  xz = 5,  ///< xz/LZMA compression
  snappy = 6,  ///< Snappy compression (Arrow/Parquet)
  brotli = 7,  ///< Brotli compression (Arrow/Parquet)
  lzo = 8,  ///< LZO compression (Arrow/Parquet)
};

inline constexpr auto compression_codec_entries =
    std::to_array<EnumEntry<CompressionCodec>>({
        {.name = "uncompressed", .value = CompressionCodec::uncompressed},
        {.name = "gzip", .value = CompressionCodec::gzip},
        {.name = "zstd", .value = CompressionCodec::zstd},
        {.name = "lz4", .value = CompressionCodec::lz4},
        {.name = "bzip2", .value = CompressionCodec::bzip2},
        {.name = "xz", .value = CompressionCodec::xz},
        {.name = "snappy", .value = CompressionCodec::snappy},
        {.name = "brotli", .value = CompressionCodec::brotli},
        {.name = "lzo", .value = CompressionCodec::lzo},
    });

constexpr auto enum_entries(CompressionCodec /*tag*/) noexcept
{
  return std::span {compression_codec_entries};
}

}  // namespace gtopt
