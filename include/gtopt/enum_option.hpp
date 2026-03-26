/**
 * @file      enum_option.hpp
 * @brief     Generic compile-time enum-to-string and string-to-enum framework
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a reusable pattern for mapping enum values to string names and
 * back.  Each enum defines a `std::to_array<EnumEntry<E>>` table; the two
 * free-function templates perform linear lookup in that table.
 *
 * The pattern mirrors LPAlgo in solver_options.hpp but generalises it to
 * any enum type via templates and `std::span`.
 *
 * ### Usage
 * ```cpp
 * enum class Colour : uint8_t { red, green, blue };
 * inline constexpr auto colour_entries = std::to_array<EnumEntry<Colour>>({
 *     {.name = "red",   .value = Colour::red},
 *     {.name = "green", .value = Colour::green},
 *     {.name = "blue",  .value = Colour::blue},
 * });
 * // Look up by name:
 * auto c = enum_from_name(std::span{colour_entries}, "green");
 * // Look up by value:
 * auto n = enum_name(std::span{colour_entries}, Colour::blue);
 * ```
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gtopt
{

/**
 * @brief Name-value pair for an enumerator.
 *
 * With C++26 static reflection (P2996 `std::meta::enumerators_of`) this
 * table could be generated automatically.  Until then it is maintained
 * manually next to each enum definition.
 */
template<typename E>
struct EnumEntry
{
  std::string_view name;
  E value;
};

/**
 * @brief Look up an enumerator by its canonical name.
 *
 * @tparam E  Enum type.
 * @tparam N  Table size (deduced from the span extent).
 * @param entries  Compile-time table of name-value pairs.
 * @param name     Case-sensitive name to search for.
 * @return The matching enumerator, or @c std::nullopt if not found.
 */
template<typename E, std::size_t N>
[[nodiscard]] constexpr auto enum_from_name(
    std::span<const EnumEntry<E>, N> entries, std::string_view name) noexcept
    -> std::optional<E>
{
  const auto it = std::ranges::find_if(
      entries, [name](const EnumEntry<E>& e) { return e.name == name; });
  if (it != entries.end()) {
    return it->value;
  }
  return std::nullopt;
}

/**
 * @brief Return the canonical name of an enumerator.
 *
 * @tparam E  Enum type.
 * @tparam N  Table size (deduced from the span extent).
 * @param entries  Compile-time table of name-value pairs.
 * @param value    The enumerator to look up.
 * @return The name string, or @c "unknown" for out-of-range values.
 */
template<typename E, std::size_t N>
[[nodiscard]] constexpr auto enum_name(std::span<const EnumEntry<E>, N> entries,
                                       E value) noexcept -> std::string_view
{
  const auto it = std::ranges::find_if(
      entries, [value](const EnumEntry<E>& e) { return e.value == value; });
  return it != entries.end() ? it->name : "unknown";
}

// ─── MethodType ──────────────────────────────────────────────────────────────

/**
 * @brief Top-level solver selection: monolithic LP, SDDP, or cascade.
 */
enum class MethodType : uint8_t
{
  monolithic = 0,  ///< Single monolithic LP/MIP (default)
  sddp = 1,  ///< Stochastic Dual Dynamic Programming decomposition
  cascade = 2,  ///< Cascade: Benders → guided SDDP → free SDDP
};

/// Name-value table for MethodType
inline constexpr auto method_type_entries =
    std::to_array<EnumEntry<MethodType>>({
        {.name = "monolithic", .value = MethodType::monolithic},
        {.name = "sddp", .value = MethodType::sddp},
        {.name = "cascade", .value = MethodType::cascade},
    });

/// Parse a MethodType from a string ("monolithic", "sddp", "cascade")
[[nodiscard]] constexpr auto method_type_from_name(
    std::string_view name) noexcept -> std::optional<MethodType>
{
  return enum_from_name(std::span {method_type_entries}, name);
}

/// Return the canonical name of a MethodType
[[nodiscard]] constexpr auto method_type_name(MethodType value) noexcept
    -> std::string_view
{
  return enum_name(std::span {method_type_entries}, value);
}

// ─── SolveMode ───────────────────────────────────────────────────────────────

/**
 * @brief Monolithic solver execution mode.
 */
enum class SolveMode : uint8_t
{
  monolithic = 0,  ///< Solve all phases in a single LP (default)
  sequential = 1,  ///< Solve phases sequentially
};

/// Name-value table for SolveMode
inline constexpr auto solve_mode_entries = std::to_array<EnumEntry<SolveMode>>({
    {.name = "monolithic", .value = SolveMode::monolithic},
    {.name = "sequential", .value = SolveMode::sequential},
});

