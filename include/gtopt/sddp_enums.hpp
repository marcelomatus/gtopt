/**
 * @file      sddp_enums.hpp
 * @brief     Named enum types for SDDP and boundary-cut options
 * @date      2026-03-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by SddpOptions and MonolithicOptions
 * (boundary cuts).  Extracted from sddp_options.hpp so that the
 * struct definition stays focused on the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── BoundaryCutsMode ───────────────────────────────────────────────────────

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

inline constexpr auto boundary_cuts_mode_entries =
    std::to_array<EnumEntry<BoundaryCutsMode>>({
        {.name = "noload", .value = BoundaryCutsMode::noload},
        {.name = "separated", .value = BoundaryCutsMode::separated},
        {.name = "combined", .value = BoundaryCutsMode::combined},
    });

[[nodiscard]] constexpr auto enum_entries(BoundaryCutsMode /*tag*/) noexcept
{
  return std::span {boundary_cuts_mode_entries};
}

// ─── CutSharingMode ────────────────────────────────────────────────────────

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
  expected = 1,  ///< Average cuts within each scene, then sum across scenes
  accumulate = 2,  ///< Sum all cuts directly (LP objectives pre-weighted)
  max = 3,  ///< All cuts from all scenes added to all scenes
};

inline constexpr auto cut_sharing_mode_entries =
    std::to_array<EnumEntry<CutSharingMode>>({
        {.name = "none", .value = CutSharingMode::none},
        {.name = "expected", .value = CutSharingMode::expected},
        {.name = "accumulate", .value = CutSharingMode::accumulate},
        {.name = "max", .value = CutSharingMode::max},
    });

[[nodiscard]] constexpr auto enum_entries(CutSharingMode /*tag*/) noexcept
{
  return std::span {cut_sharing_mode_entries};
}

// ─── ElasticFilterMode ──────────────────────────────────────────────────────

/**
 * @brief How the elastic filter handles feasibility issues in the backward
 * pass.
 *
 * - `single_cut`: Build a single Benders feasibility cut.
 * - `multi_cut`: Build a Benders cut + per-slack bound cuts.
 * - `backpropagate`: Update source bounds to elastic trial values (PLP).
 * - `chinneck` (default): Run a Chinneck-style elastic IIS filter —
 *   identify the irreducible infeasible subset of fixed state-variable
 *   bounds, then emit per-IIS-bound multi-cuts plus a tightened
 *   Benders cut whose reduced costs come from the IIS-restricted
 *   clone.  More LP solves per fcut event than `multi_cut`, but the
 *   cuts forbid only the true infeasibility-causing region (Chinneck,
 *   *Feasibility and Infeasibility in Optimization*, 2008, §3.5; PLP
 *   `osi_lp_get_feasible_cut`).  Falls back to the full elastic
 *   result when the IIS re-fix step cannot confirm a smaller subset,
 *   so behaviour is no worse than `multi_cut` in the worst case.
 */
enum class ElasticFilterMode : uint8_t
{
  single_cut = 0,  ///< Build a single Benders feasibility cut
  multi_cut = 1,  ///< Build a Benders cut + per-slack bound cuts
  backpropagate = 2,  ///< Update source bounds to elastic trial values (PLP)
  chinneck = 3,  ///< Build cuts only on the Chinneck IIS (default)
};

/// Includes "cut" as a backward-compatible alias for "single_cut",
/// and "iis" as an alias for "chinneck".
inline constexpr auto elastic_filter_mode_entries =
    std::to_array<EnumEntry<ElasticFilterMode>>({
        {.name = "single_cut", .value = ElasticFilterMode::single_cut},
        {.name = "cut",
         .value = ElasticFilterMode::single_cut,
         .is_alias = true},
        {.name = "multi_cut", .value = ElasticFilterMode::multi_cut},
        {.name = "backpropagate", .value = ElasticFilterMode::backpropagate},
        {.name = "chinneck", .value = ElasticFilterMode::chinneck},
        {.name = "iis", .value = ElasticFilterMode::chinneck, .is_alias = true},
    });

