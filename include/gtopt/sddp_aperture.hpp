/**
 * @file      sddp_aperture.hpp
 * @brief     Aperture backward-pass logic for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp into standalone free functions following
 * the same pattern as benders_cut.hpp and sddp_cut_io.hpp.  Each function
 * takes explicit parameters instead of accessing class members, making
 * them independently testable and reusable.
 *
 * ## Free functions
 *
 * - `build_effective_apertures()` – deduplicate aperture UIDs with counts
 * - `build_synthetic_apertures()` – create apertures from first N scenarios
 * - `solve_apertures_for_phase()`  – clone LP per aperture, solve, build
 *    the probability-weighted expected Benders cut
 */

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

// Forward declarations
class ScenarioLP;
class SystemLP;
class PhaseLP;
struct PhaseStateInfo;

// ─── Effective aperture entry ───────────────────────────────────────────────

/// A deduplicated aperture reference with a repetition count.
///
/// When the per-phase aperture_set contains duplicates (e.g. [1,2,3,3,3]),
/// each unique aperture is solved only once but its weight is scaled by
/// @p count (the number of occurrences).
struct ApertureEntry
{
  std::reference_wrapper<const Aperture> aperture;
  int count {};
};

// ─── Effective aperture list builder ────────────────────────────────────────

/// Build the effective (deduplicated) aperture list for a single phase.
///
/// When @p phase_apertures is empty, all active apertures from
/// @p aperture_defs are used (each with count = 1).
/// When @p phase_apertures is non-empty, UIDs are counted for duplicates
/// and mapped to their definitions; order of first appearance is preserved
/// for deterministic results.
///
/// @param aperture_defs   All aperture definitions from the simulation
/// @param phase_apertures Per-phase aperture UID set (may be empty)
/// @return Deduplicated aperture entries with repetition counts
[[nodiscard]] auto build_effective_apertures(
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures) -> std::vector<ApertureEntry>;

// ─── Synthetic aperture builder ─────────────────────────────────────────────

/// Build synthetic aperture definitions from the first N scenarios.
///
/// Creates one aperture per scenario with equal probability (1/N).
/// Used when no explicit aperture_array is provided in the simulation
/// and the solver falls back to the legacy num_apertures-based behaviour.
///
/// @param all_scenarios   All scenario LP objects
/// @param n_apertures     Number of apertures to create (capped at
///                        all_scenarios.size())
/// @return Array of synthetic Aperture objects
[[nodiscard]] auto build_synthetic_apertures(
    std::span<const ScenarioLP> all_scenarios, int n_apertures)
    -> Array<Aperture>;

// ─── Resolve callback type ──────────────────────────────────────────────────

/// Callback for resolving a cloned LP.
///
/// The SDDP solver passes resolve_clone_via_pool wrapped as this callback
/// so that solve_apertures_for_phase remains a free function without pool
/// dependencies.  Returns the solver status code on success, or an error.
using ApertureResolveFunc = std::function<std::expected<int, Error>(
    LinearInterface& clone, const SolverOptions& opts, PhaseIndex phase)>;

// ─── Core aperture solver ───────────────────────────────────────────────────

/// Solve all apertures for a single phase and return the expected cut.
///
/// For each effective aperture: clones the phase LP, updates flow column
/// bounds to the aperture's source scenario, solves the clone, and builds
/// a Benders cut from the reduced costs.  The probability-weighted average
/// of all feasible aperture cuts is returned as the expected cut.
///
/// Returns std::nullopt if all apertures are infeasible or skipped.
///
/// @param scene            Scene index (for logging/labelling)
/// @param phase            Target phase being solved
/// @param src_state        Phase state of the source (previous) phase
/// @param base_scenario    The scene's base scenario (for flow bound update)
/// @param all_scenarios    All simulation scenarios (for aperture lookup)
/// @param aperture_defs    Aperture definitions to use
/// @param phase_apertures  Per-phase aperture UID set (may be empty)
/// @param total_cuts       Running cut count (for label uniqueness)
/// @param sys              SystemLP for the (scene, phase) pair
/// @param phase_lp         PhaseLP for the target phase
/// @param opts             Solver options
/// @param label_maker      Label maker for LP row names
/// @param log_directory    Directory for debug LP files (empty = no save)
/// @param scene_uid        Scene UID integer (for logging)
/// @param phase_uid        Phase UID integer (for logging)
/// @param resolve_fn       Callback to resolve a cloned LP
[[nodiscard]] auto solve_apertures_for_phase(
    SceneIndex scene,
    PhaseIndex phase,
    const PhaseStateInfo& src_state,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures,
    int total_cuts,
    SystemLP& sys,
    const PhaseLP& phase_lp,
    const SolverOptions& opts,
    const LabelMaker& label_maker,
    const std::string& log_directory,
    int scene_uid,
    int phase_uid,
    const ApertureResolveFunc& resolve_fn) -> std::optional<SparseRow>;

}  // namespace gtopt
