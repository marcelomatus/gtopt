# SDDPCutStore Split: SceneCutStore + SDDPCutManager Refactor Plan

**Status:** draft — pending review
**Date:** 2026-04-30
**Author:** marce (with Claude review)
**Trigger:** user question (2026-04-30):
> "why we have this global sddp_cut_store, why we do not have a cut store
> per scene, instead of the global one"

The class today is global in *name* only — half of its API is per-scene
(takes a leading `SceneIndex` parameter), the other half is genuinely
cross-scene.  The combined surface forces every per-scene call site to
reach through the global object and re-thread `SceneIndex` it usually
already has on hand.  The recently-added `clear_scene_cuts(SceneIndex,
PlanningLP&)` (`include/gtopt/sddp_cut_store.hpp:147-148`) is the most
recent example of that asymmetry.  This plan splits the class so the
ownership boundary in the data layout (`StrongIndexVector<SceneIndex,
std::vector<StoredCut>>`, `sddp_cut_store.hpp:197`) is mirrored at the
type level.

---

## 1. Motivation

The class header already documents the per-scene single-writer property:

> "Single source of truth: per-scene vectors in `m_scene_cuts_`. […]
> No global mutex: per-scene vectors are single-writer during the
> forward/backward pass (phase access within a scene is serial).
> Cross-scene cut sharing reads from these vectors after the
> phase-step barrier in `run_backward_pass_synchronized`, so no lock
> is needed there either."
> — `include/gtopt/sddp_cut_store.hpp:62-70`

The data layout already enforces this invariant.  The API does not.
Concretely:

- `store_cut(SceneIndex, PhaseIndex, ...)` is invoked from
  `SDDPMethod::store_cut` (`source/sddp_method_cut_store.cpp:71-90`),
  which itself takes `SceneIndex` as its first parameter; the `m_cut_store_`
  member just re-receives the same scene index it cannot use without
  it.
- `clear_scene_cuts(SceneIndex, PlanningLP&)`
  (`source/sddp_cut_store.cpp:171-202`) is the rollback hook; the
  caller in `source/sddp_method_iteration.cpp:684` already has
  `scene_index` in scope from the `for (... si : ...)` loop.
- `cap_stored_cuts` (`source/sddp_cut_store.cpp:380-406`) and
  `prune_inactive_cuts` (`:233-376`) iterate `m_scene_cuts_` themselves
  — the *iteration* over scenes is the only cross-scene piece; the
  per-scene work inside the loop is pure per-scene logic that wants
  to live on a per-scene type.
- `apply_cut_sharing_for_iteration` (`:424-512`) and
  `build_combined_cuts` (`:410-420`) are genuinely cross-scene: they
  read all per-scene vectors at once.

A second symptom: every existing call site that touches per-scene cuts
(`source/sddp_method_iteration.cpp:123, 143, 590, 856, 920`,
`source/sddp_iteration.cpp:234-238, 538, 729, 953, 1412`) reaches
through `m_cut_store_.scene_cuts()[si]` to get a `std::vector<StoredCut>`.
That is a leaky public accessor — the wrapper is doing nothing for
those call sites except hiding the `StrongIndexVector` behind a getter.

---

## 2. Current state

Classification of every public method on `SDDPCutStore`:

