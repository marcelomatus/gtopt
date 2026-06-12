# LinearInterface Lifecycle & Low-Memory API Refactor Plan

**Status:** draft — pending review
**Date:** 2026-04-30
**Author:** marce (with Claude review)
**Trigger:** `support/juan/gtopt_iplp` SDDP run blew up with
`generate_labels_from_maps: row 16974 has no entry in m_row_labels_meta_`
because `LinearInterface::add_rows(span<SparseRow>)` (the bulk path used
in `apply_post_load_replay`) skipped `track_row_label_meta`. The fix
landed in `source/linear_interface.cpp:1362-1382`. The fact that none of
the existing 10-phase / 2-reservoir / multi-mode SDDP regression tests
caught it points at a deeper API problem: the low-memory lifecycle is
spread across many ad-hoc methods with implicit ordering, and the type
system enforces nothing.

---

## 1. Current state

### 1.1 Public API surface (today)

`include/gtopt/linear_interface.hpp` exposes the following methods that
together control the low-memory lifecycle:

| Method | Purpose |
|---|---|
| `set_low_memory(mode, codec)` | declare intent only — no state change |
| `save_snapshot(FlatLinearProblem)` | own a copy of the rebuild source |
| `defer_initial_load(FlatLinearProblem)` | pre-seed snapshot, skip eager `load_flat` |
| `clear_snapshot()` | drop the snapshot, leave cache live |
| `release_backend()` | discard backend, optionally compress meta |
| `reconstruct_backend(col_sol, row_dual)` | rehydrate from snapshot |
| `mark_released()` | rebuild-mode bypass |
| `save_base_numrows()` | freeze structural-vs-cuts row boundary |
| `set_base_numrows(n)` | restore boundary after rollback |
| `record_dynamic_col(SparseCol)` | log alpha cols for replay |
| `record_cut_row(SparseRow)` | log cuts for replay |
| `record_cut_deletion(span)` | log deletions for replay |
| `take_dynamic_cols / restore_dynamic_cols` | rollback |
| `take_active_cuts / restore_active_cuts` | rollback |
| `add_col_disposable / add_row_disposable` | clone-local extras |

That is **17 user-facing methods** for what is conceptually one state
machine. Most call orders are not enforced at the type level. Several
methods are pre-conditioned on the value of `m_low_memory_mode_` and
silently degenerate to no-ops or wrong behaviour when the assumption is
violated.

### 1.2 Concrete fragility surfaces

**F1 — `set_low_memory` is a *promise*, not a *commitment*.**
It only flips `m_low_memory_mode_`. If the caller then drops the
backend without first calling `save_snapshot`, `reconstruct_backend`
silently early-returns and `m_active_cuts_` accumulates rows the LP
will never see. The "clear_snapshot drops flat LP but keeps cache"
test (`test_linear_interface_lowmem.cpp:627`) actively relies on this
silent semantic.

**F2 — `save_base_numrows` has implicit phase ordering.**
Must be called *after* the structural build (`load_flat`) and *before*
the first cut row. Calling it twice silently reseats the boundary;
forgetting it makes `apply_post_load_replay` eat rows from the wrong
side. The bulk-replay bug we just fixed is a direct symptom.

**F3 — Cut addition is a two-call pair.**
Most callers do
```cpp
li.add_row(cut);          // mutates backend + maybe metadata
li.record_cut_row(cut);   // mirrors into m_active_cuts_
```
`add_cut_row` exists as a single-call wrapper but is not used
universally; CSV/JSON/boundary loaders still do the pair manually. Any
forgotten `record_cut_row` produces a cell that solves correctly once
and then loses the cut on the next reconstruct cycle. There is no
way to detect this until the next `release_backend → reconstruct →
solve` round trip, possibly hours later in an SDDP run.

**F4 — `clear_snapshot` is a third state.**
After `clear_snapshot`, the cache is alive but the snapshot is gone
and `reconstruct_backend` becomes a silent no-op. Code reading the
LP through the cache works; code expecting a live backend fails
with a hard-to-debug nullptr.

