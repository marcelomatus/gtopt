# LinearInterface solution-cache lifecycle — off vs compress

> **Date:** 2026-05-04
> **Status:** Critical review pre-Option-A refactor.  Records the
> invariants we WANT to enforce, audits the tests we have, and
> proposes targeted instrumentation for the residual juan/iplp
> off↔compress LB divergence (see
> `docs/sddp_compress_open_issues.md`).

## 1. Invariants we want to hold

Both modes must produce **byte-identical** SDDP trajectories on the
same fixture.  The residual divergence (juan compress LB ≈ 5–22× off
LB at iter ≥ 3) proves at least one of the following invariants is
currently violated under compress:

| # | Invariant | Off | Compress (target) |
|---|-----------|-----|-------------------|
| I1 | After a successful `resolve()`, every public read path returns the same value as a fresh CPXgetx/CPXgetdj/CPXgetpi against the live solver. | Trivial — one source. | Cache must mirror live values exactly post-solve. |
| I2 | After a structural mutation (`add_row`/`add_col`/`set_coeff_raw`/`set_*_bound`) before the next `resolve()`, `is_optimal()` returns false. | Trivial — backend's CPXgetstat reflects mutation. | `m_cached_is_optimal_=false` (committed: `invalidate_cached_optimal_on_mutation`). |
| I3 | After a structural mutation, the cached solution vectors are *invalid* and must not be returned to readers. | Off has no cache. | Cache cleared on mutation (committed: `shrink_to_fit` in invalidate hook). |
| I4 | Cell read at backward iter k, phase p (cross-phase via `physical_eini`) returns the SAME `efin` value as off mode. | Live backend = forward iter k-1 p solution. | LI cache populated at last `release_backend` = forward iter k-1 p solution. |
| I5 | The LP fed to `m_backend_->resolve()` at backward iter k phase p has **identical** matval / collb / colub / rowlb / rowub between modes (modulo equilibration scales which are baked into the snapshot). | Live LP after all forward+backward mutations. | Reconstructed LP after replay (dynamic cols, dynamic rows, active cuts, pending col bounds). |
| I6 | Off mode owns NO solution cache. | True by construction (release_backend is no-op). | N/A (this is an off-mode invariant). |
| I7 | Compress cache is freed the moment it's no longer valid or in use. | N/A. | Free on mutation (committed). Free at next `cache_and_release` — `assign()` overwrites old contents. |

## 2. Cache lifecycle states

Each cell goes through these states.  States marked **★** are where
off and compress diverge in non-obvious ways — the candidates for
the residual bug.

```
     INITIAL_BUILD                 (load_flat → first solve)
            │
            ▼
     ┌──────────────┐
     │  LIVE+FRESH  │     ← post-resolve(), cache valid
     └──────┬───────┘
            │ release_backend()       ★ (off=no-op, compress=cache+free)
            ▼
     ┌──────────────┐
     │   RELEASED   │     ← (compress only) backend gone; cache survives
     └──────┬───────┘
            │ ensure_lp_built() / reconstruct_backend()
            ▼
     ┌──────────────┐
     │ LIVE+!FRESH  │     ★ (compress only) backend reloaded, cache holds prior solve
     └──────┬───────┘
            │ mutation (add_row/set_coeff/set_col_bound) ★ INVALIDATE
            ▼
     ┌──────────────┐
     │ LIVE+!FRESH+ │     cache empty; reads return defaults
     │  INVALID     │
     └──────┬───────┘
            │ resolve()
            ▼
     ┌──────────────┐
     │  LIVE+FRESH  │     loop back; cache repopulated
     └──────────────┘
```

### Off-mode realisation

* **release_backend** is a no-op (early return).
* **reconstruct_backend** is never called.
* The `LIVE+!FRESH` and `RELEASED` states are unreachable.
* **The LI cache vectors are NEVER populated** (no caller writes to
  them under off; this is invariant **I6**).

### Compress-mode realisation

* **release_backend** populates `m_cached_col_sol_`/`cost_`/`dual_`
  by *copying* from the backend's `col_solution()`/`reduced_cost()`/
  `row_price()` then frees the backend.
* **reconstruct_backend** runs `load_flat` (resets bounds, coefs to
  construction-time) then `apply_post_load_replay` (re-applies
  dynamic_cols, dynamic_rows, active_cuts, pending_col_bounds).
* `m_backend_solution_fresh_=false` until next solve — read paths
  consult the cache.

## 3. What survives reconstruct vs what doesn't

This is the heart of the question.  An asymmetry here is the most
likely source of the residual divergence.

### Survives via `apply_post_load_replay`

