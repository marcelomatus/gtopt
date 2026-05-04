# SDDP × LowMemoryMode::compress refactor + fix plan

> **Status (2026-05-03)**: root cause identified and a minimal fix applied
> (commit `5e679f01` + backward-pass `update_lp_for_phase` call inside
> `backward_pass_single_phase`).  This document captures the broader
> refactoring needed to make the lifecycle robust.

## 1 · Root cause (confirmed)

`SDDPMethod::backward_pass_single_phase` calls `tgt_sys.ensure_lp_built()`
followed by `tgt_li.resolve(opts)` without `update_lp_for_phase` in
between.  Under `LowMemoryMode::compress`, `ensure_lp_built()` reloads
the **construction-time** structural LP from the snapshot — the
`update_lp` mutations (turbine production factor, seepage, discharge
limit, …) live only on the volatile backend that was dropped at the
previous `release_backend()`.

Concrete trace on juan/iplp:

| Path                      | `turbine_conversion_*_54_*` coeff |
|---------------------------|-----------------------------------|
| Forward iter 0+ (after `update_lp_for_phase`) | `0.97611974713196` |
| Backward iter 0+ (before fix, after reload only) | `0.97522168125` (construction-time) |

The cuts built from the backward solve encode the wrong value-function
geometry → juan plateaus at gap = 133.78 % from iter 2, LB stuck at
−695 M.

`source/sddp_method_iteration.cpp:432-479` — pre-fix source of bug.

## 2 · Minimal fix applied

Inside `backward_pass_single_phase`, between
`tgt_sys.ensure_lp_built()` and `tgt_li.resolve(opts)`, call
`update_lp_for_phase(scene_index, phase_index)` so the backward LP
operates on the same dynamically-patched matval as the forward.

**Cost**: under compress this re-triggers the throw-away flatten in
`rebuild_collections_if_needed` per backward step.  ~50 % runtime
overhead measured on juan (≈ 30 s/iter → ≈ 60 s/iter).  Acceptable
short-term; addressed by the structural refactor below.

## 3 · Invariants that should be enforced

1. **"Reload-without-update is invalid for any solve"** — under
   `low_memory != off`, calling `resolve()` after `ensure_lp_built()`
   without an intervening `update_lp_for_phase()` produces an LP whose
   coefficients lag the forward-pass trajectory.
   *Where relied on*: every SDDP solve site
   (`sddp_forward_pass.cpp`, `sddp_method_iteration.cpp`,
   `sddp_aperture_pass.cpp`).
   *Enforcement*: a `LiPhase` state machine that disallows
   `LiPhase::Reconstructed → resolve()` directly; `update_lp` must
   transition through `LiPhase::Updated` first.

2. **"Snapshot is frozen at construction time"** — `m_snapshot_.flat_lp`
   reflects the structural matval at flatten time.  No code path bakes
   `update_lp` mutations or `set_coeff` calls back into it.
   *Where relied on*: every `reconstruct_backend` fast-path
   (`linear_interface.cpp:360`).
   *Enforcement*: rename `m_snapshot_` to `m_structural_snapshot_` and
   surface a `bool snapshot_includes_updates() const` returning `false`,
   documenting the invariant.  Or: add an `apply_updates(span<const
   double> matval_patch)` API and require callers to use it.

3. **"`is_optimal()` reflects the live backend, not the cache"** — under
   compress post-reconstruct-pre-resolve, `m_backend_->is_proven_optimal()`
   returns `false` even when the cell is legitimately optimal from a
   prior solve.  Pre-solve readers (`physical_eini` / `physical_efin`)
   then fall through to defaults.
   *Where relied on*: `linear_interface.cpp:2698`,
   `storage_lp.hpp:362,446`.
   *Enforcement*: a `m_backend_solution_fresh_` flag (attempted in this
   session, currently reverted) — but only useful once invariant #2 is
   resolved.

4. **"Wrapper indices match the live backend's column ordering"** — the
   throw-away flatten in `rebuild_collections_if_needed` produces
   wrappers whose `(row, col)` indices must equal what's actually in
   the live backend after `load_flat + apply_post_load_replay`.
   *Where relied on*: `system_lp.cpp:932`, every
   `set_coeff(ci.row, ci.col, …)` in element `update_lp` overrides.
   *Enforcement*: a debug-build invariant check that compares the
   throw-away flatten's column count with the live backend's
   `get_num_cols() - dynamic_cols.size()`.