**F5 — Boundary-condition coupling between paths.**
`apply_post_load_replay` reads `m_base_numrows_` to decide where cut
rows start. The metadata invariant "every backend row has an entry in
`m_row_labels_meta_` *or* `m_post_clone_row_metas_`" is enforced only
in `generate_labels_from_maps` (i.e. at output time, far away from
the call that violated it). The bulk add path violated this for
months without any test catching it because no regression test
combined `low_memory=compress` + reconstruct + label query.

**F6 — Disposable / clone path is correct but undocumented.**
`add_col_disposable` / `add_row_disposable` write to per-clone-local
`m_post_clone_*_metas_` and never touch shared metadata. This is
the right design for the elastic filter and aperture clones, but
the contract is *implicit* — there is no compile-time signal that
these methods should only be called on a shallow clone.

### 1.3 Three orthogonal call sites that mutate "temporary" rows/cols

The refactor must preserve these three paths without forcing them into
a shared state machine they don't need:

**(A) Structural build + cuts on the original (planning) LP.**
Goes through `load_flat` → `save_base_numrows` → many `add_row(cut)`
calls under low-memory. The bulk replay (`apply_post_load_replay →
add_rows(m_active_cuts_)`) is on this path. **This is the path the
bug lived on.**

**(B) Elastic filter clone (`benders_cut.cpp`).**
```cpp
auto cloned = li.clone(LinearInterface::CloneKind::shallow);
cloned.add_col_disposable(slack_sup);
cloned.add_col_disposable(slack_sdn);
cloned.add_row_disposable(fixing_row);
cloned.resolve(opts);
// clone destroyed
```
The clone is throw-away. Bounds and obj coefficients are mutated
through the backend; metadata is only added via the disposable
APIs which write to clone-local extras. **Never enters the
low-memory replay path** (the clone has no snapshot, no
`save_base_numrows`, no `record_cut_row`). On clone destruction, the
slacks/fixing-row disappear with the backend.

**(C) Aperture solve clone (`sddp_aperture.cpp`).**
```cpp
LinearInterface clone = phase_li.clone(CloneKind::shallow);
// update_aperture: only mutates bounds + obj coeffs via backend
clone.resolve(aperture_opts);
// clone destroyed
```
**No disposable cols/rows are added** — only bounds and objective
coefficients are mutated. Even the disposable-extras path stays
empty. This is the simplest of the three.

The refactor in §2 below explicitly preserves (B) and (C). The
disposable APIs become formal "clone-only" methods; the lifecycle
state machine in §2.1 only applies to path (A).

---

## 2. Proposed refactor

### 2.1 A single explicit lifecycle state on the original LP

Add one enum and one transition method:

```cpp
enum class LiPhase : uint8_t {
  Building,        // accepts add_col, add_row, set_coeff
  Frozen,          // structural rows fixed; cuts may be added
  BackendReleased, // low-memory release; only metadata + replay state alive
  Reconstructed,   // backend reloaded from snapshot + replay log
};

[[nodiscard]] LiPhase phase() const noexcept;
```

State transitions become method-driven and asserted in debug:

| From | To | Method |
|---|---|---|
| (none) | `Building` | constructor / `load_flat` |
| `Building` | `Frozen` | **new** `freeze_for_cuts(LowMemoryMode, FlatLinearProblem, codec)` |
| `Frozen` | `BackendReleased` | `release_backend` (only valid post-freeze) |
| `BackendReleased` | `Reconstructed` | `reconstruct_backend` |
| `Reconstructed` | `BackendReleased` | `release_backend` |

The new `freeze_for_cuts` collapses the **3-call dance** into one:

```cpp
li.freeze_for_cuts(LowMemoryMode::compress,
                   FlatLinearProblem {flat},
                   CompressionCodec::lz4);
// After this call:
//   - m_low_memory_mode_  == compress
//   - snapshot stored + (optionally) compressed
//   - m_base_numrows_     = m_backend_->get_num_rows()
//   - m_base_numrows_set_ = true
//   - phase()             == Frozen
//   - the type rejects further structural mutation in debug builds
```

**Backward compatibility.** Keep `set_low_memory`, `save_snapshot`,
`save_base_numrows` as `[[deprecated]]` shims that route through
`freeze_for_cuts`. Both paths advance `LiPhase` identically. Existing
callers continue to work; new callers and migrated SDDP/cascade code
get the safety properties for free.