| State | Mechanism | Test coverage |
|-------|-----------|---------------|
| Dynamic columns (alpha) | `m_dynamic_cols_` → `add_cols(span)` | ✓ existing |
| Dynamic rows (cascade fixings) | `m_dynamic_rows_` → `add_rows(span)` | ✓ existing |
| Active cuts | `m_active_cuts_` → `replay_active_cuts()` | ✓ existing |
| Column bound overrides (`set_col_low/upp_raw`) | `m_pending_col_bounds_` → direct backend calls | ✓ committed (`f8b1b54c` + `bb3d3f26`) |
| `m_base_numrows_` | `save_base_numrows()` after dynamic rows | ✓ existing |
| `m_col_scales_` / `m_row_scales_` | restored from snapshot via `load_flat` | ✓ existing |
| `m_scale_objective_` | restored from snapshot | ✓ existing |
| `m_equilibration_method_` | restored from snapshot | ✓ existing |
| Label metadata (`m_col_labels_meta_`, `m_post_flatten_*`) | preserved across release; not reset by load_flat | ✗ no direct test |

### Does NOT survive — must be recomputed each iteration

| State | Recomputed where | Risk |
|-------|------------------|------|
| Coefficient updates by `update_lp_for_phase` (turbine_conversion, seepage segment, discharge_limit) | `update_lp_for_phase(phase_index)` is called explicitly in `backward_pass_single_phase` line 463 before resolve | **HIGH** — these reads `physical_eini` cross-phase; if compress's prev_li returns a different value than off's live prev_li, coefficients diverge |
| Row bound mutations (`set_row_low/upp_raw`/`set_rhs_raw`) | NOT replayed.  Caller must re-issue every reconstruct. | **MEDIUM** — verify all SDDP row mutations are inside an "always re-issued" callback (cascade elastic targets are, per the `m_dynamic_rows_` mechanism; SDDP fcut elastic relaxations may not be) |
| `set_obj_coeffs_raw` | NOT replayed.  Caller responsibility. | **LOW** — only used at structural build before snapshot |

### LIKELY DIVERGENCE SOURCE — equilibration scales drift

When `equilibration_method == ruiz` (or similar iterative scheme),
**off mode's scales are computed once during initial flatten**, then
stay fixed through the run.  **Compress's scales are also computed
once** (in flatten) and baked into the snapshot, so reconstruct
restores the same scales.  ✓ no drift here.

But: **CPLEX may apply its own internal scaling on each `resolve()`
call**, especially when `presolve=true`.  Under off, CPLEX caches its
internal scaling between solves.  Under compress, the backend is
reborn each cycle — CPLEX recomputes from scratch.  This is a
potential numerical-state difference.

### LIKELY DIVERGENCE SOURCE — `m_pending_col_bounds_` map order

`m_pending_col_bounds_` is a `std::map<ColIndex, std::pair<double,
double>>`.  Replay iterates `for (const auto& [col, bounds] :
m_pending_col_bounds_)` — order is `ColIndex` ascending (map
ordering).  But under off, the bounds were set in **call order** by
`propagate_trial_values` and other mutators.  If two bound-set calls
on the SAME column fire (e.g. propagate iter 1 forward sets dep_col
to v1; then later something sets it to v2), the map keeps only the
last value (correct).  ✗ but if a NUMERIC-NORMALISATION step in
`m_backend_->set_col_lower` produces a slightly different value
under compress vs off, we'd see it.

## 4. Test coverage audit

### Currently covered (post commit `2495ab48`)

* Mutation invariants under compress: each public mutation API drops
  cache + flips `m_cached_is_optimal_=false`.  (8 new TEST_CASEs.)
* Off-mode no-op: hook is benign under off.
* Replay path: `apply_post_load_replay` does NOT drop cache.
* `set_low_memory(off)` clears prior compress cache.
* `release_backend` cycle preserves cache when reconstructed-not-resolved.
* Col-bound replay survives reconstruct (`bb3d3f26`).

### Test gaps (recommended additions)

1. **Off-mode no-cache invariant (I6)** — explicit assertion that
   `cached_col_sol_size() == 0` always under off, even after
   `release_backend` (no-op) and reads (no-op).  Test scaffold:
   ```cpp
   TEST_CASE("Off mode never populates LI cache") {
     // ... build, solve, repeated read+release+reconstruct(no-op)
     CHECK(li.cached_col_sol_size() == 0);
   }
   ```

2. **Cross-phase efin parity (I4)** — table-driven test that builds
   a 2-phase LP, solves p0, releases (compress) / no-ops (off), and
   asserts `prev_rsv.physical_efin(prev_li, ...)` returns the same
   value in both modes after sequence of release+reconstruct cycles.

3. **LP-equality after replay (I5)** — for compress, dump the LP
   matrix post-`apply_post_load_replay`, verify it byte-equals the
   off-mode live LP after the same sequence of mutations.  Use
   `m_backend_->write_lp` to a string buffer and `diff` with off.