5. **"`m_collections_built_` ⇔ `m_collections_` non-empty"** — the
   flag and the actual content can drift via the
   `RebuildPassGuard` paths.
   *Where relied on*: every `elements<X>()` / `element<X>(id)` access
   (`system_lp.hpp:312-348`).
   *Enforcement*: replace the bare `bool` with a class invariant on
   `m_collections_` itself (e.g. `Collections` class with an `empty()`
   accessor, no separate flag).

6. **"`PhaseGridRecorder::record` and SDDP log keys use UID
   convention"** — done in this session: `record(IterationUid,
   SceneUid, PhaseUid, GridCell)` plus `sddp_log` always renders
   `uid_of(iteration_index)`.

## 4 · Proposed module shape (pick: BackendSession RAII guard)

A single guard owned by SDDP that wraps the protocol so forward and
backward share the same code path:

```cpp
// Sketch — not implemented:
class BackendSession {
 public:
  BackendSession(SystemLP& sys, IterationIndex iter, SceneIndex scene,
                 PhaseIndex phase)
      : m_sys_(sys), m_iter_(iter) {
    sys.ensure_lp_built();
    sys.update_lp_for_phase_if_needed(scene, phase, iter);
    // m_phase_ is now LiPhase::Updated; resolve() is allowed.
  }
  ~BackendSession() {
    // Optional: release_backend() + cache scalars for next ensure_lp_built.
  }

  LinearInterface& li() { return m_sys_.linear_interface(); }
  std::expected<int, Error> resolve(const SolverOptions& opts) {
    return li().resolve(opts);
  }
};

