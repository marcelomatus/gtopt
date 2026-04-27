/**
 * @file      sddp_cut_store.hpp
 * @brief     Thread-safe storage for SDDP Benders cuts
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Owns the combined and per-scene cut vectors, the protecting mutex,
 * and the per-scene snapshot used by cut sharing.  Extracted from
 * SDDPMethod to separate cut bookkeeping from the solver orchestration.
 */

#pragma once

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/sddp_cut_store_enums.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// Forward declarations
class PlanningLP;
struct SDDPOptions;
struct PhaseStateInfo;

// ─── Stored cut types ──────────────────────────────────────────────────────

/// Result of a cut load operation.
struct CutLoadResult
{
  int count {};  ///< Number of unique cuts loaded
  IterationIndex max_iteration {};  ///< Highest iteration index found
};

/// A serialisable representation of a Benders cut
struct StoredCut
{
  CutType type {CutType::Optimality};
  PhaseUid phase_uid {};  ///< Phase UID this cut was added to
  SceneUid scene_uid {};  ///< Scene UID that generated this cut (-1 = shared)
  std::string name {};  ///< Cut name
  double rhs {};  ///< Right-hand side (lower bound)
  double scale {1.0};  ///< Row scale (physical → LP), mirrors SparseRow::scale
  std::optional<double> dual {};  ///< Row dual value (nullopt = unknown)
  RowIndex row {};  ///< LP row index where this cut was added
  /// Coefficient pairs: (column_index, coefficient)
  std::vector<std::pair<ColIndex, double>> coefficients {};
};

/// Storage for SDDP Benders cuts.
///
/// Single source of truth: per-scene vectors in `m_scene_cuts_`.
/// The combined view is rebuilt on demand via `build_combined_cuts`.
/// No global mutex: per-scene vectors are single-writer during the
/// forward/backward pass (phase access within a scene is serial).
/// Cross-scene cut sharing reads from these vectors after the
/// phase-step barrier in `run_backward_pass_synchronized`, so no lock
/// is needed there either.
class SDDPCutStore
{
public:
  SDDPCutStore() = default;

  // ── Accessors ────────────────────────────────────────────────────────

  /// Per-scene cut storage.
  [[nodiscard]] const auto& scene_cuts() const noexcept
  {
    return m_scene_cuts_;
  }

  /// Per-scene cut count snapshot before backward pass.
  [[nodiscard]] auto& scene_cuts_before() noexcept
  {
    return m_scene_cuts_before_;
  }
  [[nodiscard]] const auto& scene_cuts_before() const noexcept
  {
    return m_scene_cuts_before_;
  }

  /// Mutable per-scene cuts (for direct iteration access).
  [[nodiscard]] auto& scene_cuts() noexcept { return m_scene_cuts_; }

  /// Resize per-scene storage to the given number of scenes.
  void resize_scenes(Index num_scenes) { m_scene_cuts_.resize(num_scenes); }

  // ── Cut count ────────────────────────────────────────────────────────

  /// Total number of stored cuts across all scenes.
  [[nodiscard]] std::ptrdiff_t num_stored_cuts() const noexcept
  {
    std::ptrdiff_t total = 0;
    for (const auto& sc : m_scene_cuts_) {
      total += std::ssize(sc);
    }
    return total;
  }

  // ── Core operations ──────────────────────────────────────────────────

  /// Store a cut in the per-scene vector.  Safe to call from scene
  /// workers concurrently since each scene has its own vector and
  /// phase access within a scene is serial.
  void store_cut(SceneIndex scene_index,
                 PhaseIndex src_phase_index,
                 const SparseRow& cut,
                 CutType type,
                 RowIndex row,
                 SceneUid scene_uid_val,
                 PhaseUid phase_uid_val);

  /// Clear all stored cut metadata (combined + per-scene).
  void clear();

  /// Remove the first @p count cuts from stored cuts and from the LP.
  void forget_first_cuts(std::ptrdiff_t count, PlanningLP& planning_lp);

  /// Update dual values of stored cuts from the current LP solution.
  void update_stored_cut_duals(PlanningLP& planning_lp);

  /// Prune inactive cuts from all (scene, phase) LPs.
  void prune_inactive_cuts(
      const SDDPOptions& options,
      PlanningLP& planning_lp,
      const StrongIndexVector<SceneIndex,
                              StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
          scene_phase_states);

  /// Cap per-scene stored cuts to max_stored_cuts (oldest dropped).
  void cap_stored_cuts(const SDDPOptions& options,
                       const PlanningLP& planning_lp);

  /// Build combined stored cuts from per-scene vectors.
  [[nodiscard]] std::vector<StoredCut> build_combined_cuts(
      const PlanningLP& planning_lp) const;

  /// Apply cut-sharing across scenes for all phases in this iteration.
  /// New cuts are identified by comparing the current per-scene vector
  /// size against `m_scene_cuts_before_` (populated by the caller
  /// before the backward pass).
  void apply_cut_sharing_for_iteration(IterationIndex iteration_index,
                                       const SDDPOptions& options,
                                       PlanningLP& planning_lp,
                                       const LabelMaker& label_maker);

  /// Save cuts (combined + per-scene) after an iteration.
  void save_cuts_for_iteration(
      IterationIndex iteration_index,
      std::span<const uint8_t> scene_feasible,
      const SDDPOptions& options,
      PlanningLP& planning_lp,
      const LabelMaker& label_maker,
      const StrongIndexVector<SceneIndex,
                              StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
          scene_phase_states,
      IterationIndex current_iteration);

private:
  /// Per-scene cut storage — the single source of truth for every
  /// cut the SDDP method has stored.  Optimality and feasibility cuts
  /// both live here; `build_combined_cuts` exposes a union view for
  /// save paths.  Phase access within a scene is serial in the SDDP
  /// forward/backward passes, so per-scene vectors are single-writer
  /// and need no mutex.
  StrongIndexVector<SceneIndex, std::vector<StoredCut>> m_scene_cuts_ {};

  /// Per-scene cut count snapshot before each backward pass.
  /// Populated by the caller right before dispatching the backward
  /// step so `apply_cut_sharing_for_iteration` can identify newly
  /// added cuts by offset.
  std::vector<std::size_t> m_scene_cuts_before_ {};
};

}  // namespace gtopt