/// Parse a SolveMode from a string ("monolithic", "sequential")
[[nodiscard]] constexpr auto solve_mode_from_name(
    std::string_view name) noexcept -> std::optional<SolveMode>
{
  return enum_from_name(std::span {solve_mode_entries}, name);
}

/// Return the canonical name of a SolveMode
[[nodiscard]] constexpr auto solve_mode_name(SolveMode value) noexcept
    -> std::string_view
{
  return enum_name(std::span {solve_mode_entries}, value);
}

// ─── BoundaryCutsMode ────────────────────────────────────────────────────────

/**
 * @brief How boundary (future-cost) cuts are loaded for the last phase.
 *
 * - `noload`:    Do not load boundary cuts even if a file is given.
 * - `separated`: Load cuts per scene (scene UID matching; default).
 * - `combined`:  Broadcast all cuts into all scenes.
 */
enum class BoundaryCutsMode : uint8_t
{
  noload = 0,  ///< Skip loading boundary cuts
  separated = 1,  ///< Per-scene cut assignment (default)
  combined = 2,  ///< Broadcast all cuts to all scenes
};

/// Name-value table for BoundaryCutsMode
inline constexpr auto boundary_cuts_mode_entries =
    std::to_array<EnumEntry<BoundaryCutsMode>>({
        {.name = "noload", .value = BoundaryCutsMode::noload},
        {.name = "separated", .value = BoundaryCutsMode::separated},
        {.name = "combined", .value = BoundaryCutsMode::combined},
    });

/// Parse a BoundaryCutsMode from a string ("noload", "separated", "combined")
[[nodiscard]] constexpr auto boundary_cuts_mode_from_name(
    std::string_view name) noexcept -> std::optional<BoundaryCutsMode>
{
  return enum_from_name(std::span {boundary_cuts_mode_entries}, name);
}

/// Return the canonical name of a BoundaryCutsMode
[[nodiscard]] constexpr auto boundary_cuts_mode_name(
    BoundaryCutsMode value) noexcept -> std::string_view
{
  return enum_name(std::span {boundary_cuts_mode_entries}, value);
}

// ─── DataFormat ──────────────────────────────────────────────────────────────

/**
 * @brief File format for input/output data files.
 */
enum class DataFormat : uint8_t
{
  parquet = 0,  ///< Apache Parquet columnar format (default)
  csv = 1,  ///< Comma-separated values
};

/// Name-value table for DataFormat
inline constexpr auto data_format_entries =
    std::to_array<EnumEntry<DataFormat>>({
        {.name = "parquet", .value = DataFormat::parquet},
        {.name = "csv", .value = DataFormat::csv},
    });

/// Parse a DataFormat from a string ("parquet", "csv")
[[nodiscard]] constexpr auto data_format_from_name(
    std::string_view name) noexcept -> std::optional<DataFormat>
{
  return enum_from_name(std::span {data_format_entries}, name);
}

/// Return the canonical name of a DataFormat
[[nodiscard]] constexpr auto data_format_name(DataFormat value) noexcept
    -> std::string_view
{
  return enum_name(std::span {data_format_entries}, value);
}

// ─── CompressionCodec ────────────────────────────────────────────────────────

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

/// Name-value table for CompressionCodec
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

/// Parse a CompressionCodec from a string
[[nodiscard]] constexpr auto compression_codec_from_name(
    std::string_view name) noexcept -> std::optional<CompressionCodec>
{
  return enum_from_name(std::span {compression_codec_entries}, name);
}

/// Return the canonical name of a CompressionCodec
[[nodiscard]] constexpr auto compression_codec_name(
    CompressionCodec value) noexcept -> std::string_view
{
  return enum_name(std::span {compression_codec_entries}, value);
}

// ─── CutSharingMode ─────────────────────────────────────────────────────────

/**
 * @brief SDDP cut sharing mode across scenes in the backward pass.
 *
 * When a sharing mode other than `none` is selected, the backward pass is
 * synchronized per-phase: all scenes complete a phase before cuts are shared
 * and the next phase is processed.
 */
enum class CutSharingMode : uint8_t
{
  none = 0,  ///< No sharing; scenes solved independently (default)
  expected = 1,  ///< Probability-weighted average cut shared to all scenes
  accumulate = 2,  ///< Sum all cuts directly (LP objectives pre-weighted)
  max = 3,  ///< All cuts from all scenes added to all scenes
};

/// Name-value table for CutSharingMode
inline constexpr auto cut_sharing_mode_entries =
    std::to_array<EnumEntry<CutSharingMode>>({
        {.name = "none", .value = CutSharingMode::none},
        {.name = "expected", .value = CutSharingMode::expected},
        {.name = "accumulate", .value = CutSharingMode::accumulate},
        {.name = "max", .value = CutSharingMode::max},
    });