// Forward pass: BackendSession sess(system, iter, scene, phase);
//               sess.resolve(opts);
//
// Backward pass: BackendSession sess(tgt_sys, iter, scene, phase);
//                sess.resolve(opts);
//
// Aperture: BackendSession sess(target_sys, iter, scene, phase);
//           sess.resolve(opts);
```

**Why this design**: the bug just fixed is structurally impossible —
you can't construct a `BackendSession` without going through
`update_lp_for_phase_if_needed`.  The `Updated` precondition for
`resolve()` is a runtime invariant baked into the type.

The `*_if_needed` part respects the existing
`should_dispatch_update_lp` skip-policy.

## 5 · Permanent trace instrumentation

Add the following `spdlog::trace(...)` lines (function form — the
`SPDLOG_TRACE` macro is compiled out by the PCH, per session
feedback memory `feedback_spdlog_trace_macro`):

| File:line | Format | Fields |
|-----------|--------|--------|
| `linear_interface.cpp:181` (top of `release_backend`) | `"LI release [iter={} mode={}]: numrows={} numcols={} cached_optimal={} cached_col_sol_size={} active_cuts={}"` | iter, mode_label, numrows, numcols, is_optimal, col_sol cache size, active_cuts.size() |
| `linear_interface.cpp:382` (after `apply_post_load_replay` in `reconstruct_backend`) | `"LI reconstruct [iter={} mode={}]: replayed dynamic_cols={} dynamic_rows={} active_cuts={} numrows={} numcols={}"` | iter, mode, replay counts, post-load row/col counts |
| `sddp_method_iteration.cpp:255` (top of `update_lp_for_phase`) | `"SDDP UpdateLP [i{} s{} p{}]: prev_phase_optimal={} default_eini_sample={:.3f} efin_read={:.3f}"` | iter_uid, scene_uid, phase_uid, optional prev-phase optimality flag, sample reservoir's default & read efin |
| `sddp_method_iteration.cpp:307` (bottom of `update_lp_for_phase`) | `"SDDP UpdateLP [i{} s{} p{}]: updated {} elements in {:.3f}ms"` | iter_uid, scene_uid, phase_uid, count, dt |
| `linear_interface_solve.cpp:175` (after `m_backend_->resolve()` in `LinearInterface::resolve`) | `"LI resolve done [iter={}]: status={} obj={} kappa={:.2e} dt={:.3f}ms"` | iter, status, obj_value, kappa, elapsed |
| `system_lp.cpp:949` (top of `rebuild_collections_if_needed` after the early-return checks) | `"SystemLP rebuild_collections [scene={} phase={}]: rebuilding (mode={})"` | scene_uid, phase_uid, mode |
| `sddp_forward_pass.cpp:125` (after `system.ensure_lp_built()`) | `"SDDP Forward [i{} s{} p{}]: ensured backend (mode={} backend_released_at_entry={})"` | i_uid, s_uid, p_uid, mode, prev-state flag |
| `sddp_method_iteration.cpp:441` (after `tgt_li.resolve` in backward) | `"SDDP Backward resolve [i{} s{} p{}]: status={} obj={} kappa={:.2e}"` | i_uid, s_uid, p_uid, status, obj, kappa |

These run at TRACE level (zero overhead in INFO builds via spdlog's
runtime level filter).  Activated via `--trace-log=path` or
`spdlog::set_level(trace)`.

## 6 · Unit-test plan

**Test 1 — `LinearInterface` snapshot is frozen at construction**.
File: `test/source/test_linear_interface_lowmem.cpp`.
Setup: build a 2×2 LP, `set_low_memory(compress)`, `save_snapshot(flat)`,
`set_coeff(0, 0, 99.0)`, `release_backend()`, `reconstruct_backend()`.
Assert: backend's `(0, 0)` coefficient is the **original** value (not
99.0) — documents the known invariant.

**Test 2 — `update_lp_for_phase` is idempotent under compress**.
Fixture: 2-phase hydro (`make_simple_hydro_lp`), one
`ReservoirProductionFactorLP` element.
Steps:
1. Build under compress.
2. `update_lp_for_phase(s0, p0)` → record coeff = X.
3. `release_backend()`.
4. `ensure_lp_built()`.
5. `update_lp_for_phase(s0, p0)` → assert coeff == X (idempotent).
6. WITHOUT calling step 5, read coefficient directly →
   assert it equals **construction-time** value (snapshot pins it).
   This pins the "snapshot is stale" invariant.

**Test 3 — Backward and forward see the same coefficient under
compress**.
Fixture: 3-phase hydro.
Steps: run iter 0 forward, then run iter 0 backward.  Inspect the
turbine_conversion coefficient on `(scene 0, phase 0)`'s LP at:
- end of forward p0,
- start of backward p1's `tgt_li.resolve` (i.e., after
  `tgt_sys.ensure_lp_built()` + `update_lp_for_phase`).
Assert both values are equal.  This is the **direct regression guard**
for the bug just fixed.

**Test 4 — `LB ≤ UB` holds at every iteration under all modes**.
Fixture: any multi-phase hydro (the existing `test_sddp_method.cpp`
fixtures qualify).  Run for 5 iterations under
`{off, compress}` and assert `gap_after_iter[k] >= -kSddpGapFpEpsilon`
at every iter.  Integration-level guard.

## 7 · Phased implementation plan

| Phase | Effort | Description | Done-when |
|-------|--------|-------------|-----------|
| P1 | S | Apply backward `update_lp_for_phase` fix. | **DONE** (this session) |
| P2 | S | Add the unit tests in §6 above. | All four tests pass on `off`; tests 1-3 pass on `compress`; test 4 pinned. |
| P3 | M | Optimize `rebuild_collections_if_needed` cost under compress (cache the post-rebuild wrappers across release/reconstruct cycles within an iteration). | juan compress runtime within 30 % of off mode. |
| P4 | M | Bake `update_lp` mutations into the snapshot so subsequent `reconstruct_backend` doesn't need a re-update (resolves invariant #2). | Test 1 expectations flip — coefficient *does* survive. |
| P5 | L | `BackendSession` RAII guard introduced; forward / backward / aperture all migrate. | Bug class structurally impossible. |
| P6 | M | Permanent trace instrumentation in §5 lands. | Logs visible with `--trace-log` on a representative juan run. |
| P7 | S | Remove the `m_collections_built_` boolean and the `RebuildPassGuard` once the snapshot includes update mutations. | Class invariant on `Collections`; `elements<X>()` no longer throws because there's no "dropped" state. |

P1 + P2 ship the user-visible fix.  P3 restores performance.  P4
reduces the CPU + memory pressure of the workaround.  P5 prevents the
class of bug from recurring.  P6/P7 round out the modernization.

## 8 · References

- Numerics agent's full diagnosis: in-conversation report dated 2026-05-03.
- Commit `5e679f01` — partial fix (rebuild_collections_if_needed,
  drop_cached_primal_only removal, warm-start cleanup).
- Commit (pending) — backward `update_lp_for_phase` call in
  `backward_pass_single_phase`.
- Memory `feedback_spdlog_trace_macro` — use `spdlog::trace(...)` not
  `SPDLOG_TRACE(...)` macro.
- Memory `project_juan_sddp_regression` — original session that
  surfaced the issue.