4. **Backward-pass cross-phase coefficient parity** — instrument
   `update_lp_for_phase` to capture the (col, row, value) triples it
   writes, run both modes, assert the triples are identical.

5. **Row-bound mutation NOT replayed** — pin the caller-contract
   that `set_row_low/upp/rhs_raw` mutations are non-replayed and
   must be re-issued on every reconstruct.  (This already exists in
   the cpp26-modernizer doc but should be a TEST_CASE.)

6. **`m_active_cuts_` ordering invariant** — the LP coefficient
   ordering in CPLEX depends on `add_row` insertion order.  If
   compress replays cuts in a different order than off added them,
   row indices diverge and `record_cut_deletion` indexes wrong rows.
   Test: add cuts in known order, release, reconstruct, dump cut
   row indices, verify match.

7. **Pending-col-bounds ORDER invariant** — `m_pending_col_bounds_`
   is replayed in `ColIndex` order (map ordering), but off applies
   in *call* order.  For non-commutative bound semantics (e.g. a
   bound update that triggers solver-side equilibration), this
   could matter.  Empirical test: bound the same column twice in
   different sequences, verify identical `col_lower()`/`col_upper()`
   reads.

## 5. Instrumentation plan to find the residual divergence

The trace infrastructure landed in `994b327d` covers
release/reconstruct events.  We need to add three more probes:

### Probe 1 — Cross-phase value reads at `update_lp_for_phase`

At every `physical_eini`/`physical_efin` cross-phase call site,
emit:
```
DIAG-XPHASE iter={} scene={} phase={} src_phase={} elem_uid={} \
  prev_released={} prev_fresh={} prev_optimal={} v_phys={:.6f}
```
With both off and compress traces saved to separate files, a `diff`
will isolate the FIRST iteration where any returned value differs
between modes.  That's the spot to debug.

**Where to add:** `include/gtopt/storage_lp.hpp::physical_eini`
(line ~347, just before `return v_phys` in the cross-phase branch).
Already partially scaffolded in this session — finalise once
storage_lp's interface stabilises.

### Probe 2 — LP coefficient hashes at backward solve

Hash the live LP's matval+collb+colub+rowlb+rowub immediately before
`tgt_li.resolve()` in `backward_pass_single_phase` line 469.  Emit:
```
DIAG-LPHASH iter={} scene={} phase={} hash=0x{:016x} numrows={} numcols={}
```
A diff between modes will pinpoint the exact iteration/phase where
the LP first differs.

### Probe 3 — Cut coefficient + RHS at `add_cut_row`

Emit each new cut's full `cmap` as it's added:
```
DIAG-CUT iter={} scene={} phase={} row_idx={} lowb={} \
  cmap=[(col=N val=V)...]
```
A diff will tell us whether the cuts themselves differ between
modes (after my symmetry fix, they should not — but the empirical
LB divergence says they do).

### Putting them together

Run both modes with `--trace-log` to separate files, then:
```bash
diff <(grep DIAG-XPHASE off.trace) <(grep DIAG-XPHASE compress.trace) | head -50
diff <(grep DIAG-LPHASH off.trace) <(grep DIAG-LPHASH compress.trace) | head -20
diff <(grep DIAG-CUT off.trace)    <(grep DIAG-CUT compress.trace)    | head -50
```

The FIRST `DIAG-XPHASE` line that differs locates the bug.  If
DIAG-XPHASE matches but DIAG-LPHASH differs, the bug is somewhere
between the cross-phase read and the LP submission (i.e., a
non-replayed mutation).  If both match but DIAG-CUT differs, the
bug is in the cut construction itself (unlikely given off works).

## 6. Action items

1. **(in-flight)** Plugin solution-cache removal — agent
   `a5577fe18edf10858`.  Establishes single source of truth in LI.
2. **(post-agent)** Enforce off-mode no-cache invariant (I6) by
   gating the eager-populate step on `m_low_memory_mode_ != off`
   inside `timed_solve`.  Add the test from §4.1.
3. **Then:** add Probe 1 (XPHASE).  Run juan once each mode, diff.
4. **If Probe 1 doesn't diverge:** add Probes 2 + 3.  Diff again.
5. **Once divergence is localised:** apply the symmetry fix at the
   exact mutation/replay site uncovered.

## 7. Open question (for later)

Even after Option A (single-source LI cache) lands, we still have
the `invalidate_cached_optimal_on_mutation` hook which was added
specifically because compress's `m_cached_is_optimal_` would
otherwise outlive a `add_row`.  After Option A, the cache vectors
get repopulated only after a successful resolve anyway — so the
*vectors* are self-invalidating.  But the *flag*
(`m_cached_is_optimal_`) still needs explicit invalidation, since
nothing else flips it back to false on mutation.  Keep the hook for
that flag; it's cheap and the contract-level test in §4 protects it.
