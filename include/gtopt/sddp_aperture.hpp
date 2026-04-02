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

#include <concepts>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

// ─── Aperture element concepts ──────────────────────────────────────────────

/// Value-provider signature: (StageUid, BlockUid) -> std::optional<double>.
/// Returns the aperture value for the given stage/block, or std::nullopt
/// if the value is unavailable (keeps the forward-pass value unchanged).
using ApertureValueFn =
    std::function<std::optional<double>(StageUid, BlockUid)>;

/// An element that can update its LP for an aperture scenario using a
/// generic value provider.
template<typename T>
concept HasUpdateAperture = requires(const T& e,
                                     LinearInterface& li,
                                     const ScenarioLP& base,
                                     const ApertureValueFn& value_fn,
                                     const StageLP& stage) {
  { e.update_aperture(li, base, value_fn, stage) } -> std::same_as<bool>;
};

// ─── Effective aperture entry ───────────────────────────────────────────────

/// A deduplicated aperture reference with a repetition count.
///
/// When the per-phase apertures contains duplicates (e.g. [1,2,3,3,3]),
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

// ─── Aperture task submission ────────────────────────────────────────────────

/// Result of a single aperture task (clone + update + solve + cut).
struct ApertureCutResult
{
  ApertureUid ap_uid {};
  double weight {0.0};
  bool feasible {false};
  int status {0};
  std::optional<SparseRow> cut {};
};

/// Callback for submitting a complete aperture task to the work pool.
///
/// Accepts a task function that returns an ApertureCutResult and submits
/// it to the SDDP work pool.  Returns a future for the result.
/// The caller submits all apertures first, then collects all futures,
/// enabling parallel execution.
using ApertureSubmitFunc = std::function<std::future<ApertureCutResult>(
    const std::function<ApertureCutResult()>& task)>;

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
/// @param scene_uid        Scene UID (for logging)
/// @param phase_uid        Phase UID (for logging)
/// @param submit_fn        Callback to submit an aperture task to the work pool
/// @param aperture_timeout Timeout in seconds for each aperture LP solve;
///                         0 = no timeout.  When exceeded, the aperture is
///                         treated as infeasible and skipped.
/// @param save_aperture_lp If true, save each aperture LP to the log directory
/// @param aperture_cache   Cache of pre-built aperture LP data
/// @param forward_col_sol  Forward-pass primal solution (warm-start hint for
///                         aperture clones).  Applied after update_aperture.
/// @param forward_row_dual Forward-pass dual solution (warm-start hint for
///                         aperture clones).  Applied after update_aperture.
/// @param pooled_clone     Optional pre-allocated LP clone from a work pool
/// @param iteration        Current SDDP iteration index
/// @param cut_coeff_mode   Mode for computing cut coefficients
/// @param scale_alpha      Scaling factor applied to the cut alpha (RHS)
/// @param cut_coeff_eps    Epsilon below which cut coefficients are zeroed
/// @param cut_coeff_max    Maximum absolute cut coefficient (0 = no limit)
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
    SceneUid scene_uid,
    PhaseUid phase_uid,
    const ApertureSubmitFunc& submit_fn,
    double aperture_timeout = 0.0,
    bool save_aperture_lp = false,
    const ApertureDataCache& aperture_cache = {},
    std::span<const double> forward_col_sol = {},
    std::span<const double> forward_row_dual = {},
    LinearInterface* pooled_clone = nullptr,
    IterationIndex iteration = {},
    CutCoeffMode cut_coeff_mode = CutCoeffMode::reduced_cost,
    double scale_alpha = 1.0,
    double cut_coeff_eps = 0.0,
    double cut_coeff_max = 0.0) -> std::optional<SparseRow>;

}  // namespace gtopt
