# Per-Scene Infeasibility Rollback + Stall-Detection Plan

**Status:** draft — pending review
**Date:** 2026-04-30
**Author:** marce (with Claude review)
**Trigger:** companion to the `share_cuts_for_phase → add_cut_row` fix
that landed today (2026-04-30).  That fix unblocks `juan/gtopt_iplp` iter
i1 by routing shared optimality cuts through `free_alpha_for_cut`.  The
present plan addresses the *next* class of failure: when a scene's
forward pass cannot recover within an iteration, the cuts accumulated on
its bad trajectory poison subsequent iterations and may also induce an
infinite retry loop in degenerate cases.

---

## 1. Motivation

Current behaviour when a scene S fails its forward pass at iteration k:

- `out.scene_feasible[S] = 0` is set; backward pass skips S
  (`sddp_method_iteration.cpp:614-615`, `:698-700`).
- **Every cut S installed during iter k stays.**  In particular, the
  PLP-style backtrack path (`sddp_forward_pass.cpp:526-577`) installs
  feasibility cuts on phases p, p-1, p-2, … as the elastic filter
  recovers.  Each `add_cut_row(...CutType::Feasibility, ...)` writes
  the cut into `m_scene_cuts_[S]` and into S's LP rows.  When the
  filter eventually gives up (no predecessor phase to cut on), S is
  declared infeasible — but the partial chain of fcuts it installed
  along the bad trajectory survives.
- Iteration k+1 reuses that LP.  The accumulated fcuts may rule out
  the new trial point produced by other scenes' backward-pass
  optimality cuts.  In the worst case S remains infeasible for the
  rest of the run.

A second failure mode lurks even with the rollback in place: if S is
the only scene producing cuts (single-scene run, or `cut_sharing=none`
with all other scenes also stuck), no fresh cuts ever arrive on S's
LP and a naive "always retry" policy spins forever.

The proposal closes both gaps:

1. **Rollback** — when S is declared infeasible at iter k forward,
   discard every cut S accumulated during iter k from S's LP cells and
   from `m_scene_cuts_[S]`.
2. **Stall detection** — record the scene's "total external cuts visible"
   snapshot at the moment of failure.  At iter k+1 dispatch, compare the
   current snapshot.  If it grew (other scenes contributed via cut
   sharing), retry S.  If it did not grow, the run cannot make progress
   on S and we abort cleanly.

Both behaviours are gated on a new `SDDPOptions` field so the existing
PLP-style backtrack semantics remain the default until the feature is
opted into and validated.

---

## 2. Design

### 2.1 New state on `SDDPMethod`

Two per-scene vectors, sized at `initialize_solver` time alongside the
existing `m_scene_phase_states_`:

```cpp
struct SceneRetryState {
  /// Cut count in `m_scene_cuts_[S]` at the START of the current
  /// iteration.  Set at iter k forward dispatch.  Used to identify
  /// "cuts S installed during iter k" for rollback at the end of
  /// iter k forward if S failed.
  std::size_t cuts_at_iter_start {0};

  /// Snapshot of the GLOBAL cut count (sum across every scene's
  /// `m_scene_cuts_[*]`) at the moment S was last declared
  /// infeasible.  `std::nullopt` if S has never failed.
  std::optional<std::ptrdiff_t> global_cuts_at_last_failure {};
};

StrongIndexVector<SceneIndex, SceneRetryState> m_scene_retry_state_ {};
```

`m_scene_retry_state_` is allocated once per `SDDPMethod::solve` call.
Sized in `initialize_solver` after `resize_scenes(num_scenes)`; cleared
on each new `solve()` so cascade levels and hot-restarts start fresh.

### 2.2 Forward-pass dispatch hook (snapshot)

In `run_forward_pass_all_scenes` (`sddp_method_iteration.cpp:583-654`),
right before submitting the per-scene tasks:

```cpp
for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
  m_scene_retry_state_[si].cuts_at_iter_start =
      m_cut_store_.scene_cuts()[si].size();
}
```

Cheap (one size read per scene) and correct because the forward pass is
the only source of new cuts on `m_scene_cuts_[si]` during this iteration
window — backward-pass cuts on phase p land on `m_scene_cuts_[si]` for
phase p-1, but only after the forward pass's barrier.