[[nodiscard]] constexpr auto enum_entries(ElasticFilterMode /*tag*/) noexcept
{
  return std::span {elastic_filter_mode_entries};
}

// ─── HotStartMode ──────────────────────────────────────────────────────────

/**
 * @brief How the SDDP solver handles hot-start and the output cut file.
 *
 * Controls both whether to load cuts from a previous run and how to
 * handle the combined output file (`sddp_cuts.csv`) at the end of
 * the solve.
 */
enum class HotStartMode : uint8_t
{
  none = 0,  ///< Cold start — no cuts loaded (default)
  keep = 1,  ///< Hot-start; keep original output file unchanged
  append = 2,  ///< Hot-start; append new cuts to original file
  replace = 3,  ///< Hot-start; replace original file with all cuts
};

inline constexpr auto hot_start_mode_entries =
    std::to_array<EnumEntry<HotStartMode>>({
        {.name = "none", .value = HotStartMode::none},
        {.name = "keep", .value = HotStartMode::keep},
        {.name = "append", .value = HotStartMode::append},
        {.name = "replace", .value = HotStartMode::replace},
    });

[[nodiscard]] constexpr auto enum_entries(HotStartMode /*tag*/) noexcept
{
  return std::span {hot_start_mode_entries};
}

// ─── RecoveryMode ───────────────────────────────────────────────────────────

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

inline constexpr auto recovery_mode_entries =
    std::to_array<EnumEntry<RecoveryMode>>({
        {.name = "none", .value = RecoveryMode::none},
        {.name = "cuts", .value = RecoveryMode::cuts},
        {.name = "full", .value = RecoveryMode::full},
    });

[[nodiscard]] constexpr auto enum_entries(RecoveryMode /*tag*/) noexcept
{
  return std::span {recovery_mode_entries};
}

// ─── MissingCutVarMode ─────────────────────────────────────────────────────

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

inline constexpr auto missing_cut_var_mode_entries =
    std::to_array<EnumEntry<MissingCutVarMode>>({
        {.name = "skip_coeff", .value = MissingCutVarMode::skip_coeff},
        {.name = "skip_cut", .value = MissingCutVarMode::skip_cut},
    });

[[nodiscard]] constexpr auto enum_entries(MissingCutVarMode /*tag*/) noexcept
{
  return std::span {missing_cut_var_mode_entries};
}

// ─── ConvergenceMode ───────────────────────────────────────────────────────

/**
 * @brief SDDP convergence criterion selection.
 *
 * All modes respect `min_iterations` and `max_iterations`.
 *
 * - `gap_only`:        Converge only when the deterministic gap closes.
 * - `gap_stationary`:  Also declare convergence when the gap stops
 *                      improving.
 * - `statistical`:     Full PLP-style criterion (default).  Adds a
 *                      confidence-interval test for multi-scene problems.
 */
enum class ConvergenceMode : uint8_t
{
  gap_only = 0,  ///< Deterministic gap test only
  gap_stationary = 1,  ///< Gap + stationary gap detection
  statistical = 2,  ///< Gap + stationary + CI (default, PLP-style)
};

inline constexpr auto convergence_mode_entries =
    std::to_array<EnumEntry<ConvergenceMode>>({
        {.name = "gap_only", .value = ConvergenceMode::gap_only},
        {.name = "gap_stationary", .value = ConvergenceMode::gap_stationary},
        {.name = "statistical", .value = ConvergenceMode::statistical},
    });

[[nodiscard]] constexpr auto enum_entries(ConvergenceMode /*tag*/) noexcept
{
  return std::span {convergence_mode_entries};
}

// ─── StateVariableLookupMode ───────────────────────────────────────────────

/**
 * @brief How update_lp elements obtain reservoir/battery volume between phases.
 *
 * - `warm_start` (default): volume comes from the previous iteration's
 *   warm-start solution, a recovered state file, or the element's vini.
 * - `cross_phase`: volume is taken from the previous phase's efin within
 *   the same forward pass.
 */