| Method | Category | Source |
|---|---|---|
| `store_cut(SceneIndex, ...)` | per-scene | `sddp_cut_store.cpp:42-71` |
| `clear_scene_cuts(SceneIndex, PlanningLP&)` | per-scene | `:171-202` |
| `forget_first_cuts(count, PlanningLP&)` | per-scene (per-scene cap, but iterates all) | `:84-159` |
| `prune_inactive_cuts(opts, plp, states)` | per-scene work, cross-scene loop | `:233-376` |
| `cap_stored_cuts(opts, plp)` | per-scene work, cross-scene loop | `:380-406` |
| `apply_cut_sharing_for_iteration(...)` | cross-scene | `:424-512` |
| `build_combined_cuts(plp)` | cross-scene | `:410-420` |
| `save_cuts_for_iteration(...)` | cross-scene (orchestration) | `:516-625` |
| `update_stored_cut_duals(plp)` | per-scene work, cross-scene loop | `:206-229` |
| `num_stored_cuts()` | cross-scene (sum) | `sddp_cut_store.hpp:103-110` |
| `clear()` | cross-scene (clears all) | `sddp_cut_store.cpp:75-80` |
| `scene_cuts()` (mutable + const) | bookkeeping (raw access) | `sddp_cut_store.hpp:79-95` |
| `scene_cuts_before()` (mutable + const) | bookkeeping (snapshot) | `sddp_cut_store.hpp:85-92` |
| `resize_scenes(Index)` | bookkeeping | `sddp_cut_store.hpp:98` |

The `cap_stored_cuts` / `prune_inactive_cuts` / `forget_first_cuts` /
`update_stored_cut_duals` row reflects an interesting middle ground:
the *per-cell* work is per-scene, but the *cross-scene loop + the
PLP-level mutation that touches multiple LP cells* is naturally a
manager-level concern.  The split below puts the per-cell work on
`SceneCutStore` and keeps the loop on `SDDPCutManager`.

### 2.1 Migration list (call sites)

`grep -rn "m_cut_store_\." source/`:

- `source/sddp_method.cpp:260` — `resize_scenes(num_scenes)` setup
- `source/sddp_method_cut_store.cpp:56,61,66,85,112,120,125,137,145` —
  the thin wrapper layer; each call to a per-scene method on
  `SDDPCutStore` becomes a `m_cut_manager_.at(si).<method>(...)`.
- `source/sddp_method_iteration.cpp:123,143,590,596,630,684,690,715,856,920,994,1106`
  — mixed: read-only `scene_cuts()[si]`, `num_stored_cuts()`,
  `clear_scene_cuts(si, plp)`, `apply_cut_sharing_for_iteration(...)`,
  `save_cuts_for_iteration(...)`.
- `source/sddp_iteration.cpp:234-238,538,729,953,1412` — read/write to
  `scene_cuts()[si]` and `scene_cuts_before()` for the cut-sharing
  snapshot.
- `source/sddp_cut_sharing.cpp:70` — comment only.

Plus `cut_store()` on `SDDPMethod` (`include/gtopt/sddp_method.hpp:351`):
the single external accessor returning `SDDPCutStore&`.  After the
rename it returns `SDDPCutManager&`; the only caller is the cascade
layer + tests.  We keep a `[[deprecated]]` `cut_store()` shim during
migration.

---

## 3. Proposed design

### 3.1 `SceneCutStore` — owns one scene's cuts

```cpp
namespace gtopt {

/// Storage for a single SDDP scene's Benders cuts.  Single-writer
/// during the forward/backward pass (phase access within a scene is
/// serial), so no mutex is needed.
class SceneCutStore
{
public:
  SceneCutStore() = default;

  // ── Read access ──────────────────────────────────────────────────
  [[nodiscard]] const std::vector<StoredCut>& cuts() const noexcept;
  [[nodiscard]] std::vector<StoredCut>& cuts() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  /// Per-scene snapshot of cuts before the backward pass — used by
  /// the manager's cut-sharing pass to identify newly-added cuts by
  /// offset.  One offset per scene replaces the parallel
  /// `m_scene_cuts_before_` vector.
  [[nodiscard]] std::size_t cuts_before() const noexcept;
  void set_cuts_before(std::size_t n) noexcept;

  // ── Mutations ────────────────────────────────────────────────────

  /// Append one stored cut.  Caller threads its already-known
  /// `SceneIndex` through because LP-cell access still needs it
  /// (`(s, phase)` cells live on `PlanningLP`, not on the cut store).
  void store(SceneIndex scene_index, PhaseIndex src_phase_index,
             const SparseRow& cut, CutType type, RowIndex row,
             SceneUid scene_uid_val, PhaseUid phase_uid_val);

  /// Drop every cut this scene has accumulated; deletes matching LP
  /// rows from each `(scene_index, phase)` cell.  Returns rows
  /// deleted.  Replaces `SDDPCutStore::clear_scene_cuts`.
  std::ptrdiff_t clear(PlanningLP& planning_lp, SceneIndex scene_index);

  /// Drop the first `count` cuts of this scene (per-scene cap of
  /// `forget_first_cuts`).  Cross-scene row-shift bookkeeping stays
  /// on the manager.
  void forget_first(std::ptrdiff_t count);

  /// Apply the manager's per-cell pruning result: erase cuts on
  /// pruned rows + row-shift survivors.
  void apply_pruning(const std::map<PhaseIndex, std::vector<Index>>& deleted);

  /// Refresh each stored cut's dual from the current LP solution.
  void update_duals(PlanningLP& planning_lp, SceneIndex scene_index);

private:
  std::vector<StoredCut> m_cuts_ {};
  std::size_t m_cuts_before_ {0};
};

}  // namespace gtopt
```

