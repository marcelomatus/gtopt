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
 * @brief Compression codec for file I/O and in-memory compression.
 *
 * Used for both output files (Parquet/CSV) and in-memory LP snapshots
 * (low_memory compress mode).  For in-memory use, `auto_select` picks
 * the fastest available codec at runtime (lz4 > snappy > zstd > gzip).
 */
enum class CompressionCodec : uint8_t
{
  uncompressed = 0,  ///< No compression
  gzip = 1,  ///< gzip compression
  zstd = 2,  ///< Zstandard compression (default for file I/O)
  lz4 = 3,  ///< LZ4 compression (default for in-memory)
  bzip2 = 4,  ///< bzip2 compression (file I/O only)
  xz = 5,  ///< xz/LZMA compression (file I/O only)
  snappy = 6,  ///< Snappy compression
  brotli = 7,  ///< Brotli compression (Arrow/Parquet only)
  lzo = 8,  ///< LZO compression (Arrow/Parquet only)
  auto_select = 9,  ///< Pick fastest available (in-memory only)
};

inline constexpr auto compression_codec_entries =
    std::to_array<EnumEntry<CompressionCodec>>({
        {.name = "uncompressed", .value = CompressionCodec::uncompressed},
        {.name = "none", .value = CompressionCodec::uncompressed},
        {.name = "gzip", .value = CompressionCodec::gzip},
        {.name = "zstd", .value = CompressionCodec::zstd},
        {.name = "lz4", .value = CompressionCodec::lz4},
        {.name = "bzip2", .value = CompressionCodec::bzip2},
        {.name = "xz", .value = CompressionCodec::xz},
        {.name = "snappy", .value = CompressionCodec::snappy},
        {.name = "brotli", .value = CompressionCodec::brotli},
        {.name = "lzo", .value = CompressionCodec::lzo},
        {.name = "auto", .value = CompressionCodec::auto_select},
    });

constexpr auto enum_entries(CompressionCodec /*tag*/) noexcept
{
  return std::span {compression_codec_entries};
}

// --- BoundaryCutsValuation ---------------------------------------------------

/**
 * @brief Valuation basis for boundary (future-cost) cut coefficients and RHS.
 *
 * Determines whether the effective discount factor of the last stage
 * (combining the annual discount rate and the stage discount_factor) is
 * applied when loading boundary cuts into the LP.
 *
 * - `end_of_horizon`:  Cuts are valued at the end of the last stage.
 *                       No discount factor is applied (default).
 * - `present_value`:   Cuts are expressed in present-value terms.
 *                       The effective discount factor of the last stage
 *                       is applied to the RHS and coefficients so that
 *                       the future cost is discounted back to the
 *                       planning horizon's reference time.
 */
enum class BoundaryCutsValuation : uint8_t
{
  end_of_horizon = 0,  ///< No discounting; cuts valued at last stage (default)
  present_value = 1,  ///< Apply last-stage effective discount factor
};

inline constexpr auto boundary_cuts_valuation_entries =
    std::to_array<EnumEntry<BoundaryCutsValuation>>({
        {
            .name = "end_of_horizon",
            .value = BoundaryCutsValuation::end_of_horizon,
        },
        {
            .name = "present_value",
            .value = BoundaryCutsValuation::present_value,
        },
    });

constexpr auto enum_entries(BoundaryCutsValuation /*tag*/) noexcept
{
  return std::span {boundary_cuts_valuation_entries};
}

// --- ProbabilityRescaleMode --------------------------------------------------

/**
 * @brief Controls when scenario/scene probabilities are rescaled to sum 1.0.
 *
 * Scenario `probability_factor` values within each scene should sum to 1.0.
 * When they do not, a warning is always emitted.  This option controls
 * whether (and when) the solver normalizes the values automatically.
 *
 * - `none`:     No rescaling.  Warn about mismatched probabilities but
 *               leave values unchanged.  This may produce incorrect
 *               expected-cost computations if probabilities do not sum
 *               to 1.0.
 * - `build`:    Rescale at build time (during validation, before LP
 *               construction).  Probabilities are normalized to sum 1.0
 *               per scene and across all scenes.
 * - `runtime`:  Rescale at build time AND at runtime.  In addition to
 *               build-time normalization, scene weights are re-normalized
 *               during SDDP when a scene becomes infeasible, so the
 *               remaining feasible scenes' probabilities still sum to 1.0.
 *               This is the default.
 */