/// Parse a CutSharingMode from a string
/// ("none", "expected", "accumulate", "max")
[[nodiscard]] constexpr auto cut_sharing_mode_from_name(
    std::string_view name) noexcept -> std::optional<CutSharingMode>
{
  return enum_from_name(std::span {cut_sharing_mode_entries}, name);
}

/// Return the canonical name of a CutSharingMode
[[nodiscard]] constexpr auto cut_sharing_mode_name(
    CutSharingMode value) noexcept -> std::string_view
{
  return enum_name(std::span {cut_sharing_mode_entries}, value);
}

// ─── ElasticFilterMode ───────────────────────────────────────────────────────

/**
 * @brief How the elastic filter handles feasibility issues in the backward
 * pass.
 *
 * - `single_cut` (default): Build a single Benders feasibility cut.
 * - `multi_cut`: Build a Benders cut + per-slack bound cuts.
 * - `backpropagate`: Update source bounds to elastic trial values (PLP).
 */
enum class ElasticFilterMode : uint8_t
{
  single_cut = 0,  ///< Build a single Benders feasibility cut (default)
  multi_cut = 1,  ///< Build a Benders cut + per-slack bound cuts
  backpropagate = 2,  ///< Update source bounds to elastic trial values (PLP)
};

/// Name-value table for ElasticFilterMode.
/// Includes "cut" as a backward-compatible alias for "single_cut".
inline constexpr auto elastic_filter_mode_entries =
    std::to_array<EnumEntry<ElasticFilterMode>>({
        {.name = "single_cut", .value = ElasticFilterMode::single_cut},
        {.name = "cut", .value = ElasticFilterMode::single_cut},
        {.name = "multi_cut", .value = ElasticFilterMode::multi_cut},
        {.name = "backpropagate", .value = ElasticFilterMode::backpropagate},
    });

/// Parse an ElasticFilterMode from a string.
/// Accepts "single_cut" / "cut" (= single_cut), "multi_cut",
/// "backpropagate".
[[nodiscard]] constexpr auto elastic_filter_mode_from_name(
    std::string_view name) noexcept -> std::optional<ElasticFilterMode>
{
  return enum_from_name(std::span {elastic_filter_mode_entries}, name);
}

/// Return the canonical name of an ElasticFilterMode
[[nodiscard]] constexpr auto elastic_filter_mode_name(
    ElasticFilterMode value) noexcept -> std::string_view
{
  return enum_name(std::span {elastic_filter_mode_entries}, value);
}

// ─── HotStartMode ───────────────────────────────────────────────────────────

/**
 * @brief How the SDDP solver handles hot-start and the output cut file.
 *
 * Controls both whether to load cuts from a previous run and how to
 * handle the combined output file (`sddp_cuts.csv`) at the end of
 * the solve.
 *
 * - `none`:    Cold start — no cuts loaded.  Write the combined file
 *              normally on completion.
 * - `keep`:    Load cuts from the previous run.  Do NOT modify the
 *              original combined file on completion (only per-iteration
 *              versioned files are written).
 * - `append`:  Load cuts from the previous run.  Append newly generated
 *              cuts to the existing combined file on completion.
 * - `replace`: Load cuts from the previous run.  Replace the combined
 *              file with all cuts (loaded + new) on completion.
 */
enum class HotStartMode : uint8_t
{
  none = 0,  ///< Cold start — no cuts loaded (default)
  keep = 1,  ///< Hot-start; keep original output file unchanged
  append = 2,  ///< Hot-start; append new cuts to original file
  replace = 3,  ///< Hot-start; replace original file with all cuts
};

/// Name-value table for HotStartMode
inline constexpr auto cut_recovery_mode_entries =
    std::to_array<EnumEntry<HotStartMode>>({
        {.name = "none", .value = HotStartMode::none},
        {.name = "keep", .value = HotStartMode::keep},
        {.name = "append", .value = HotStartMode::append},
        {.name = "replace", .value = HotStartMode::replace},
    });

/// Parse a HotStartMode from a string
/// ("none", "keep", "append", "replace")
[[nodiscard]] constexpr auto cut_recovery_mode_from_name(
    std::string_view name) noexcept -> std::optional<HotStartMode>
{
  return enum_from_name(std::span {cut_recovery_mode_entries}, name);
}

/// Return the canonical name of a HotStartMode
[[nodiscard]] constexpr auto cut_recovery_mode_name(HotStartMode value) noexcept
    -> std::string_view
{
  return enum_name(std::span {cut_recovery_mode_entries}, value);
}