### 2.2 Collapse the cut-addition pair

Make `add_cut_row` the one true cut entry-point and forbid the manual
pair under the new state. Concretely:

- Rename the public `add_row(SparseRow, eps)` to `add_structural_row`
  and keep a `[[deprecated]]` alias.
- Make `add_structural_row` assert `phase() == Building` in debug;
  outside debug, allow it but issue an spdlog warning the first time.
- `add_cut_row(SparseRow, eps)` asserts `phase() == Frozen ||
  phase() == Reconstructed`. It already wraps the
  `add_row + record_cut_row` pair internally
  (`linear_interface.hpp:757`); we just need every caller to stop
  doing the pair manually.

A grep over `--include="*.cpp" record_cut_row` is the migration list:
`source/sddp_cut_csv.cpp`, `source/sddp_cut_json.cpp`,
`source/sddp_named_cuts.cpp`, `source/sddp_boundary_cuts.cpp`,
`source/benders_cut.cpp`, plus the cut-replay path itself. All of
them already construct a `SparseRow cut`; replacing
`li.add_row(cut); li.record_cut_row(cut);` with
`li.add_cut_row(cut, eps);` is a 2-line per-site change.

The bulk variant `add_rows(span<SparseRow>)` becomes private (used
only by `apply_post_load_replay`) and is renamed
`replay_cut_rows_bulk` so its single legitimate caller is obvious.
The fix from this session lives there permanently.

### 2.3 Disposable APIs become formally clone-only

`add_col_disposable` / `add_row_disposable` are correct today but
their "this only makes sense on a shallow clone" contract is
implicit. Two options, listed in increasing cost:

**Option (i) — runtime guard (cheap, recommended).**
Add a `bool m_is_clone_ = false;` flag set by `LinearInterface::clone`.
The disposable methods `assert(m_is_clone_)`. Production builds can
keep the assert; the cost is one bool per LP. Catches any future
caller that tries to use disposable on the original LP at the
nearest call site instead of at output time.

**Option (ii) — strong-type split (bigger).**
Introduce `LinearInterfaceClone` as a thin wrapper over
`LinearInterface` that exposes `add_col_disposable` /
`add_row_disposable` and *not* the structural / cut methods.
`LinearInterface::clone` returns `LinearInterfaceClone` instead.
The compiler enforces the split. Higher migration cost (every
elastic / aperture call site changes its variable type), so defer
to a later phase unless the runtime guard misses cases.

We start with option (i). Promote to (ii) only if a real bug shows
up that the runtime guard didn't catch.

### 2.4 Enforce the metadata size invariant

Independent of the lifecycle change, add a small invariant check
that fires whenever the backend row count grows. This is a defence
in depth — if some new code path ever adds a row without metadata
again, we want to know at the call site, not at `write_lp` time:

```cpp
// in any public add_row* / add_rows*:
DEBUG_INVARIANT(
    m_backend_->get_num_rows()
    == static_cast<int>(m_row_labels_meta_->size()
                        + post_clone_row_meta_count())
);
```

`DEBUG_INVARIANT` is a `[[gnu::assume]]` in release / `assert` in
debug. Cost: one comparison per row addition. Catches the bulk-add
class of bugs at insertion time rather than at output time.

### 2.5 Aperture / elastic clone path: verify, don't change

The disposable + shallow-clone design is correct today. The plan
*does not* introduce any state machine on clones. Verify with one
new test per path:

- `test_benders_cut_elastic.cpp`: after
  `elastic_filter_solve`, `clone.materialize_labels()` succeeds and
  the slack labels are present. (Currently tested only via the
  consumer; this would be an explicit invariant check.)
- `test_sddp_aperture.cpp`: after an aperture clone solves,
  `clone.materialize_labels()` succeeds. Aperture clones add no
  rows/cols — the test confirms label inheritance works.

These are read-only assertions. They will pass against the current
code (the labels path on clones is correct) and will keep passing
after the refactor as long as we leave the clone path alone.

---

## 3. Migration sequence

