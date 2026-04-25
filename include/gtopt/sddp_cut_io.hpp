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

/// Extract the iteration field from an SDDP cut row name.
///
/// Format (the type-tag determines whether the iteration field is
/// present):
///   sddp_scut_{uid}_{scene}_{phase}_{iteration}_{offset}  → field [5]
///   sddp_fcut_{uid}_{scene}_{phase}_{iteration}_{offset}  → field [5]
///   sddp_bcut_{uid}_{scene}_{phase}_{iteration}_{offset}  → field [5]
///   sddp_ecut_{scene}_{phase}_{total_cuts}                → no iter
///
/// The on-disk ``{iteration}`` field is a 0-based ``IterationIndex``
/// (matching the runtime loop counter) — NOT the 1-based
/// ``IterationUid`` that LP-label contexts carry.  Callers feeding
/// this back into ``make_iteration_context`` must convert via
/// ``uid_of(extract_iteration_from_name(...))``.  Keeping the disk
/// format 0-based preserves backward compat with existing golden
/// files; switching the format to UID-based is a separate invasive
/// change that must update all serialised cut files.
///
/// Returns ``IterationIndex {0}`` if the iteration cannot be
/// determined (unknown row-name shape, missing field, parse error).
[[nodiscard]] auto extract_iteration_from_name(std::string_view name)
    -> IterationIndex;

/// Build a scene UID -> SceneIndex lookup from a SimulationLP.
/// Uses flat_map for cache-friendly sorted lookup.
[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>;

// ─── Scale helpers ─────────────────────────────────────────────────────────

/// Compute the effective scale_alpha: if the option is > 0 use it,
/// otherwise auto-compute as max(var_scale) across all state variables.
[[nodiscard]] auto effective_scale_alpha(const PlanningLP& planning_lp,
                                         double option_scale_alpha) -> double;

// ─── Save functions ─────────────────────────────────────────────────────────

/// Save accumulated cuts to a CSV file for hot-start.
///
/// Both RHS and coefficients are stored in fully physical space so that
/// cuts are portable across runs with different scale_objective or
/// variable scaling configurations.
///
/// @param cuts         All stored cuts to save
/// @param planning_lp  The PlanningLP (for scale and column names)
/// @param filepath     Output CSV file path
/// @param append_mode  When true, append rows without header
///                     (file must already exist with correct header)
[[nodiscard]] auto save_cuts_csv(std::span<const StoredCut> cuts,
                                 const PlanningLP& planning_lp,
                                 const std::string& filepath,
                                 bool append_mode = false)
    -> std::expected<void, Error>;

/// Save cuts for a single scene to a per-scene CSV file.
///
/// @param cuts             The scene's stored cuts
/// @param scene_index      Scene index (for column name lookup)
/// @param scene_uid        Scene UID (for file naming)
/// @param planning_lp      The PlanningLP (for scale and column names)
/// @param directory        Output directory (file: scene_<UID>.csv)
[[nodiscard]] auto save_scene_cuts_csv(std::span<const StoredCut> cuts,
                                       SceneIndex scene_index,
                                       SceneUid scene_uid,
                                       const PlanningLP& planning_lp,
                                       const std::string& directory)
    -> std::expected<void, Error>;

// ─── Load functions ─────────────────────────────────────────────────────────

/// Load cuts from a CSV file and add to all scenes' phase LPs.
///
/// Cuts are broadcast to all scenes regardless of originating scene,
/// since loaded cuts serve as warm-start approximations for the entire
/// problem (analogous to PLP's cut sharing across scenarios).
///
/// Supports three coefficient formats:
///   - `class:var:uid=coeff`  (structured key — preferred, no LP names)
///   - `name=coeff`           (legacy name-based — resolve by column name)
///   - `index:coeff`          (legacy index-based — validate column index)
///
/// @param planning_lp        The PlanningLP to add cuts to
/// @param filepath           Input CSV file path
/// @param scale_alpha        Scale for alpha variable
/// @param label_maker        Label maker for LP row names
/// @param scene_phase_states Unused (kept for API compatibility).
/// @return CutLoadResult with count and max iteration, or an error
[[nodiscard]] auto load_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr) -> std::expected<CutLoadResult, Error>;

