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

// ─── MissingCutVarMode ───────────────────────────────────────────────────────

/**
 * @brief How to handle boundary/named cut rows that reference state variables
 *        not present in the current model.
 *
 * - `skip_coeff`: Drop the missing variable's coefficient from the cut but
 *                 still load the cut (default).
 * - `skip_cut`:   Skip the entire cut if any missing variable has a non-zero
 *                 coefficient.
 */
enum class MissingCutVarMode : uint8_t
{
  skip_coeff = 0,  ///< Drop the coefficient, load the cut
  skip_cut = 1,  ///< Skip the entire cut
};

/// Name-value table for MissingCutVarMode
inline constexpr auto missing_cut_var_mode_entries =
    std::to_array<EnumEntry<MissingCutVarMode>>({
        {.name = "skip_coeff", .value = MissingCutVarMode::skip_coeff},
        {.name = "skip_cut", .value = MissingCutVarMode::skip_cut},
    });

/// Parse a MissingCutVarMode from a string
[[nodiscard]] constexpr auto missing_cut_var_mode_from_name(
    std::string_view name) noexcept -> std::optional<MissingCutVarMode>
{
  return enum_from_name(std::span {missing_cut_var_mode_entries}, name);
}

/// Return the canonical name of a MissingCutVarMode
[[nodiscard]] constexpr auto missing_cut_var_mode_name(
    MissingCutVarMode value) noexcept -> std::string_view
{
  return enum_name(std::span {missing_cut_var_mode_entries}, value);
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

// ─── CutCoeffMode ───────────────────────────────────────────────────────────

/**
 * @brief How Benders cut coefficients are extracted from solved subproblems.
 *
 * - `reduced_cost` (default): Uses reduced costs of the dependent (fixed)
 *   columns.  The dependent columns are pinned via lo == hi bounds; their
 *   reduced costs equal the shadow price of the implicit fixing constraint.
 *   This is the standard approach used by SDDP.jl and most modern SDDP
 *   implementations.
 *
 * - `row_dual`: Adds explicit equality constraint rows to fix each
 *   dependent column (`x_dep = trial_value`) and reads the row duals of
 *   those coupling constraints.  This is the approach used by PLP (Fortran),
 *   which reads `GetDual(lp, PDLDAcFilaInd(IColAcop))`.  Mathematically
 *   equivalent to reduced_cost, but may differ numerically depending on
 *   the LP solver's presolve and dual reporting behaviour.
 */
enum class CutCoeffMode : uint8_t
{
  reduced_cost = 0,  ///< Reduced costs of fixed dependent columns (default)
  row_dual = 1,  ///< Row duals of explicit coupling constraint rows (PLP)
};

/// Name-value table for CutCoeffMode
inline constexpr auto cut_coeff_mode_entries =
    std::to_array<EnumEntry<CutCoeffMode>>({
        {.name = "reduced_cost", .value = CutCoeffMode::reduced_cost},
        {.name = "row_dual", .value = CutCoeffMode::row_dual},
    });

/// Parse a CutCoeffMode from a string ("reduced_cost", "row_dual")
[[nodiscard]] constexpr auto cut_coeff_mode_from_name(
    std::string_view name) noexcept -> std::optional<CutCoeffMode>
{
  return enum_from_name(std::span {cut_coeff_mode_entries}, name);
}

/// Return the canonical name of a CutCoeffMode
[[nodiscard]] constexpr auto cut_coeff_mode_name(CutCoeffMode value) noexcept
    -> std::string_view
{
  return enum_name(std::span {cut_coeff_mode_entries}, value);
}

// ─── ConvergenceMode ────────────────────────────────────────────────────────

/**
 * @brief SDDP convergence criterion selection.
 *
 * All modes respect `min_iterations` and `max_iterations`.  The primary
 * gap test (`gap < convergence_tol`) is always active regardless of mode.
 *
 * - `gap_only`:        Converge only when the deterministic gap closes.
 *                      Simplest mode; no stationarity or CI checks.
 *
 * - `gap_stationary`:  Also declare convergence when the gap stops
 *                      improving (gap_change < stationary_tol over
 *                      stationary_window iterations).  Handles the
 *                      non-zero-gap case for deterministic problems.
 *
 * - `statistical`:     Full PLP-style criterion (default).  Adds a
 *                      confidence-interval test for multi-scene problems:
 *                      UB − LB ≤ z_{α/2} · σ.  When the CI test alone
 *                      fails but the gap is stationary, convergence is
 *                      still declared (non-zero gap that stopped
 *                      improving).  Degrades gracefully to gap_stationary
 *                      when only one scene is present (no apertures or
 *                      pure Benders fallback).
 */
enum class ConvergenceMode : uint8_t
{
  gap_only = 0,  ///< Deterministic gap test only
  gap_stationary = 1,  ///< Gap + stationary gap detection
  statistical = 2,  ///< Gap + stationary + CI (default, PLP-style)
};

/// Name-value table for ConvergenceMode
inline constexpr auto convergence_mode_entries =
    std::to_array<EnumEntry<ConvergenceMode>>({
        {.name = "gap_only", .value = ConvergenceMode::gap_only},
        {.name = "gap_stationary", .value = ConvergenceMode::gap_stationary},
        {.name = "statistical", .value = ConvergenceMode::statistical},
    });

/// Parse a ConvergenceMode from a string
[[nodiscard]] constexpr auto convergence_mode_from_name(
    std::string_view name) noexcept -> std::optional<ConvergenceMode>
{
  return enum_from_name(std::span {convergence_mode_entries}, name);
}

/// Return the canonical name of a ConvergenceMode
[[nodiscard]] constexpr auto convergence_mode_name(
    ConvergenceMode value) noexcept -> std::string_view
{
  return enum_name(std::span {convergence_mode_entries}, value);
}

// ─── EnergyScaleMode ────────────────────────────────────────────────────────

/**
 * @brief How the LP energy-variable scaling factor is determined for storage
 *        elements (reservoirs, batteries).
 *
 * - `manual` (0): Use the explicit `energy_scale` field (default 1.0 if
 *   unset).  This is the legacy behaviour.
 * - `auto_scale` (1, default): Compute `energy_scale = max(1.0, emax/1000)`
 *   so that LP variables stay in the O(1000) range regardless of physical
 *   reservoir size.  Mirrors PLP's `ScaleVol(i) = max(1, Vmax/1000)`.
 *   An explicit `energy_scale` field on the element overrides auto.
 */
enum class EnergyScaleMode : uint8_t
{
  manual = 0,  ///< Use explicit energy_scale field (legacy, 1.0 default)
  auto_scale = 1,  ///< Compute from emax: max(1.0, emax / 1000) (default)
};

/// Name-value table for EnergyScaleMode
inline constexpr auto energy_scale_mode_entries =
    std::to_array<EnumEntry<EnergyScaleMode>>({
        {.name = "manual", .value = EnergyScaleMode::manual},
        {.name = "auto", .value = EnergyScaleMode::auto_scale},
    });

/// Parse an EnergyScaleMode from a string ("manual", "auto")
[[nodiscard]] constexpr auto energy_scale_mode_from_name(
    std::string_view name) noexcept -> std::optional<EnergyScaleMode>
{
  return enum_from_name(std::span {energy_scale_mode_entries}, name);
}

/// Return the canonical name of an EnergyScaleMode
[[nodiscard]] constexpr auto energy_scale_mode_name(
    EnergyScaleMode value) noexcept -> std::string_view
{
  return enum_name(std::span {energy_scale_mode_entries}, value);
}

// ─── StateVariableLookupMode ─────────────────────────────────────────────────

/**
 * @brief How update_lp elements obtain reservoir/battery volume between phases.
 *
 * Controls the fallback chain in StorageLP::physical_eini when the current
 * phase has not yet been solved.  This affects only the nonlinear LP
 * coefficient updates (seepage, production factor, discharge limit) — it
 * does NOT affect SDDP state-variable chaining, cut generation, or the
 * convergence criterion.
 *
 * - `warm_start` (default): volume comes from the previous iteration's
 *   warm-start solution, a recovered state file, or the element's vini.
 *   No cross-phase lookup is performed.
 * - `cross_phase`: volume is taken from the previous phase's efin within
 *   the same forward pass (classic cross-phase chaining).
 */
enum class StateVariableLookupMode : uint8_t
{
  warm_start =
      0,  ///< Warm solution / recovery / vini (default, no cross-phase)
  cross_phase = 1,  ///< Previous phase's efin within the same forward pass
};

/// Name-value table for StateVariableLookupMode
inline constexpr auto state_variable_lookup_mode_entries =
    std::to_array<EnumEntry<StateVariableLookupMode>>({
        {.name = "warm_start", .value = StateVariableLookupMode::warm_start},
        {.name = "cross_phase", .value = StateVariableLookupMode::cross_phase},
    });

/// Parse a StateVariableLookupMode from a string
[[nodiscard]] constexpr auto state_variable_lookup_mode_from_name(
    std::string_view name) noexcept -> std::optional<StateVariableLookupMode>
{
  return enum_from_name(std::span {state_variable_lookup_mode_entries}, name);
}

/// Return the canonical name of a StateVariableLookupMode
[[nodiscard]] constexpr auto state_variable_lookup_mode_name(
    StateVariableLookupMode value) noexcept -> std::string_view
{
  return enum_name(std::span {state_variable_lookup_mode_entries}, value);
}

}  // namespace gtopt