Each step lands as its own commit. Steps 1-3 are fully back-compat;
step 4 introduces the deprecated-shim layer; step 5 removes the
shims after a quiet period.

| # | Change | Tests | Risk |
|---|---|---|---|
| 1 | Add `freeze_for_cuts(mode, flat, codec)` as a thin wrapper that calls `set_low_memory + save_snapshot + save_base_numrows`. No deprecation yet. | Add unit test mirroring the recipe. | none |
| 2 | Add `LiPhase` enum + `phase()` accessor; populate from existing methods. No assertions yet, just observation. | Add unit test that tracks phase across full lifecycle. | none |
| 3 | Migrate one SDDP cut loader (start with `sddp_cut_json.cpp`) from the manual pair to `add_cut_row`. Repeat for the others. | Existing CSV/JSON round-trip tests. | low |
| 4 | Add debug assertions on `LiPhase` transitions + `m_is_clone_` runtime guard on disposable APIs. Add `DEBUG_INVARIANT` for the metadata-size invariant. | Run the full SDDP suite under debug build. | medium — may surface latent ordering bugs we'll need to fix |
| 5 | Mark `set_low_memory`, `save_snapshot`, `save_base_numrows` `[[deprecated]]`. Migrate every caller in source/, test/, integration_test/. | Full ctest. | low |
| 6 | Convert `add_rows(span<SparseRow>)` to private `replay_cut_rows_bulk`; remove the old name. | Full ctest. | low |
| 7 | (optional, deferred) Promote runtime clone guard to `LinearInterfaceClone` strong type. | Full ctest. | high — wide blast radius; only do if step 4 misses real bugs |

Steps 1–5 should land within a single PR each; step 6 in a separate
cleanup PR. Step 7 deferred indefinitely.

---

## 4. Test gap closure (independent of the API change)

The bug went undetected because no regression test combined
`low_memory=compress` + reconstruct + label query. Even before the
API refactor lands, add these tests:

- **Already added (this session):** `test_linear_interface_lowmem.cpp`
  — `LinearInterface — bulk add_rows replay preserves row label
  metadata`. Reproduces the exact failure mode at the unit level.
- **Add to `test_sddp_method.cpp`:** at the end of the existing
  `"SDDPMethod low_memory_mode invariance — 10-phase 2-reservoir
  hard + dual±1 soft"` test, after the solve completes, call
  `linear_interface().materialize_labels()` on a phase that took
  cuts. This makes the existing 10-phase / multi-mode invariance
  test catch the bug class at the integration level.
- **Add to `test_sddp_boundary_cuts_low_memory_replay.cpp`:** after
  the `release_backend → reconstruct_backend → resolve` cycle, call
  `materialize_labels` and a cheap `write_lp` to a temp path. The
  existing test only verifies numeric correctness; this adds the
  label invariant that gtopt_iplp tripped over.

---

## 5. What the plan deliberately does *not* change

- **Backend cloning semantics.** `clone(CloneKind)` stays as is.
  The deep/shallow split is well-justified by the elastic + aperture
  paths and not part of this lifecycle problem.
- **`m_active_cuts_` data structure.** Replaying cuts via a stored
  vector of `SparseRow` is the right design — the bug was the bulk
  add path forgetting metadata, not the storage choice.
- **Element-level update calls** (`update_aperture`, bound mutations
  in aperture / elastic). Those operate on the backend directly and
  are orthogonal to the lifecycle state machine.
- **Anything in `LinearProblem` (the builder).** This refactor lives
  entirely in `LinearInterface`.

---

## 6. Open questions to resolve before kickoff

1. Should `clear_snapshot` survive the refactor, or fold its
   semantics into a 5th `LiPhase::CacheOnly`? Currently it leaves the
   LP in a half-state. (Author leans toward keeping it but renaming
   it to `cacheonly_drop_snapshot` and adding a phase.)
2. Should the disposable runtime-guard assertion crash or just
   spdlog::warn under release? (Author leans toward `assert` —
   any caller hitting this is broken.)
3. Should `add_structural_row` warn or error in release when called
   in `Frozen` phase? Some test fixtures intentionally extend the
   LP after freeze; a hard error breaks them.
