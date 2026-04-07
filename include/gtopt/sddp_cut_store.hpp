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

/// Thread-safe storage for SDDP Benders cuts (combined + per-scene).
///
/// This class owns the cut vectors that were previously data members
/// of SDDPMethod: m_stored_cuts_, m_cuts_mutex_, m_scene_cuts_, and
/// m_scene_cuts_before_.  All mutating operations are provided as
/// methods that receive external dependencies (PlanningLP, SDDPOptions,
/// etc.) as parameters.
class SDDPCutStore
{
public:
  SDDPCutStore() = default;

  // ── Accessors ────────────────────────────────────────────────────────

  /// All stored cuts (combined storage).
  [[nodiscard]] const auto& stored_cuts() const noexcept
  {
    return m_stored_cuts_;
  }

  /// Mutable reference to combined storage (for resize in iteration).
  [[nodiscard]] auto& stored_cuts() noexcept { return m_stored_cuts_; }

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

  /// Mutex protecting m_stored_cuts_.
  [[nodiscard]] auto& cuts_mutex() const noexcept { return m_cuts_mutex_; }

  /// Resize per-scene storage to the given number of scenes.
  void resize_scenes(Index num_scenes) { m_scene_cuts_.resize(num_scenes); }

  // ── Cut count ────────────────────────────────────────────────────────

  /// Number of stored cuts (thread-safe).
  /// In single_cut_storage mode, counts across all per-scene vectors.
  [[nodiscard]] int num_stored_cuts(bool single_cut_storage) const noexcept
  {
    if (single_cut_storage) {
      int total = 0;
      for (const auto& sc : m_scene_cuts_) {
        total += static_cast<int>(sc.size());
      }
      return total;
    }
    const std::scoped_lock lock(m_cuts_mutex_);
    return static_cast<int>(m_stored_cuts_.size());
  }

  // ── Core operations ──────────────────────────────────────────────────

  /// Store a cut for sharing and persistence (thread-safe).
  void store_cut(SceneIndex scene_index,
                 PhaseIndex src_phase_index,
                 const SparseRow& cut,
                 CutType type,
                 RowIndex row,
                 bool single_cut_storage,
                 SceneUid scene_uid_val,
                 PhaseUid phase_uid_val);

  /// Clear all stored cut metadata (combined + per-scene).
  void clear() noexcept;

  /// Remove the first @p count cuts from stored cuts and from the LP.
  void forget_first_cuts(int count, PlanningLP& planning_lp);

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
  void apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                       IterationIndex iteration_index,
                                       const SDDPOptions& options,
                                       PlanningLP& planning_lp,
                                       const LabelMaker& label_maker);

  /// Save cuts (combined + per-scene) after an iteration.
  void save_cuts_for_iteration(
      IterationIndex iter,
      std::span<const uint8_t> scene_feasible,
      const SDDPOptions& options,
      PlanningLP& planning_lp,
      const LabelMaker& label_maker,
      const StrongIndexVector<SceneIndex,
                              StrongIndexVector<PhaseIndex, PhaseStateInfo>>&
          scene_phase_states,
      int current_iteration);

private:
  std::vector<StoredCut> m_stored_cuts_ {};
  mutable std::mutex m_cuts_mutex_;

  /// Per-scene cut storage.
  StrongIndexVector<SceneIndex, std::vector<StoredCut>> m_scene_cuts_ {};

  /// Per-scene cut count snapshot before each backward pass.
  std::vector<std::size_t> m_scene_cuts_before_ {};
};

}  // namespace gtopt
