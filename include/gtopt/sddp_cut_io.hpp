/**
 * @file      sddp_cut_io.hpp
 * @brief     Cut persistence (save/load) for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp into standalone free functions following
 * the same pattern as benders_cut.hpp.  Each function takes explicit
 * parameters instead of accessing class members, making them independently
 * testable and reusable.
 */

#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sddp_method.hpp>

namespace gtopt
{

/// Strip trailing carriage-return left by std::getline on DOS text files.
inline void strip_cr(std::string& s) noexcept
{
  if (!s.empty() && s.back() == '\r') {
    s.pop_back();
  }
}

// Forward declarations
class PlanningLP;

// ─── UID lookup helpers ─────────────────────────────────────────────────────

/// Build a phase UID -> PhaseIndex lookup from a SimulationLP.
/// Uses flat_map for cache-friendly sorted lookup.
[[nodiscard]] auto build_phase_uid_map(const PlanningLP& planning_lp)
    -> flat_map<PhaseUid, PhaseIndex>;

// ``extract_iteration_from_name`` was removed in 2026-05.  Every
// consumer now reads the iteration index directly from the matching
// struct field (``StoredCut::iteration_index``,
// ``RawBoundaryCut::iteration_index``).  See the documentation block
// in ``source/sddp_cut_io.cpp`` for the migration notes.

/// Canonical state-variable column name that may appear as a cut
/// coefficient column in boundary / hot-start cut CSV headers.
///
/// Mirrors the value of ``StorageLP<...>::EfinName`` (the LP
/// assembly emits this name from ``StorageLP::add_to_lp`` for
/// reservoir / battery / lng-terminal final energy).  We define a
/// local mirror — instead of reaching ``StorageLP::EfinName``
/// directly — because ``StorageLP`` is a class template and its
/// static members aren't reachable without an instantiation.
///
/// Defined as a named constexpr constant — never spelled inline as a
/// string literal — so a regression that breaks the allow-list shows
/// up at the type/symbol level, not as a silent string mismatch.
inline constexpr std::string_view EfinColName {"efin"};

/// Returns true if @p col_name is a final-state column name that may
/// appear in boundary / hot-start cut CSV headers.
///
/// Used by the cut CSV loader to decide whether a header column is a
/// state variable (and thus a candidate cut coefficient column) or
/// metadata (rhs / scene / phase / iteration).
///
/// Exposed for unit testing: the predicate is the source of truth for
/// the cut-file column allow-list, and a regression here silently
/// drops state coefficients on load.
[[nodiscard]] constexpr auto is_final_state_col(
    std::string_view col_name) noexcept -> bool
{
  return col_name == EfinColName;
}

/// Build a scene UID -> SceneIndex lookup from a SimulationLP.
/// Uses flat_map for cache-friendly sorted lookup.
[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>;

// ─── Scale helpers ─────────────────────────────────────────────────────────

/// Compute the effective scale_alpha: if the option is > 0 use it,
/// otherwise auto-compute as max(var_scale) across all state variables.
[[nodiscard]] auto effective_scale_alpha(const PlanningLP& planning_lp,
                                         double option_scale_alpha) -> double;

// ─── Boundary / named-cut CSV loaders ──────────────────────────────────────
//
// The combined SDDP cut path (save_cuts / load_cuts) is now Parquet-only —
// see `save_cuts_parquet` and `load_cuts_parquet` below.  The legacy CSV
// save/load functions were removed in the Phase 1.3 cleanup; the only
// remaining CSV readers are for **externally-produced** boundary and
// named-cut files (e.g. PLP-generated input).

/// Load boundary (future-cost) cuts from a named-variable CSV file.
///
/// The CSV header names the state variables (e.g. reservoir or battery
/// names); subsequent rows provide {name, [iteration,] scene, rhs,
/// coefficients}.  Cuts are added only to the last phase, with an
/// alpha column created if needed.  Analogous to PLP's "planos de
/// embalse".
///
/// @param planning_lp         The PlanningLP to add cuts to
/// @param filepath            Input CSV file path
/// @param options             SDDP options (boundary mode, max iters)
/// @param label_maker         Label maker for LP row names
/// @param scene_phase_states  Per-scene phase state (for alpha columns)
/// @return CutLoadResult with count and max iteration, or an error
[[nodiscard]] auto load_boundary_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<CutLoadResult, Error>;

// ``load_named_cuts_csv`` was retired in 2026-05.  "Named hot-start
// cuts" are gtopt's own internal format and now travel via the
// Parquet writer / loader (``save_cuts_parquet`` /
// ``load_cuts_parquet``) only — the same path every other internal
// SDDP cut uses.  Boundary cuts (PLP-imported "planos de embalse")
// remain CSV-compatible via ``load_boundary_cuts_csv`` above; that
// is the only CSV cut path left.

// ─── Parquet save/load functions ────────────────────────────────────────────
//
// Parquet schema v3 (2026-05):
//   {type:int8, phase:int32, scene:int32, iteration:int32, extra:int32,
//    rhs:float64, dual:float64?, coeffs:list<struct<key:utf8, val:float64>>}
// File-level KeyValueMetadata: {version: "3", scale_objective: "<.17g>"}
//
// Cut identity is the structured :class:`CutKey` 5-tuple
// {type, scene_uid, phase_uid, iteration_index, extra}; the legacy
// ``name: utf8`` column was dropped in 2026-05 along with the JSON
// writer and ``extract_iteration_from_name`` parser.  LP row labels
// (``LabelMaker::make_row_label``) are still emitted for
// CoinLpIO / debug-dump consumers but live only on the live LP, never
// in the on-disk cut file.
//
// `append_mode = true` writes a sibling file with a unique suffix
// (`<stem>.append-<stamp>.parquet`) in the same directory.  The loader
// globs all sibling parquet files to reconstruct the full cut set, since
// Parquet has no row-level append primitive.

/// Save accumulated cuts to a Parquet file with typed schema.
[[nodiscard]] auto save_cuts_parquet(std::span<const StoredCut> cuts,
                                     const PlanningLP& planning_lp,
                                     const std::string& filepath,
                                     bool append_mode = false)
    -> std::expected<void, Error>;

/// Save cuts for a single scene to a per-scene Parquet file.
[[nodiscard]] auto save_scene_cuts_parquet(std::span<const StoredCut> cuts,
                                           SceneIndex scene_index,
                                           SceneUid scene_uid,
                                           const PlanningLP& planning_lp,
                                           const std::string& directory)
    -> std::expected<void, Error>;

/// Load cuts from a Parquet file (plus any sibling `*.append-*.parquet`
/// files) and add to all scenes' phase LPs.  Coefficient keys must be
/// structured `class:var:uid`.
[[nodiscard]] auto load_cuts_parquet(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr,
    /// When non-null, every loaded cut is also pushed into this
    /// :class:`SDDPCutManager` via ``store_cut`` so the manager's
    /// per-scene vectors stay authoritative for inherited + generated
    /// cuts.  Without this, ``SDDPCutManager::forget_first_cuts(N)``
    /// would walk a store that doesn't know about the loaded cuts and
    /// would delete the wrong rows from the LP — root cause of the
    /// cascade ``forget_first_cuts`` + ``low_memory=compress`` crash
    /// observed on juan/IPLP when ``inherit_optimality_cuts > 0``.
    SDDPCutManager* cut_store = nullptr) -> std::expected<CutLoadResult, Error>;

/// Load all per-scene Parquet cut files from a directory.
///
/// Files matching `scene_<N>.parquet` and the combined
/// `sddp_cuts.parquet` are loaded.  Files with the `error_` prefix
/// (from infeasible scenes in a previous run) are skipped.
[[nodiscard]] auto load_scene_cuts_from_directory_parquet(
    PlanningLP& planning_lp,
    const std::string& directory,
    double scale_alpha,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr) -> std::expected<CutLoadResult, Error>;

}  // namespace gtopt

// `sddp_state_io.hpp` was removed (2026-05-14) along with its
// `save_state_csv` writer — no consumer in the codebase reads the
// resulting state CSV; policy state is reconstructed from the
// versioned cut files at recovery.