/// Load all per-scene cut files from a directory.
///
/// Files matching `scene_<N>.csv` and `sddp_cuts.csv` are loaded.
/// Files with the `error_` prefix (from infeasible scenes in a
/// previous run) are skipped to prevent loading invalid cuts.
///
/// @param planning_lp        The PlanningLP to add cuts to
/// @param directory          Directory containing cut CSV files
/// @param scale_alpha        Actual scale_alpha (computed from state var
/// scales)
/// @param label_maker        Label maker for LP row names
/// @param scene_phase_states Unused (kept for API compatibility).
/// @return CutLoadResult with total count and max iteration, or an error
[[nodiscard]] auto load_scene_cuts_from_directory(
    PlanningLP& planning_lp,
    const std::string& directory,
    double scale_alpha,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr) -> std::expected<CutLoadResult, Error>;

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

/// Load named-variable cuts from a CSV file with a `phase` column.
///
/// Unlike boundary cuts (which load into the last phase only), these
/// cuts include a `phase` column indicating which phase they belong to.
/// The solver resolves named state-variable headers in each specified
/// phase and adds the cuts to the corresponding phase LP.
///
/// @param planning_lp         The PlanningLP to add cuts to
/// @param filepath            Input CSV file path
/// @param options             SDDP options (for alpha bounds)
/// @param label_maker         Label maker for LP row names
/// @param scene_phase_states  Per-scene phase state (for alpha columns)
/// @return CutLoadResult with count and max iteration, or an error
[[nodiscard]] auto load_named_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<CutLoadResult, Error>;

// ─── JSON save/load functions ───────────────────────────────────────────────

/// Save accumulated cuts to a JSON file using compact daw::json.
///
/// Coefficients use structured keys (class:var:uid).
/// Fully portable — no LP column names required.
///
/// @param cuts         All stored cuts to save
/// @param planning_lp  The PlanningLP (for scale and state variable map)
/// @param filepath     Output JSON file path
[[nodiscard]] auto save_cuts_json(std::span<const StoredCut> cuts,
                                  const PlanningLP& planning_lp,
                                  const std::string& filepath)
    -> std::expected<void, Error>;

/// Save cuts for a single scene to a per-scene JSON file.
[[nodiscard]] auto save_scene_cuts_json(std::span<const StoredCut> cuts,
                                        SceneIndex scene_index,
                                        SceneUid scene_uid,
                                        const PlanningLP& planning_lp,
                                        const std::string& directory)
    -> std::expected<void, Error>;

/// Load cuts from a JSON file and add to all scenes' phase LPs.
///
/// @param planning_lp        The PlanningLP to add cuts to
/// @param filepath           Input JSON file path
/// @param scale_alpha        Scale for alpha variable
/// @param scene_phase_states Unused (kept for API compatibility).
/// @return CutLoadResult with count and max iteration, or an error
[[nodiscard]] auto load_cuts_json(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr) -> std::expected<CutLoadResult, Error>;

// ─── Format-dispatching functions ───────────────────────────────────────────

/// Save cuts using the specified format (csv or json).
[[nodiscard]] auto save_cuts(std::span<const StoredCut> cuts,
                             const PlanningLP& planning_lp,
                             const std::string& filepath,
                             CutIOFormat format,
                             bool append_mode = false)
    -> std::expected<void, Error>;

/// Load cuts trying the preferred format first, falling back to the other.
///
/// If the preferred-format file does not exist, the other format is tried.
/// This allows seamless migration between CSV and JSON cut files.
[[nodiscard]] auto load_cuts(
    PlanningLP& planning_lp,
    const std::string& filepath,
    double scale_alpha,
    CutIOFormat format,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states = nullptr) -> std::expected<CutLoadResult, Error>;

}  // namespace gtopt

// Backwards compatibility: state I/O was extracted to its own header.
#include <gtopt/sddp_state_io.hpp>
