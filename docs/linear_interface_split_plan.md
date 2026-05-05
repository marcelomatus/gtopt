# `LinearInterface` split — implementation plan

**Status:** Phase 1, 2a, 2b landed.  Remaining phases re-evaluated (see
"Stopping here" below).
**Date:** 2026-05-04
**Source:** B2 from `docs/improvement_recommendations.md` (~1.5 weeks).
**Companion plans:**
* `docs/solver_backend_span_out_plan.md` — Phase 2 of the *plugin*-side
  split (already landed via commits `a64d9ecc`, `05523154`, `b90eb971`,
  `13cd3447`).  This plan is the matching *LinearInterface*-side split.
* `docs/analysis/investigations/linear_interface/linear_interface_lifecycle_plan_2026-04-30.md` — earlier
  doc on the bulk-`add_rows` / label-meta failure mode that motivated
  the lifecycle subsystem boundary used here.

## Progress tracker

| Phase | Class | Status | PR |
|-------|-------|--------|----|
| 1     | `LpCache`         | ✅ landed | [#461](https://github.com/marcelomatus/gtopt/pull/461) |
| 2a    | `LpReplayBuffer`  | ✅ landed | [#462](https://github.com/marcelomatus/gtopt/pull/462) |
| 2b    | `LpSnapshotHolder` (snapshot + codec + compression) | ✅ landed | [#463](https://github.com/marcelomatus/gtopt/pull/463) |
| 2c    | `LpBackendOwner` (backend ptr + released + phase + ensure/reconstruct) | 🛑 deferred — see "Stopping here" |
| 3     | `LpSolver` (initial_solve / resolve / fallback) | 🛑 deferred — see "Stopping here" |
| 4     | `LpModel` (structural LP + label meta + scaling) | 🛑 deferred — see "Stopping here" |

After Phase 2b, `LinearInterface` extracted **3 distinct
subsystems**:

* `LpCache` — post-solve scalars + col_sol/cost/row_dual.  Invariants C1–C8.
* `LpReplayBuffer` — dynamic cols/rows, active cuts, pending col bounds, replay flag.  Invariants R1–R6.
* `LpSnapshotHolder` — flat-LP snapshot + compression codec + compress/decompress lifecycle.  Invariants S1–S5.

24 isolated unit tests (168 assertions) pin every invariant in
isolation.  `LinearInterface` is ~6 fields lighter and the code
formerly tangled in 5700 lines of mixed concerns now has 3 named
subsystems with documented contracts.

## Stopping here — and why

Phase 1 + 2a + 2b extracted the **orthogonal** subsystems — each
owns state that is not read or written by any other subsystem
across the LinearInterface API.  The remaining state is genuinely
*entangled*:

* `m_backend_` (the live solver) is dereferenced at ~200 sites
  inside `LinearInterface` methods.  Wrapping it in `LpBackendOwner`
  would force every call to become `m_owner_.backend()->X()` (or
  add forwarding macros) — pure churn, no semantic win.
* `m_low_memory_mode_` is a gate-checked flag at ~30 scattered
  sites.  Moving it into a holder forces the same forwarding
  pattern.
* `m_rebuilding_` / `m_rebuild_owner_` are tightly coupled to
  `ensure_backend` — the rebuild logic *is* the lifecycle logic.
  Extracting the hook into its own class would actually complicate
  the API rather than simplify it (the rebuild path needs reentrant
  access to backend + flag + replay buffer + cache, which are then
  scattered across multiple holders).
* The structural LP (`add_col` / `add_row` / `set_*` / label-meta /
  scaling) lives across both `LinearInterface` and the
  `SolverBackend` plugin layer — the natural split (B2's `LpModel`)
  would mostly be a code move within the file, not a meaningful
  re-architecture.

The high-leverage extractions are done.  Continued slicing in
the same style would be churn-without-payoff.  Future contributors
should land further refinements only when they are driven by a
concrete bug or perf concern rather than by aesthetic
"shrink-the-class" pressure.

If/when the remaining phases become worthwhile, they should
probably be reconsidered with a fresh perspective — perhaps a
true facade pattern with explicit policy parameters rather than
an accumulation of holders.

---

## 1. Why split

`include/gtopt/linear_interface.hpp` (2 967 lines) +
`source/linear_interface.cpp` (2 988 lines) = **5 955 lines** of mixed
concerns:

| Concern | Lines (approx.) | Failure-mode signature |
|---------|-----------------|------------------------|
| Structural LP + label-meta + scaling | ~2 200 | "matrix coefficient out of range", coefficient-ratio warnings, label-meta drift |
| Solver lifecycle (initial_solve / resolve / fallback) | ~700 | "non-optimal status" loops, kappa explosion, timeouts |
| Low-memory lifecycle (release / reconstruct / replay) | ~1 500 | off↔compress divergence, post-load_replay misses, snapshot decompression hangs |
| Post-solve cache (cached col_sol / col_cost / row_dual / scalars) | ~400 | "cache reads stale value after release", I6 invariant breaks |

Each failure class is self-contained — the off↔compress investigation
lived entirely inside the *lifecycle* concern; the seepage / DRL bug
lived entirely inside the *cache* layer (early-return on stale
state); the matind-sort perf fix lived entirely inside the *plugin
boundary*.  Mixing them in one class:

* Forces every contributor to understand all 4 axes before touching
  any one.
* Makes ownership of state implicit — `m_cached_col_sol_` is read by
  the cache layer, written by the lifecycle layer, invalidated by
  the model layer.  A typo in any of them silently corrupts the
  others.
* Hides the natural test boundaries.  A real cache regression test
  needs only 50 lines if `LpCache` is its own type; today it has to
  spin up a full `LinearInterface` with a backend.

## 2. Target shape (post-split)

```
LinearInterface (thin facade, ~300 lines)
   ├── LpModel       — structural LP + label-meta + scaling (~2 000 lines)
   ├── LpSolver      — initial_solve / resolve / status / fallback (~700 lines)
   ├── LpLifecycle   — release / reconstruct / replay / snapshot (~1 500 lines)
   └── LpCache       — post-solve scalars + col_sol/col_cost/row_dual (~400 lines)
```

Each subsystem owns its members and exposes a focused API.  The
facade aggregates them and handles cross-cutting concerns
(construction, the public API surface that downstream code already
depends on).  The 4 subsystems hold pointers to each other only when
strictly necessary — `LpLifecycle` writes `LpCache` post-solve;
`LpModel`'s mutators call `LpCache::invalidate_optimal_on_mutation`;
`LpSolver` writes `LpCache::set_post_solve(...)` after a successful
solve.  All three of those flows go through small, named helpers
rather than direct member access.

### 2.1 `LpCache` — Phase 1 target (smallest slice, ~400 lines)

State (8 members today, all under `// ── Cached post-solve scalars ──`):

```cpp
class LpCache {
public:
  // Read accessors (consumed by LinearInterface getters)
  [[nodiscard]] double             cached_obj_value()    const noexcept;
  [[nodiscard]] std::optional<double> cached_kappa()     const noexcept;
  [[nodiscard]] Index              cached_numrows()      const noexcept;
  [[nodiscard]] Index              cached_numcols()      const noexcept;
  [[nodiscard]] bool               cached_is_optimal()   const noexcept;
  [[nodiscard]] bool               backend_solution_fresh() const noexcept;
  [[nodiscard]] std::span<const double> cached_col_sol() const noexcept;
  [[nodiscard]] std::span<const double> cached_col_cost() const noexcept;
  [[nodiscard]] std::span<const double> cached_row_dual() const noexcept;

  // Span-out fill targets (LpLifecycle writes here in compress mode)
  [[nodiscard]] std::span<double> col_sol_buffer(size_t ncols);
  [[nodiscard]] std::span<double> col_cost_buffer(size_t ncols);
  [[nodiscard]] std::span<double> row_dual_buffer(size_t nrows);
  void set_post_solve_scalars(double obj, std::optional<double> kappa,
                              Index nrows, Index ncols, bool optimal) noexcept;

  // Mutation hooks (LpModel calls these when the structural LP changes)
  void invalidate_optimal_on_mutation() noexcept;
  void mark_solution_fresh(bool fresh) noexcept;

  // Memory hygiene hooks (LpLifecycle calls these at iteration end)
  void drop_solution_caches() noexcept;            // col_sol / col_cost only
  [[nodiscard]] size_t cache_size_bytes() const noexcept;

private:
  double m_obj_value_ {};
  std::optional<double> m_kappa_ {};
  Index m_numrows_ {};
  Index m_numcols_ {};
  bool m_is_optimal_ {false};
  bool m_backend_solution_fresh_ {false};
  std::vector<double> m_col_sol_ {};
  std::vector<double> m_col_cost_ {};
  std::vector<double> m_row_dual_ {};
};
```

`LinearInterface` becomes:

```cpp
class LinearInterface {
  LpCache m_cache_;          // owned by value
  // existing m_backend_ etc. unchanged in Phase 1
};
```

The 36 read sites that touch `m_cached_*` directly today migrate to
`m_cache_.cached_col_sol()` etc. — purely mechanical (one sed pass).

### 2.2 Subsequent phases

| Phase | Subsystem | Effort | Risk | Pre-condition |
|-------|-----------|--------|------|---------------|
| 1 | `LpCache` | 2 days | low | none — independent of others |
| 2 | `LpLifecycle` | 4 days | medium | Phase 1 done (LpCache writes go through helpers) |
| 3 | `LpSolver` | 3 days | medium | Phase 2 done (lifecycle owns post-solve cache write) |
| 4 | `LpModel` | 3 days | medium-high | Phase 3 done (model mutators call cache invalidator) |

`LpModel` is last because it has the largest call-site surface
(every `add_col` / `add_row` / `set_coeff` / `set_*_bounds` site).
By the time we get there the other 3 subsystems already hide their
own state, so the `LpModel` extract is a pure code move.

## 3. Phase 1 — `LpCache` extract

### 3.1 Files

* `include/gtopt/lp_cache.hpp` (new) — class declaration + inline accessors.
* `source/lp_cache.cpp` (new) — non-trivial methods (currently
  `populate_solution_cache_post_solve` body, `invalidate_cached_optimal_on_mutation`).
  Keep small (~100 lines).
* `include/gtopt/linear_interface.hpp` — replace 8 `m_cached_*` members
  with one `LpCache m_cache_;`.  Replace direct reads with
  `m_cache_.cached_*()` accessors.  Replace direct writes with
  `m_cache_.set_post_solve_scalars(...)` / span-fill helpers.
* `source/linear_interface.cpp` — corresponding migration of writes.
* `test/source/test_lp_cache.cpp` (new) — unit tests for `LpCache` in
  isolation (no backend).  Covers the 8 invariants below.

### 3.2 Invariants `LpCache` must enforce

These are already implicit in `LinearInterface` today; making them
explicit on the new type is half the win.

**C1.**  `cached_is_optimal() == true` ⇒ `cached_col_sol().size() ==
cached_numcols()`.  Asserted in debug, also surfaced via a
`validate_consistency()` method usable from regression tests.

**C2.**  `invalidate_optimal_on_mutation()` ⇒ `cached_is_optimal()
== false`.  Idempotent.  Does NOT clear the col_sol / col_cost
vectors (they remain readable via the empty-fallback path used by
the cut-construction code in compress mode after release).

**C3.**  `drop_solution_caches()` clears `m_col_sol_` and
`m_col_cost_`, leaves `m_row_dual_` and the scalars intact.  Used
post-state-CSV-write (the scalars + row_dual are still needed for
output writing).

**C4.**  `set_post_solve_scalars(...)` is the *only* path that flips
`m_is_optimal_` to `true`.  No public mutator can set it directly.

**C5.**  `mark_solution_fresh(true)` is set inside `timed_solve`
(in `LpSolver` after Phase 3, in `LinearInterface` for Phase 1)
*before* the optimality check.  `mark_solution_fresh(false)` fires
on every `reconstruct_backend()` and `install_flat_as_rebuild()` —
both of which are pre-solve states.

**C6.**  `cache_size_bytes()` reports the actual bytes held
(sum of vector sizes × 8).  Used by the diagnostic
`LinearInterface::cache_size_bytes()` accessor that already exists.

**C7.**  `col_sol_buffer(n)` / `col_cost_buffer(n)` /
`row_dual_buffer(n)` `resize` and return a span — the same shape
the `SolverBackend::fill_*` Phase-2 contract expects.  This is the
only write entry-point for the post-solve vectors in compress mode.

**C8.**  Default-constructed `LpCache{}` reports
`cached_is_optimal() == false`, `cached_obj_value() == 0.0`,
`cached_numrows() == 0`, `cached_numcols() == 0`,
`cached_kappa() == std::nullopt`, all spans empty.  Matches the
LinearInterface initial state exactly.

### 3.3 Migration recipe (mechanical)

1. **Add the new header** + `.cpp`, define `LpCache` with the API
   above + invariants enforced via `assert()` in debug.
2. **Add `LpCache m_cache_;` to `LinearInterface`** (in the
   `private:` block where the cached members live today, around line
   2870 of `linear_interface.hpp`).
3. **Sed-replace** every `m_cached_obj_value_` →
   `m_cache_.cached_obj_value()` (read sites) /
   `m_cache_.set_post_solve_scalars(obj, ...)` (write sites).
   Same for the other 7 fields.  ~80 sites total, all visible in
   `git grep "m_cached_"`.
4. **Replace** `m_cached_col_sol_.resize(ncols); m_backend_->fill_col_sol(m_cached_col_sol_);`
   (and 5 sister sites in `populate_solution_cache_post_solve`)
   with:
   ```cpp
   m_backend_->fill_col_sol(m_cache_.col_sol_buffer(ncols));
   ```
5. **Delete** the 8 raw members from `LinearInterface`.
6. **Add `test/source/test_lp_cache.cpp`** with one test per
   invariant C1–C8.

### 3.4 Tests

| Test | Purpose | C |
|------|---------|---|
| `LpCache default state` | Default-constructed values | C8 |
| `set_post_solve_scalars writes scalars` | Round-trip | C4 |
| `invalidate_optimal_on_mutation idempotent` | Mutation hook | C2 |
| `drop_solution_caches preserves row_dual + scalars` | Memory hygiene | C3 |
| `col_sol_buffer resizes + returns span` | Span-out write | C7 |
| `validate_consistency on optimal state` | Sanity check | C1 |
| `mark_solution_fresh tracks live backend` | Lifecycle hook | C5 |
| `cache_size_bytes accurate` | Diagnostic | C6 |

Plus a regression test that exercises the *full* LinearInterface
read/write cycle and asserts `LpCache` consistency at every step.

### 3.5 Pre-commit

* `clang-format` on the 2 new + 2 modified files.
* `clang-tidy -p tools/compile_commands.json` on the 2 new + 2
  modified `.cpp` files.
* `ctest -j20` — all 3 168 unit tests must pass with zero diff in
  any other test's pass/fail status.

### 3.6 Post-commit

* Bench `time ./gtoptTests -tc='SDDPMethod*'` before/after the
  Phase-1 commit.  Difference must be < 1% (LpCache is a pure
  encapsulation move; no semantic change).

## 4. Phase 2 sketch — `LpLifecycle`

Out of scope for the Phase-1 commit, but the contract is:

* Owns `m_backend_`, `m_low_memory_mode_`, `m_backend_released_`,
  `m_lifecycle_phase_`, `m_snapshot_*`, `m_dynamic_cols_/rows_`,
  `m_pending_col_bounds_`, `m_post_load_replay_*`, etc.
* Exposes `release_backend()`, `reconstruct_backend()`,
  `apply_post_load_replay()`, `ensure_backend()`,
  `set_low_memory(...)`, `save_flat()`, `clone(...)`.
* Calls into `LpCache::set_post_solve_scalars(...)` /
  `col_sol_buffer(...)` once per successful solve (today's
  `populate_solution_cache_post_solve` body relocates here).
* Calls into `LpCache::mark_solution_fresh(false)` on
  reconstruct.

## 5. Phase 3 sketch — `LpSolver`

* Owns `initial_solve(opts)`, `resolve(opts)`, `is_optimal()`,
  `get_status()`, the algorithm-fallback state machine, `timed_solve`.
* Reads `LpCache::cached_is_optimal()` for status fallthrough.
* Writes `LpCache::set_post_solve_scalars(...)` /
  `mark_solution_fresh(true)` after each solve.

## 6. Phase 4 sketch — `LpModel`

* Owns the structural LP: `add_col`, `add_row`, `set_coeff`,
  `set_obj_coeff`, every `set_col_*` / `set_row_*`, the label-meta
  buffers, the validation accumulator, scaling state.
* Calls into `LpCache::invalidate_optimal_on_mutation()` on every
  mutator that bumps the validity epoch.

## 7. Out-of-scope

* No changes to the JSON public API.
* No changes to the plugin (`SolverBackend`) API — that was Phase 1+2
  of the plugin split, already landed.
* No changes to the SDDP / cascade / monolithic solver state machines.
* No changes to the column/row strong-typing (`ColIndex`, `RowIndex`).

## 8. Implementation order

1. Land this doc as `docs/linear_interface_split_plan.md`.  Commit
   alone (so the plan can be reviewed in a separate PR).
2. **Phase 1** (LpCache):
   a. Add `LpCache` header + .cpp.  Add `LpCache m_cache_;` to
      `LinearInterface` *without* removing the old members yet —
      both write paths run, the new one is unread.  Commit.
   b. Migrate read sites (~50) one at a time via member-forwarding
      inline accessors on `LinearInterface` itself
      (`obj_value() const noexcept { return m_cache_.cached_obj_value(); }`).
      Commit per logical group (col_sol, col_cost, row_dual,
      scalars).
   c. Migrate write sites (~6).  Commit alone — this is the
      semantically-meaningful step.
   d. Delete the now-dead raw members.  Commit alone.
   e. Add `test_lp_cache.cpp` with C1–C8 invariants.  Commit alone.
3. **Pause for review.**  If Phase 1 lands cleanly with zero
   regressions, proceed to Phase 2.
4. **Phase 2** (LpLifecycle): same 5-step pattern as Phase 1.
5. **Phase 3** (LpSolver): same.
6. **Phase 4** (LpModel): same — biggest, last.

Each phase's PR has a fixed shape: add new type + tests, then sed-
migrate the call sites, then delete the old members.  The 5-step
pattern keeps every commit independently bisectable.