The key design point: `SceneIndex` does not disappear from the API,
it migrates from the call-site's parameter list to the manager-level
indexer.  Inside the per-scene methods, `scene_index` is still a
required parameter for any operation that touches LP cells, because
the cells live on `PlanningLP::system(scene_index, phase_index)` —
not on the cut store.  The header comment on `clear` makes that
explicit: per-scene ownership of cuts vs. per-cell ownership of LP
rows is a deliberate split.

### 3.2 `SDDPCutManager` — thin owner of per-scene stores

```cpp
namespace gtopt {

class SDDPCutManager
{
public:
  SDDPCutManager() = default;

  // Bookkeeping
  void resize_scenes(Index num_scenes);
  [[nodiscard]] Index scene_count() const noexcept;

  // Per-scene access — caller passes the SceneIndex it already holds.
  [[nodiscard]] SceneCutStore& at(SceneIndex s);
  [[nodiscard]] const SceneCutStore& at(SceneIndex s) const;
  [[nodiscard]] auto& scenes() noexcept;
  [[nodiscard]] const auto& scenes() const noexcept;

  // Cross-scene operations
  [[nodiscard]] std::ptrdiff_t num_stored_cuts() const noexcept;
  void clear_all();
  void forget_first_cuts(std::ptrdiff_t count, PlanningLP& planning_lp);
  void cap_stored_cuts(const SDDPOptions&, const PlanningLP&);
  void prune_inactive_cuts(const SDDPOptions&, PlanningLP&,
      const StrongIndexVector<SceneIndex,
            StrongIndexVector<PhaseIndex, PhaseStateInfo>>& scene_phase_states);
  void update_stored_cut_duals(PlanningLP&);
  [[nodiscard]] std::vector<StoredCut> build_combined_cuts(
      const PlanningLP&) const;
  void apply_cut_sharing_for_iteration(IterationIndex, const SDDPOptions&,
      PlanningLP&, const LabelMaker&);
  void save_cuts_for_iteration(IterationIndex, std::span<const uint8_t>,
      const SDDPOptions&, PlanningLP&, const LabelMaker&,
      const StrongIndexVector<SceneIndex,
            StrongIndexVector<PhaseIndex, PhaseStateInfo>>&,
      IterationIndex);

private:
  StrongIndexVector<SceneIndex, SceneCutStore> m_scene_stores_ {};
};

}  // namespace gtopt
```

Behaviour notes:
- `forget_first_cuts` owns the cross-scene row-shift bookkeeping;
  delegates the per-scene `cuts.erase` to `SceneCutStore::forget_first`.
- `prune_inactive_cuts` walks each `(scene, phase)` cell once,
  computes pruned rows, and calls `at(s).apply_pruning(...)` to do
  the per-scene erase + shift.
- `save_cuts_for_iteration` is unchanged in behaviour; see Open
  question 2 for whether it should move to a `SDDPCutPersistence`.

### 3.3 Migrated call-site shape