### 2.3 Forward-pass post-processing hook (rollback + stall check)

After collecting per-scene futures (`sddp_method_iteration.cpp:607-622`),
if `m_options_.forward_infeas_rollback == true`, for every scene with
`scene_feasible[si] == 0`:

**(a) Rollback this iteration's S-installed cuts.**

```cpp
const auto kept = m_scene_retry_state_[si].cuts_at_iter_start;
const auto current = m_cut_store_.scene_cuts()[si].size();
if (current > kept) {
  rollback_scene_cuts(si, kept, current);
}
```

`rollback_scene_cuts(si, kept, current)` (new method):
1. Walk the tail entries `m_scene_cuts_[si][kept..current)`.  Each
   `StoredCut` carries `RowIndex row` and the `(scene, phase)` cell
   it was installed on.
2. Group rows by `(SceneIndex, PhaseIndex)`.  For each group,
   call `LinearInterface::delete_rows(span<int>)` (already exists,
   `linear_interface.cpp:1384-1430`) and
   `LinearInterface::record_cut_deletion(...)` so the
   `m_active_cuts_` low-memory replay buffer drops them too.
3. Truncate `m_scene_cuts_[si]` to `kept`.

Edge case: PLP-style backtrack installed an fcut on `(scene_index,
prev_phase_index)` and then advanced — successfully — past that
backtrack point.  If the iteration eventually fails further upstream,
that earlier "successful" fcut is still on a bad trajectory and should
be rolled back together with the rest of this iteration's S-installed
cuts.  The proposed semantics (rollback **all** cuts S installed this
iteration, not just the ones after the failure point) match the
user's stated intent and are simpler to reason about.

**(b) Snapshot global cuts for next-iteration stall check.**

```cpp
m_scene_retry_state_[si].global_cuts_at_last_failure =
    m_cut_store_.num_stored_cuts();  // sum across every scene
```

Note: the snapshot is taken *after* rollback so the comparison at the
next iteration measures cuts that arrived from **other** sources.

### 2.4 Next-iteration retry / stall-stop hook

At the top of the next forward dispatch (`run_forward_pass_all_scenes`),
before `cuts_at_iter_start` is updated:

```cpp
// For every scene that failed last iteration, decide whether the
// global cut store has grown since the failure.  If it hasn't, no
// new information has arrived and retrying S would loop.
std::size_t any_progress = 0;
std::size_t stalled = 0;
for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
  const auto& rs = m_scene_retry_state_[si];
  if (!rs.global_cuts_at_last_failure.has_value()) continue;
  if (m_cut_store_.num_stored_cuts() > *rs.global_cuts_at_last_failure) {
    ++any_progress;
    rs.global_cuts_at_last_failure.reset();  // S retries this iter
  } else {
    ++stalled;
  }
}

// Stall-stop: every previously-failed scene saw zero new cuts.  No
// recovery path exists.  Abort the run cleanly.
const auto failed_last_iter =
    std::ranges::count_if(m_scene_retry_state_, [](const auto& rs) {
      return rs.global_cuts_at_last_failure.has_value();
    });
if (failed_last_iter > 0 && any_progress == 0) {
  return std::unexpected(Error {
      .code = ErrorCode::SolverError,
      .message = std::format(
          "SDDP: {} scene(s) infeasible at iter {} and zero new cuts "
          "since their last failure — no recovery path",
          stalled, iteration_index),
  });
}
```

The "global cut count grew" predicate covers all four
`CutSharingMode` settings:

| Mode | Source of new cuts on a failed S | Detected by snapshot? |
|---|---|---|
| `none` | none (S's LP receives no cuts from other scenes) | no, so stall-stop fires immediately on a single-scene run or all-scenes-failed run |
| `accumulate` | broadcast accumulated cut on every phase | yes — `m_scene_cuts_[*]` grows across every scene |
| `expected` | broadcast expected cut on every phase | yes |
| `max` | every source cut broadcast to every scene | yes |

The single-scene case (`num_scenes == 1` and S fails) reduces to mode
`none`: no other source ever produces cuts, snapshot stays equal,
stall-stop fires.  The "only scene running or sharing produces no
cuts" guard the user requested is exactly this predicate.

### 2.5 New CLI / JSON option

```cpp
struct SDDPOptions {
  ...
  /// When `true`, a scene declared infeasible in the forward pass
  /// has its cuts from THIS iteration rolled back (deleted from the
  /// LP and from `m_scene_cuts_`).  At the next iteration the scene
  /// retries iff new cuts have arrived from other scenes' backward
  /// pass + cut sharing; otherwise the run stops with an
  /// "no recovery path" error.  When `false` (default), preserve
  /// the legacy PLP-style behaviour: cuts persist across iterations
  /// even when the scene itself was declared infeasible.
  bool forward_infeas_rollback {false};
};
```

JSON: `sddp.forward_infeas_rollback` with the same default.

CLI: `--forward-infeas-rollback` flag (off by default).

---

## 3. Interactions to verify

### 3.1 Backward pass

Backward pass already skips infeasible scenes
(`sddp_method_iteration.cpp:698-700`).  After rollback those scenes have
the same LP shape they had at iter k start (modulo any cuts shared from
other scenes' k-1 backward pass).  No additional changes needed.

### 3.2 Cut sharing

`share_cuts_for_phase` reads `scene_cuts_before_` to identify
newly-added cuts after each phase's backward step.  Rollback runs
between iterations, after `save_cuts_for_iteration`.  The next
iteration's `apply_cut_sharing_for_iteration` will see the rolled-back
size as the "before" count, so no spurious sharing of deleted cuts.

### 3.3 Hot-start / cascade level handoff

`SDDPCutStore::build_combined_cuts` is called by the cascade layer to
hand off cuts to the next level.  Rollback runs *during* an iteration's
post-processing, BEFORE the cascade level finishes — so the handoff
sees only the surviving cuts.  No persistence path needs to know about
the rollback.

Saved cut files (`save_cuts_csv`) iterate `m_scene_cuts_`, also AFTER
rollback runs.  Same outcome: rolled-back cuts are not persisted.

### 3.4 Low-memory replay (`m_active_cuts_`)

`LinearInterface::record_cut_deletion(span<int>)` already handles the
buffer-side erase correctly (`linear_interface.cpp:398-416`).  The
rollback hook calls it for every (scene, phase) cell whose cuts are
being removed.  Confirmed correct against the bulk-replay path that
landed today (`apply_post_load_replay → add_rows`).

### 3.5 PLP-style fail-stop mode

`m_options_.forward_fail_stop = true` already exits a scene's forward
pass at the first elastic-recovery cut and re-runs the whole pass next
iter.  Rollback *adds* cleanup on top: even fail-stop's single fcut on
a predecessor phase gets rolled back if the scene also has
`forward_infeas_rollback = true`.  Either flag can be set
independently.

---

## 4. Tests

A dedicated `test/source/test_sddp_forward_infeas_rollback.cpp`:

1. **Rollback removes this iteration's cuts.**  Build a 2-scene 5-phase
   fixture where scene 0 is forced infeasible at iter 0 (e.g. tighten
   reservoir initial volume below the demand floor on a chosen phase).
   Drive `SDDPMethod::solve` for one iteration with
   `forward_infeas_rollback = true`.  Assert:
   - `m_cut_store_.scene_cuts()[0]` size after iter 0 == size at iter
     0 start.
   - LP row count for every (0, p) cell is unchanged from pre-iter.

2. **Cuts on OTHER scenes survive rollback.**  Same fixture, scene 1 is
   feasible.  Assert `m_scene_cuts_[1]` grew (backward pass added
   optimality cuts), and S=0's LPs received the shared cuts from S=1
   via `share_cuts_for_phase`.

3. **Stall-stop fires when no new cuts arrive.**  Single-scene 3-phase
   fixture, force iter 0 infeasibility, assert `solve()` returns
   `Error { SolverError, "no recovery path" }` after iter 1 dispatch
   (not iter 0 — iter 0 sets the snapshot, iter 1 sees no progress).

4. **Retry succeeds when shared cuts arrive.**  2-scene fixture with
   `cut_sharing = accumulate`.  Scene 0 infeasible iter 0, scene 1
   feasible.  Iter 1: scene 0 retries (snapshot grew), succeeds with
   the shared cuts in place.  Assert `solve()` converges, and that the
   final per-scene cut counts include the iter-1 backward-pass cuts on
   both scenes.

5. **Default OFF preserves legacy behaviour.**  Same as test 3 but with
   `forward_infeas_rollback = false`.  Assert `solve()` does NOT abort
   with the new error; it follows the existing legacy path (which may
   loop or eventually converge on a different criterion).  Pins the
   opt-in semantics.

6. **PLP-style backtrack chain rolled back atomically.**  Force a scene
   to install fcuts on phases p, p-1, p-2 via the elastic backtrack
   chain, then declare infeasible at p-3.  Assert all three fcuts are
   removed from `m_scene_cuts_` AND from each LP cell.

All six tests use the existing `make_*_planning` helpers in
`sddp_helpers.hpp` plus a new `make_forced_infeas_2scene_planning`
helper that hard-codes a guaranteed iter-0 infeasibility for a
designated scene.

---

## 5. Migration sequence

| # | Change | Tests | Risk |
|---|---|---|---|
| 1 | Add `SDDPOptions::forward_infeas_rollback` (default `false`); plumb through JSON + CLI; no behaviour change. | Schema tests; CLI parse test. | none |
| 2 | Add `SceneRetryState` and `m_scene_retry_state_`; populate `cuts_at_iter_start` at forward dispatch.  Still no rollback fires; just observation. | Unit test: snapshot equals `m_scene_cuts_[s].size()` at dispatch. | none |
| 3 | Implement `rollback_scene_cuts` and wire it into the forward-pass post-processing under the new flag. | Tests 1, 2, 6 above. | medium — interacts with `m_active_cuts_` and per-scene LP row indices; needs careful row-batch deletion |
| 4 | Implement stall-stop predicate at next-iteration dispatch. | Tests 3, 4. | low |
| 5 | Add documentation in `docs/formulation/mathematical-formulation.md` and a one-line release-notes entry. | none | none |
| 6 | (post-soak) Flip default to `true` after at least one full juan/gtopt_iplp + IEEE benchmark run. | Test 5 already pins legacy behaviour under the OFF flag. | medium — would change the failure mode for any pre-existing infeasible-loop case |

Steps 1-5 land in five focused commits; step 6 is deferred until the
feature has soaked.

---

## 6. Open questions to resolve before kickoff

1. **Should rollback also drop cuts that the scene received via
   `share_cuts_for_phase` during iter k?**  The proposed semantics
   only roll back cuts in `m_scene_cuts_[S]` (i.e. cuts S generated).
   Shared cuts S received from other scenes live as LP rows on S but
   in `m_scene_cuts_[other]` — they survive S's rollback.  This is
   the correct invariant for stall-detection (other scenes' progress
   should count) but may leave stale cuts on S's LP if the iter's
   shared cuts were poorly conditioned.  Author leans toward leaving
   them — they're not S's responsibility to invalidate.
2. **Should the stall-stop be a soft fall-back (e.g. switch to
   `cut_sharing=max` on its own) instead of a hard error?**  Author
   leans toward hard error: a soft fallback is a bigger semantic
   change and warrants a separate design.
3. **Per-iteration partial rollback** — should a scene that succeeded
   forward but with `forward_max_attempts` retries also have its
   "wasted" intermediate fcuts rolled back?  Author leans NO: those
   fcuts represent legitimate recovery points and removing them
   would reintroduce the same infeasibility next iter.  Only roll
   back when the scene was *declared infeasible* end-to-end.

---

## 7. What this plan does NOT change

- `SDDPCutStore` data structure (per-scene vectors, snapshot offset).
- `SDDPMethod::store_cut` / `add_cut_row` / `free_alpha_for_cut`
  semantics — fully reused.
- `LinearInterface::delete_rows` / `record_cut_deletion` — already
  has the correct semantics; the rollback hook is just a new caller.
- The `forward_fail_stop` and `forward_max_attempts` options — they
  remain orthogonal to `forward_infeas_rollback`.
- Aperture / cascade / boundary-cut paths — none of them touch the
  per-scene cut store mid-iteration.