// ── RecoveryMode
// ──────────────────────────────────────────────────────────────

/**
 * @brief Controls what is recovered from a previous SDDP run.
 *
 * - `none`:  No recovery — cold start.
 * - `cuts`:  Recover only Benders cuts from previous run.
 * - `full`:  Recover Benders cuts AND state variable solutions (default).
 */
enum class RecoveryMode : uint8_t
{
  none = 0,  ///< No recovery — cold start
  cuts = 1,  ///< Recover only Benders cuts
  full = 2,  ///< Recover cuts + state variable solutions (default)
};

/// Name-value table for RecoveryMode
inline constexpr auto recovery_mode_entries =
    std::to_array<EnumEntry<RecoveryMode>>({
        {.name = "none", .value = RecoveryMode::none},
        {.name = "cuts", .value = RecoveryMode::cuts},
        {.name = "full", .value = RecoveryMode::full},
    });

/// Parse a RecoveryMode from a string ("none", "cuts", "full")
[[nodiscard]] constexpr auto recovery_mode_from_name(
    std::string_view name) noexcept -> std::optional<RecoveryMode>
{
  return enum_from_name(std::span {recovery_mode_entries}, name);
}

/// Return the canonical name of a RecoveryMode
[[nodiscard]] constexpr auto recovery_mode_name(RecoveryMode value) noexcept
    -> std::string_view
{
  return enum_name(std::span {recovery_mode_entries}, value);
}

// ─── LpNamesLevel ────────────────────────────────────────────────────────────

/**
 * @brief LP variable/constraint naming level for matrix assembly.
 *
 * Controls how much naming information is generated during LP construction.
 * Higher levels provide better diagnostics but consume more memory.
 *
 * - `minimal`:      State-variable column names only (for internal use,
 *                   e.g. cascade solver state transfer).  Smallest footprint.
 * - `only_cols`:    All column names + name-to-index maps.
 * - `cols_and_rows`: Column + row names + maps.  Warns on duplicate names.
 */
enum class LpNamesLevel : uint8_t
{
  minimal = 0,  ///< State-variable column names only (default)
  only_cols = 1,  ///< All column names + name maps
  cols_and_rows = 2,  ///< Column + row names + maps + warn on duplicates
};

/// Name-value table for LpNamesLevel
inline constexpr auto lp_names_level_entries =
    std::to_array<EnumEntry<LpNamesLevel>>({
        {.name = "minimal", .value = LpNamesLevel::minimal},
        {.name = "only_cols", .value = LpNamesLevel::only_cols},
        {.name = "cols_and_rows", .value = LpNamesLevel::cols_and_rows},
    });

/// Parse an LpNamesLevel from a string
/// ("minimal", "only_cols", "cols_and_rows")
[[nodiscard]] constexpr auto lp_names_level_from_name(
    std::string_view name) noexcept -> std::optional<LpNamesLevel>
{
  return enum_from_name(std::span {lp_names_level_entries}, name);
}

/// Return the canonical name of an LpNamesLevel
[[nodiscard]] constexpr auto lp_names_level_name(LpNamesLevel value) noexcept
    -> std::string_view
{
  return enum_name(std::span {lp_names_level_entries}, value);
}

// ─── StatePropagation ────────────────────────────────────────────────────────

/**
 * @brief Controls how SDDP forward-pass state variables propagate between
 * phases.
 *
 * - `last_iteration`: state vars take values from the previous iteration's
 *   warm-start, recovered file, or default (vini).  The previous phase's
 *   solution within the same iteration is NOT used (default).
 * - `inter_phase`: state vars are pinned to the previous phase's solution
 *   within the same forward pass (classic chaining).
 */
enum class StatePropagation : uint8_t
{
  last_iteration = 0,  ///< Previous iteration / recovery / vini (default)
  inter_phase = 1,  ///< Pin to previous phase's solution
};

/// Name-value table for StatePropagation
inline constexpr auto state_propagation_entries =
    std::to_array<EnumEntry<StatePropagation>>({
        {.name = "last_iteration", .value = StatePropagation::last_iteration},
        {.name = "inter_phase", .value = StatePropagation::inter_phase},
    });

/// Parse a StatePropagation from a string ("last_iteration", "inter_phase")
[[nodiscard]] constexpr auto state_propagation_from_name(
    std::string_view name) noexcept -> std::optional<StatePropagation>
{
  return enum_from_name(std::span {state_propagation_entries}, name);
}

/// Return the canonical name of a StatePropagation
[[nodiscard]] constexpr auto state_propagation_name(
    StatePropagation value) noexcept -> std::string_view
{
  return enum_name(std::span {state_propagation_entries}, value);
}

}  // namespace gtopt
