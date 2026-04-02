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

/// Build a scene UID -> SceneIndex lookup from a SimulationLP.
/// Uses flat_map for cache-friendly sorted lookup.
[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>;

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
/// @param scene            Scene index (for column name lookup)
/// @param scene_uid_val    Scene UID value (for file naming)
/// @param planning_lp      The PlanningLP (for scale and column names)
/// @param directory        Output directory (file: scene_<UID>.csv)
[[nodiscard]] auto save_scene_cuts_csv(std::span<const StoredCut> cuts,
                                       SceneIndex scene,
                                       int scene_uid_val,
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
/// @param planning_lp   The PlanningLP to add cuts to
/// @param filepath      Input CSV file path
/// @param label_maker   Label maker for LP row names
/// @return CutLoadResult with count and max iteration, or an error
[[nodiscard]] auto load_cuts_csv(PlanningLP& planning_lp,
                                 const std::string& filepath,
                                 const LabelMaker& label_maker)
    -> std::expected<CutLoadResult, Error>;

/// Load all per-scene cut files from a directory.
///
/// Files matching `scene_<N>.csv` and `sddp_cuts.csv` are loaded.
/// Files with the `error_` prefix (from infeasible scenes in a
/// previous run) are skipped to prevent loading invalid cuts.
///
/// @param planning_lp   The PlanningLP to add cuts to
/// @param directory     Directory containing cut CSV files
/// @param label_maker   Label maker for LP row names
/// @return CutLoadResult with total count and max iteration, or an error
[[nodiscard]] auto load_scene_cuts_from_directory(PlanningLP& planning_lp,
                                                  const std::string& directory,
                                                  const LabelMaker& label_maker)
    -> std::expected<CutLoadResult, Error>;

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
    StrongIndexVector<SceneIndex,
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
    StrongIndexVector<SceneIndex,
                      StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
        scene_phase_states) -> std::expected<CutLoadResult, Error>;

}  // namespace gtopt

// Backwards compatibility: state I/O was extracted to its own header.
#include <gtopt/sddp_state_io.hpp>