Before:

```cpp
// source/sddp_method_cut_store.cpp:85
m_cut_store_.store_cut(scene_index, src_phase_index, cut, type, row,
                       scene_uid, phase_uid);

// source/sddp_method_iteration.cpp:684
m_cut_store_.clear_scene_cuts(scene_index, planning_lp());
```

After:

```cpp
m_cut_manager_.at(scene_index).store(scene_index, src_phase_index, cut,
                                     type, row, scene_uid, phase_uid);

m_cut_manager_.at(scene_index).clear(planning_lp(), scene_index);
```

The duplicated `scene_index` in `at(scene_index).store(scene_index, ...)`
makes the asymmetry visible: ownership is per-scene, LP-cell access is
per-(scene, phase).  We could collapse the duplication by stashing
`SceneIndex` on `SceneCutStore` itself (set by `resize_scenes`); see
**Open question 3**.

The bookkeeping reads on `m_cut_store_.scene_cuts()[si]` become
`m_cut_manager_.at(si).cuts()`.  The `scene_cuts_before()` snapshot
becomes `m_cut_manager_.at(si).cuts_before()` (one offset per scene
instead of a parallel `std::vector<std::size_t>`).

### 3.4 What stays where

- `build_phase_uid_map` / `build_scene_uid_map`
  (`sddp_cut_store.cpp` private helpers, used by
  `forget_first_cuts`, `update_stored_cut_duals`, `prune_inactive_cuts`)
  move to the manager's translation unit since they are cross-scene.
- The `scene_cuts_before_` parallel `std::vector<std::size_t>`
  (`sddp_cut_store.hpp:203`) is replaced by an instance member on
  each `SceneCutStore` (`m_cuts_before_`).  Same total memory, cleaner
  ownership.

---

## 4. Migration sequence

Each step compiles + passes full ctest in isolation.

| # | Change | Tests | Risk |
|---|---|---|---|
| 1 | Introduce `SceneCutStore` as a thin alias / inner type for `std::vector<StoredCut>` + `m_cuts_before_`.  No method moves yet — the existing `SDDPCutStore` keeps doing all the work but iterates `m_scene_cuts_` of type `StrongIndexVector<SceneIndex, SceneCutStore>` instead of `std::vector<StoredCut>`. | Existing SDDP suite. | none |
| 2 | Move `store`, `clear`, `forget_first`, `update_duals`, `apply_pruning` onto `SceneCutStore`.  `SDDPCutStore::store_cut` / `clear_scene_cuts` / `update_stored_cut_duals` become 1-line forwarders. | Existing SDDP suite + new `test_scene_cut_store.cpp` covering each per-scene method in isolation. | low |
| 3 | Migrate the call-site cluster in `source/sddp_method_iteration.cpp` and `source/sddp_iteration.cpp` from `m_cut_store_.scene_cuts()[si]` to `m_cut_store_.at(si).cuts()`.  Mark `SDDPCutStore::scene_cuts()` `[[deprecated]]`. | Existing suite. | low |
| 4 | Migrate `m_cut_store_.scene_cuts_before()` reads/writes to per-scene `at(si).cuts_before()` / `at(si).set_cuts_before(...)`.  Drop the parallel `m_scene_cuts_before_` member. | Existing suite + boundary-cut replay regression tests. | low |
| 5 | Rename the class `SDDPCutStore` → `SDDPCutManager`.  Add a `using SDDPCutStore [[deprecated]] = SDDPCutManager;` shim and a `[[deprecated]] cut_store()` accessor on `SDDPMethod` returning `SDDPCutManager&`.  Add the new `cut_manager()` accessor.  Migrate `SDDPMethod::cut_store()` callers (cascade + tests) to the new name. | Full ctest, including cascade integration tests. | low |
| 6 | Drop the deprecated alias + accessor.  Pure cleanup. | Full ctest. | none |

Steps 1-4 land as a single PR; step 5 in its own PR; step 6 after one
quiet release.

---

## 5. What this plan does NOT change