enum class ProbabilityRescaleMode : uint8_t
{
  none = 0,  ///< No rescaling, warn only
  build = 1,  ///< Rescale at build/validation time
  runtime = 2,  ///< Rescale at build time and at runtime (default)
};

inline constexpr auto probability_rescale_mode_entries =
    std::to_array<EnumEntry<ProbabilityRescaleMode>>({
        {.name = "none", .value = ProbabilityRescaleMode::none},
        {.name = "build", .value = ProbabilityRescaleMode::build},
        {.name = "runtime", .value = ProbabilityRescaleMode::runtime},
    });

constexpr auto enum_entries(ProbabilityRescaleMode /*tag*/) noexcept
{
  return std::span {probability_rescale_mode_entries};
}

// --- KappaWarningMode -------------------------------------------------------

/**
 * @brief Controls warnings when the LP condition number (kappa) exceeds a
 * threshold.
 *
 * After each LP solve (forward, backward, aperture, and monolithic), the
 * solver checks the basis condition number.  When kappa exceeds
 * `kappa_threshold` (default 1e9), this option controls the response.
 *
 * - `none`:     No kappa checking or warnings.
 * - `warn`:     Log a warning with scene, phase, and iteration (default).
 * - `save_lp`:  Log a warning AND save the culprit LP file for debugging.
 * - `diagnose`: Like `save_lp`, but also analyze cut rows to identify
 *               which Benders cuts have the worst coefficient ratios.
 */
enum class KappaWarningMode : uint8_t
{
  none = 0,  ///< No kappa checking
  warn = 1,  ///< Log a warning when kappa exceeds threshold (default)
  save_lp = 2,  ///< Warn and save the LP file
  diagnose = 3,  ///< Warn, save LP, and analyze cut coefficient ranges
};

inline constexpr auto kappa_warning_mode_entries =
    std::to_array<EnumEntry<KappaWarningMode>>({
        {.name = "none", .value = KappaWarningMode::none},
        {.name = "warn", .value = KappaWarningMode::warn},
        {.name = "save_lp", .value = KappaWarningMode::save_lp},
        {.name = "diagnose", .value = KappaWarningMode::diagnose},
    });

constexpr auto enum_entries(KappaWarningMode /*tag*/) noexcept
{
  return std::span {kappa_warning_mode_entries};
}

// --- ConstraintMode ----------------------------------------------------------

/**
 * @brief Controls error handling and verbosity for user constraint resolution.
 *
 * During LP construction, user constraint expressions reference LP columns
 * (variables), data parameters, and named user parameters.  When a reference
 * cannot be resolved (unknown element, attribute, or parameter name), this
 * option controls whether the solver logs a warning and skips the term
 * or raises a fatal error.
 *
 * - `normal`:  Log warnings for unresolved references and skip the affected
 *              terms.  Suitable for development and exploratory modeling.
 * - `strict`:  Treat any unresolved reference as a fatal error (default).
 *              Ensures no constraint terms are silently dropped.
 * - `debug`:   Like `strict`, but additionally logs detailed info about
 *              every resolved variable, parameter, and constraint row
 *              added to the LP.  Useful for verifying constraint assembly.
 */
enum class ConstraintMode : uint8_t
{
  normal = 0,  ///< Warn and skip unresolved references
  strict = 1,  ///< Fail on any unresolved reference (default)
  debug = 2,  ///< Strict + verbose logging of every resolved term
};

inline constexpr auto constraint_mode_entries =
    std::to_array<EnumEntry<ConstraintMode>>({
        {.name = "normal", .value = ConstraintMode::normal},
        {.name = "strict", .value = ConstraintMode::strict},
        {.name = "debug", .value = ConstraintMode::debug},
    });

constexpr auto enum_entries(ConstraintMode /*tag*/) noexcept
{
  return std::span {constraint_mode_entries};
}

}  // namespace gtopt
