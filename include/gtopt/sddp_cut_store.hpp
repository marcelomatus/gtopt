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

/// Per-scene container for SDDP Benders cuts (step 1 of the
/// `support/sddp_cut_store_split_plan_2026-04-30.md` refactor).
///
/// Wraps the per-scene `std::vector<StoredCut>` plus the per-scene
/// snapshot that cut-sharing reads to identify newly-added cuts.
/// Single-writer during the forward/backward pass (phase access within
/// a scene is serial), so no mutex is needed.
///
/// In step 1 the methods on `SDDPCutStore` continue to do all the
/// per-scene work; this type just changes the layout of `m_scene_cuts_`
/// from `vector<StoredCut>` to `SceneCutStore`.  Subsequent migration
/// steps move per-scene operations onto this class proper.
///
/// The class exposes vector-like forwarders (`size`, `empty`,
/// `begin/end`, `front/back`, `operator[]`, `push_back`, `clear`,
/// `erase`, `resize`) so existing call sites that read or mutate the
/// per-scene vector compile unchanged after the layout swap.  Direct
/// access to the underlying vector is available via `cuts()` for the
/// rare sites that need to call `std::erase_if` or other free
/// algorithms specialised on `std::vector`.
class SceneCutStore
{
public:
  SceneCutStore() = default;

  // ── Vector-like forwarders ──────────────────────────────────────────
  [[nodiscard]] auto begin() noexcept { return m_cuts_.begin(); }
  [[nodiscard]] auto end() noexcept { return m_cuts_.end(); }
  [[nodiscard]] auto begin() const noexcept { return m_cuts_.begin(); }
  [[nodiscard]] auto end() const noexcept { return m_cuts_.end(); }
  [[nodiscard]] auto cbegin() const noexcept { return m_cuts_.cbegin(); }
  [[nodiscard]] auto cend() const noexcept { return m_cuts_.cend(); }
  [[nodiscard]] auto size() const noexcept { return m_cuts_.size(); }
  [[nodiscard]] bool empty() const noexcept { return m_cuts_.empty(); }
  [[nodiscard]] auto& front() noexcept { return m_cuts_.front(); }
  [[nodiscard]] const auto& front() const noexcept { return m_cuts_.front(); }
  [[nodiscard]] auto& back() noexcept { return m_cuts_.back(); }
  [[nodiscard]] const auto& back() const noexcept { return m_cuts_.back(); }
  [[nodiscard]] auto& operator[](std::size_t i) noexcept { return m_cuts_[i]; }
  [[nodiscard]] const auto& operator[](std::size_t i) const noexcept
  {
    return m_cuts_[i];
  }

  void push_back(const StoredCut& c) { m_cuts_.push_back(c); }
  void push_back(StoredCut&& c) { m_cuts_.push_back(std::move(c)); }
  template<class... Args>
  auto& emplace_back(Args&&... args)
  {
    return m_cuts_.emplace_back(std::forward<Args>(args)...);
  }
  void clear() noexcept { m_cuts_.clear(); }
  void resize(std::size_t n) { m_cuts_.resize(n); }
  template<class It>
  auto erase(It first, It last)
  {
    return m_cuts_.erase(first, last);
  }
  template<class It>
  auto erase(It it)
  {
    return m_cuts_.erase(it);
  }

  // ── Append a stored cut ─────────────────────────────────────────────
  /// Push one cut into this scene's per-scene vector.  Single-writer
  /// during the forward/backward pass (phase access within a scene
  /// is serial), so no mutex is needed even when multiple scene
  /// workers call `store` concurrently — each touches its own
  /// `SceneCutStore`.
  void store(const SparseRow& cut,
             CutType type,
             RowIndex row,
             SceneUid scene_uid_val,
             PhaseUid phase_uid_val);

  // ── Per-scene rollback (LP-deleting variant) ────────────────────────
  /// Drop every cut this scene has accumulated, deleting matching LP
  /// rows from each `(scene_index, phase)` cell via
  /// `LinearInterface::delete_rows` + `SystemLP::record_cut_deletion`.
  /// Used by `SDDPOptions::forward_infeas_rollback` to roll back a
  /// scene declared infeasible in the forward pass — both forward-pass
  /// feasibility cuts and earlier backward-pass optimality cuts go.
  /// Returns the number of LP rows actually deleted.
  ///
  /// `scene_index` is required: it identifies which
  /// `PlanningLP::system(s, p)` cells own the rows (cuts in this
  /// store live on `(scene_index, *)` cells by ownership invariant).
  /// The caller already has it in scope.
  std::ptrdiff_t clear_with_lp(PlanningLP& planning_lp, SceneIndex scene_index);