- **`m_scene_cuts_` data layout.** Already
  `StrongIndexVector<SceneIndex, std::vector<StoredCut>>`
  (`sddp_cut_store.hpp:197`).  The split surfaces this layout in the
  public API; it doesn't reshape it.
- **`StoredCut` struct.** Same fields as today
  (`sddp_cut_store.hpp:48-60`).  In particular `StoredCut::scene_uid`
  is left in place during the split — see Open question 3 for the
  separate "is it still useful?" question.
- **Rollback semantics.** `clear_scene_cuts` / `forward_infeas_rollback`
  behave identically; the LP-row deletion path through
  `LinearInterface::delete_rows` + `record_cut_deletion` is unchanged.
- **Alpha-release semantics.** `free_alpha_for_cut` does not interact
  with the cut store; unchanged.
- **Call sites that already take `SceneIndex` as a parameter.** Every
  per-scene method retains `SceneIndex` in its signature for LP-cell
  access — the parameter only moves from the front of the argument
  list to the manager-level indexer, not out of the API entirely.
- **Concurrency model.** Per-scene single-writer property
  (`sddp_cut_store.hpp:64-70`) is preserved.  No mutex is added.

---

## 6. Open questions to resolve before kickoff

1. **Mutable per-scene access: `at(s)` reference vs. `with_scene(s,
   lambda)` callback?**  The reference-returning `at(s)` is the
   simpler ergonomic choice and matches how every existing call site
   already touches per-scene cuts (`m_cut_store_.scene_cuts()[si]`).
   The callback form (`m_cut_manager_.with_scene(si, [&](auto& store)
   {...})`) is friendlier to a future where each scene has its own
   mutex (re-introducing mutexes is on the table only if the
   single-writer invariant ever weakens).  Author leans `at(s)`
   reference; revisit only if a concurrency change forces the issue.

2. **Does cut persistence (`save_cuts_for_iteration`,
   `save_scene_cuts_csv` calls in `:561-578`) belong on the manager,
   or on a separate `SDDPCutPersistence` class?**  The current
   `save_cuts_for_iteration` is a 110-line orchestration pass that
   mixes filesystem path construction, format selection, per-scene
   CSV write, state-CSV write, and the rename-on-infeasibility step.
   None of that is fundamentally about cut storage — it's about
   formatting cuts for the on-disk schema.  Splitting it into an
   `SDDPCutPersistence` class (with its own JSON/CSV/state hooks)
   would shrink the manager to ~150 lines and make the
   formatting+naming logic separately testable.  Author leans toward
   the split, but defers it — this plan does the data-ownership split
   first, then the persistence split as a follow-up.

3. **Keep `StoredCut::scene_uid` once each store implies the scene by
   ownership?**  Today the field is stamped at `store_cut`
   (`sddp_cut_store.cpp:42-71`) and re-resolved via `build_scene_uid_map`
   in `forget_first_cuts`, `update_stored_cut_duals`, and
   `prune_inactive_cuts`.  After the split, every cut in
   `m_scene_stores_[si].cuts()` is by definition owned by `SceneIndex
   = si`, so the redundant `scene_uid` field could be dropped (and
   the `build_scene_uid_map` helper retired).  The save-path uses it
   to recover the originating UID for the CSV; a `SceneIndex`→`SceneUid`
   lookup at save time replaces it.  Author leans toward dropping it
   in a follow-up; for the split itself, leave it alone to keep diff
   surface narrow.

---

## 7. Out of scope (for a follow-up plan)

- Consolidating `m_scene_phase_states_` (`source/sddp_method.cpp:259`)
  and `m_scene_retry_state_` (`:266`) — both
  `StrongIndexVector<SceneIndex, ...>` — into a single `SDDPSceneState`
  alongside `SceneCutStore` is the natural next step but warrants its
  own plan.
- The `single_cut_storage` flag's "where do I write?" decision becomes
  a manager-level concern after the split; revisit when the
  persistence split (Open question 2) lands.