enum class StateVariableLookupMode : uint8_t
{
  warm_start =
      0,  ///< Warm solution / recovery / vini (default, no cross-phase)
  cross_phase = 1,  ///< Previous phase's efin within the same forward pass
};

inline constexpr auto state_variable_lookup_mode_entries =
    std::to_array<EnumEntry<StateVariableLookupMode>>({
        {.name = "warm_start", .value = StateVariableLookupMode::warm_start},
        {.name = "cross_phase", .value = StateVariableLookupMode::cross_phase},
    });

[[nodiscard]] constexpr auto enum_entries(
    StateVariableLookupMode /*tag*/) noexcept
{
  return std::span {state_variable_lookup_mode_entries};
}

// ─── LowMemoryMode ──────────────────────────────────────────────────────

/**
 * @brief Low-memory mode for SDDP solver.
 *
 * Controls whether and how the solver releases backend memory between solves.
 *
 * - `off`:      Disabled — keep solver backend loaded (default).
 * - `compress`: Release solver backend after each solve; keep a (optionally
 *               compressed) FlatLinearProblem snapshot + dynamic columns +
 *               accumulated cuts.  Reconstructed on demand.  Set
 *               `memory_codec = uncompressed` to retain the flat LP raw
 *               (previously the dedicated `snapshot` mode).
 * - `rebuild`:  Re-flatten the LP from element collections on every solve.
 *               No FlatLinearProblem snapshot is held; only the small
 *               persistent SDDP state (dynamic α columns and accumulated
 *               Benders cuts) survives across solves.  Lowest steady-state
 *               memory; highest CPU cost.  Skips the initial up-front
 *               build loop entirely — each (scene, phase) LP is built
 *               lazily inside the same task that solves or clones it.
 */
enum class LowMemoryMode : uint8_t
{
  off = 0,  ///< Disabled — keep solver backend loaded (default)
  compress = 2,  ///< Release solver, keep (optionally compressed) flat LP
  rebuild = 3,  ///< Re-flatten LP from collections on every solve, no snapshot
};

inline constexpr auto low_memory_mode_entries =
    std::to_array<EnumEntry<LowMemoryMode>>({
        {.name = "off", .value = LowMemoryMode::off},
        {.name = "compress", .value = LowMemoryMode::compress},
        {.name = "rebuild", .value = LowMemoryMode::rebuild},
        // Back-compat alias: "snapshot" parses to `compress`.  Callers
        // that want the old snapshot semantics (uncompressed flat LP)
        // set `memory_codec = uncompressed` explicitly.
        {.name = "snapshot",
         .value = LowMemoryMode::compress,
         .is_alias = true},
    });

[[nodiscard]] constexpr auto enum_entries(LowMemoryMode /*tag*/) noexcept
{
  return std::span {low_memory_mode_entries};
}

// ─── CutIOFormat ────────────────────────────────────────────────────────────

/**
 * @brief File format for SDDP cut and state variable I/O.
 *
 * Controls how Benders cuts and state columns are serialized for
 * hot-start, cut transfer, and cascade state persistence.
 *
 * - `csv`:  CSV with structured keys (class:var:uid=coeff).  Backward
 *           compatible with legacy name-based CSV on the load side.
 * - `json`: Compact JSON via daw::json.  Faster parsing, fully structured,
 *           no LP column name dependency.
 */
enum class CutIOFormat : uint8_t
{
  csv = 0,  ///< CSV with structured keys (default)
  json = 1,  ///< Compact JSON via daw::json
};

inline constexpr auto cut_io_format_entries =
    std::to_array<EnumEntry<CutIOFormat>>({
        {.name = "csv", .value = CutIOFormat::csv},
        {.name = "json", .value = CutIOFormat::json},
    });

[[nodiscard]] constexpr auto enum_entries(CutIOFormat /*tag*/) noexcept
{
  return std::span {cut_io_format_entries};
}

}  // namespace gtopt