  // ── Refresh stored cut duals from the live LP ───────────────────────
  /// Update each stored cut's `dual` field by reading
  /// `LinearInterface::get_row_dual_raw()` at `cut.row` on the
  /// `(scene_index, phase)` cell.  Cuts whose `phase_uid` cannot be
  /// resolved are left with their existing dual (no error).  Used by
  /// `SDDPMethod::update_stored_cut_duals` to refresh duals after a
  /// solve so subsequent cut-pruning decisions read up-to-date values.
  ///
  /// This method drops the `scene_uid` lookup the legacy
  /// `SDDPCutStore::update_stored_cut_duals` did via
  /// `build_scene_uid_map`: every cut in this store is owned by
  /// `scene_index` by construction (the per-scene single-writer
  /// invariant), so the caller-supplied index is the authoritative
  /// scene for every cut here.
  void update_duals(PlanningLP& planning_lp, SceneIndex scene_index);

  // ── Direct access to the underlying vector ──────────────────────────
  /// Mutable view; used by call sites that need
  /// `std::erase_if(cuts(), ...)` or other free algorithms specialised
  /// on `std::vector<T>`.
  [[nodiscard]] std::vector<StoredCut>& cuts() noexcept { return m_cuts_; }
  [[nodiscard]] const std::vector<StoredCut>& cuts() const noexcept
  {
    return m_cuts_;
  }

  // ── Per-scene snapshot for cut sharing ──────────────────────────────
  /// Cut count snapshot before the backward pass starts.  The cut-
  /// sharing pass uses `cuts().size() - cuts_before()` to identify
  /// newly-added cuts.  Stored per-scene here (replaces the parallel
  /// `m_scene_cuts_before_` vector that the legacy class kept beside
  /// `m_scene_cuts_`).
  [[nodiscard]] std::size_t cuts_before() const noexcept
  {
    return m_cuts_before_;
  }
  void set_cuts_before(std::size_t n) noexcept { m_cuts_before_ = n; }

private:
  std::vector<StoredCut> m_cuts_ {};
  std::size_t m_cuts_before_ {0};
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

  /// Discard every cut scene `scene_index` has accumulated.  Deletes
  /// the corresponding rows from the scene's LP cells and clears
  /// `m_scene_cuts_[scene_index]`.  Used by
  /// `SDDPOptions::forward_infeas_rollback` to roll back a scene
  /// declared infeasible in the forward pass — both forward-pass
  /// feasibility cuts (PLP-style backtrack chain) and earlier
  /// backward-pass optimality cuts go: the bad trajectory that
  /// produced any of them is no longer trusted.  Returns the number
  /// of LP rows actually deleted.
  ///
  /// Shared cuts received by `scene_index` from peers via
  /// `share_cuts_for_phase` are NOT in `m_scene_cuts_[scene_index]`
  /// (they live only as LP rows + `m_active_cuts_` replay entries),
  /// so they survive this rollback — exactly the invariant that lets
  /// the next iteration's stall check distinguish "made no progress"
  /// from "received peer cuts and can retry".
  std::ptrdiff_t clear_scene_cuts(SceneIndex scene_index,
                                  PlanningLP& planning_lp);

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
  /// and need no mutex.  Each `SceneCutStore` wraps the per-scene
  /// `vector<StoredCut>` plus a per-scene `cuts_before` snapshot —
  /// the wrapper exposes vector-like forwarders so existing call sites
  /// (`m_scene_cuts_[si].push_back/size/empty/clear`, range-for) keep
  /// working unchanged.  See `SceneCutStore` doc for the migration
  /// rationale.
  StrongIndexVector<SceneIndex, SceneCutStore> m_scene_cuts_ {};

  /// Per-scene cut count snapshot before each backward pass.
  /// Populated by the caller right before dispatching the backward
  /// step so `apply_cut_sharing_for_iteration` can identify newly
  /// added cuts by offset.  Step 4 of
  /// `support/sddp_cut_store_split_plan_2026-04-30.md` migrates this
  /// parallel vector onto each `SceneCutStore`'s `cuts_before()`
  /// member; until then it lives here unchanged.
  std::vector<std::size_t> m_scene_cuts_before_ {};
};

}  // namespace gtopt
