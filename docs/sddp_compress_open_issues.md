# SDDP × LowMemoryMode::compress — open issues, deep analysis

> Companion to `docs/sddp_compress_refactor_plan.md`.  Records what was
> learned in the 2026-05-03 deep-analysis session about the two open
> failure modes after the partial repair landed.

## Issue 1 — residual LB stall (juan/iplp at LB ≈ −454 M)

### Symptom

Under `LowMemoryMode::compress`, juan/iplp produces:

```
iter | UB     | LB        | gap     | Δgap
-----|--------|-----------|---------|------
0    | 2.113G | +18.40 M  | 99.13 % | 100 %
1    | 2.072G | −562   M  | 127.13% |  28 %
2+   | 2.072G | −454   M  | 121.92% | 23 % (stalled)
```

Off mode for the same fixture recovers to LB > 0 by iter 2 (LB = +70 M).

### Diagnosis (deep dive)

LP files dumped at iter 0 backward p51 PRE-resolve under both modes
(`/tmp/diag_bwd_off_i0_s4_p51_preresolve.lp` vs
`/tmp/diag_bwd_compress_i0_s4_p51_preresolve.lp`):

* MD5 differs (`cbc4032c…` vs `6c04e7cf…`).
* 362 lines of `diff` output.
* Concentrated in **reservoir energy / seepage rows**, e.g.
  `reservoir_energy_6_54_51_510`:

  | Mode | matval | RHS |
  |------|--------|-----|
  | off | `-0.206502709208616` | `-0.97394111760315` |
  | compress | `-0.019689188` | `+12.10300518` |

The two match exact decimal patterns from PLP's piecewise filtration
table → different *segments* of the same piecewise function are
selected.

A trace of `ReservoirSeepageLP::update_lp` for reservoir UID 6
(`grep "DIAG-SEEPAGE rsv6"`) shows that the first **51 calls** (iter 0
forward) produce **identical** `vini` values in both modes.  Starting
at call **52** (iter 0 backward) the two diverge:

```
trace# | off vini | compress vini | gap
-------|----------|---------------|-----
52     |   4.716  |   95.543      | off reads cross-phase, compress hits default
53     |  49.206  |   95.543      | "
54     |  67.879  |   95.543      | "
…
```

A second trace at the entry of `physical_eini`'s cross-phase branch
shows that **the cross-phase branch is taken in BOTH modes** with
`prev_li.is_optimal() == true`.  The values RETURNED by the
cross-phase branch under compress (≈ 5–70 Hm³) match what off reads
when phases 8-15 are processed in *forward* order, but DON'T match
what off reads at the same `(scene, phase)` cell during *backward*
processing.

### Mechanism (current best hypothesis)

1. Iter 0 forward solves all 51 phases, populating `m_cached_col_sol_`
   per cell at `release_backend()` time.
2. Iter 0 backward at phase t reads `prev = phase t-1`'s cached
   col_sol via the cross-phase branch.  The cached value reflects
   the iter-0-*forward* solution.
3. Backward at phase t **re-solves** the LP with cuts → produces a
   NEW (very negative-α) col_sol.  `release_backend()` after the
   re-solve overwrites the cache with this new col_sol.
4. Backward at phase t-1 then reads `prev = phase t-2`'s cache via
   cross-phase.  Phase t-2 was NOT re-solved by backward yet (it's
   still the iter-0-forward solution), so the read is correct.

So far, off and compress should agree.

The actual divergence happens because the *off* path, between iter 0
forward p51 and iter 0 backward p51, has a chance to RE-COMPUTE
`physical_eini` against the live backend's *current* col_solution —
which may have drifted slightly during alpha column installation
(`add_col(alpha)` extends the LP and the solver may re-equilibrate
or re-factorize).  Under compress, the cache is frozen at the
release-time snapshot, which is BEFORE the alpha installation
disturbance.

The 4.716 ↔ 95.543 magnitude jump suggests one mode hits a different
piecewise-segment boundary because `vini` lands on the opposite side
of a segment break.  Reservoir 6 has segments at multiple breakpoints
within the [0, 95] range; a difference of even a few Hm³ flips the
selected segment and produces a 10× slope difference.

### Recommended fix

The cleanest model: the cached col_sol is the source of truth for any
post-release reader.  Make off mode also serve reads from a cache —
populated at `release_backend` time and never overwritten by
intermediate `add_col`/`add_row` calls.  Then both modes read from
the same kind of frozen snapshot and agree.

Alternative (less invasive): in `physical_eini`'s cross-phase branch,
explicitly read from `prev_li.cached_col_sol()` (a new accessor) and
fall through to the live backend only when no cache exists.  Forces
both modes to read the same surface.

#### Attempted (and reverted) fix — 2026-05-03

A unified-cache implementation was tried: `cache_and_release` populated
`m_cached_col_sol_` / `m_cached_col_cost_` / `m_cached_row_dual_`
unconditionally after every successful resolve, and `get_col_sol_raw`
preferred the cache whenever populated (regardless of
`m_backend_released_`).  Result: **no change in convergence
trajectory** — juan still stalls at LB ≈ −454 M.

This rules out the "live backend col_sol drift between solves"
hypothesis.  The divergence must enter the system *earlier* than the
read site — possibly in:

* The state-variable mirror (`StateVariable::col_sol`) populated by
  `capture_state_variable_values` — the SDDP state-link path uses
  these mirrors, not the LP backend's col_sol directly.
* Coefficient writes between iter 0 forward p50 release and iter 0
  backward p51 read that touch p50's cached col_sol indirectly.
* `cross-phase` vs `own-phase` selection logic in `physical_eini`
  picking different branches between modes for reasons unrelated to
  cache freshness.

Next debug step: instrument `capture_state_variable_values` to log
the col_sol value being mirrored for reservoir 6's efin state
variable at iter 0 forward p50 (off vs compress) — if the mirrored
value differs, the bug is in the mirror population path, not the
LinearInterface cache.

#### Further finding — iter 0 forward p50 LPs are byte-identical

Captured iter 0 forward p50 PRE-RESOLVE LP files from both modes
(`/tmp/diag_fwd_off_i0_s4_p50_preresolve.lp` vs
`/tmp/diag_fwd_compress_…`).  Both have MD5 `a7cb495d…` and size
3,670,988 bytes — **byte-identical**.

This narrows the bug significantly:

* The LP itself is the same in both modes.
* The solver may produce a different `col_solution` from the same
  LP (multiple-optima case, or solver thread/randomization).
* OR: between iter 0 forward p50's solve and iter 0 backward p51's
  cross-phase read of p50, *something* mutates the col_solution
  that's read back differently in off vs compress.

The rsv6 efin trace showed off mode reading values up to ~174 Hm³
at backward — *higher* than the construction-time eini of 95.5 Hm³.
This is only physically possible if rsv6 has inflows that refill it
in late phases.  Both modes should compute the same iter-0-forward
trajectory; if they don't, that's the bug.

A definitive next experiment: dump iter 0 forward p50 POST-RESOLVE
LP under both modes (where col_solution is included in the LP file
or via a separate solution dump) and compare the rsv6 efin column
solution.

#### Result of the post-resolve col_sol experiment

Captured `/tmp/diag_fwd_colsol_off_i0_s4_p50.bin` and
`/tmp/diag_fwd_colsol_compress_…` — **byte-identical** (MD5
`6cc486f5…`, size 202,824 bytes = 25,353 doubles).

Conclusion: solver IS deterministic.  Both modes have identical
complete state at end of iter 0 forward p50.  The bug must enter
the system between iter 0 forward p50 release and iter 0 backward
p51's read.

#### Attempted (and reverted) fix — `m_backend_solution_fresh_` flag

Added a per-`LinearInterface` flag tracking "live backend has been
re-solved since last reload".  Set `false` on `reconstruct_backend`
/ `install_flat_as_rebuild`.  Set `true` immediately after
`m_backend_->resolve()` / `m_backend_->initial_solve()` (so the
fallback-cycle's `is_optimal()` check sees the live state).

Gated `is_optimal()` to return `m_cached_is_optimal_` when
`!m_backend_solution_fresh_` — under compress at iter 0 backward,
this would let `physical_eini`'s cross-phase branch see the prior
optimal-from-iter-0-forward state instead of the just-loaded-not-
yet-resolved backend's `is_proven_optimal() == false`.

Also gated `get_col_sol_raw()` similarly: prefer cache when
`m_backend_released_ || !m_backend_solution_fresh_`.

Made `cache_and_release` populate `m_cached_col_sol_` /
`m_cached_col_cost_` / `m_cached_row_dual_` after every solve so
the cache survives release/reconstruct cycles in both modes.

Result: **all 18 SDDP unit tests pass**, but juan/iplp iter 1
forward p51 fails with "degenerate cut family — all 8 contributing
links clamp at source_upp".  The cache-based reads now agree with
off mode at the LP-coefficient level, BUT the cuts built from
that aligned state expose a deeper SDDP cut-construction issue
where 8 reservoirs all simultaneously hit their physical maxima
at iter 0 backward — a degenerate vertex that prevents any valid
cut from being constructed.

Reverted (no commit).  Off mode does NOT exhibit this failure on
the same fixture, so the SDDP machinery is itself correct.  The
fix must therefore be entirely in the LinearInterface / backend
lifecycle — making compress's reads align with off's without
exposing any other side effect.

#### Precise root-cause confirmation — 2026-05-04 deep trace

Detailed trace pinpoints the exact failure:

* `LinearInterface::is_optimal()` returns `m_backend_->is_proven_optimal()`
  when `!m_backend_released_`.
* After `src_sys.ensure_lp_built()` at `backward_pass_single_phase:401`,
  prev's `m_backend_released_=false` AND `m_backend_->is_proven_optimal()=false`
  (just reloaded, no resolve yet).
* `m_cached_is_optimal_=true` (from prior iter-0-forward solve) but
  `is_optimal()` does NOT consult it on this branch.
* `physical_eini`'s cross-phase `if (li.is_optimal()) {…}` SKIPS.
* Falls through to step 4 (default) — `vini = default_eini = 95.543`.

Off mode never reloads, so `is_proven_optimal()` continues to return
true from the iter-0-forward solve.  Cross-phase fires, reads correct
`vini`.

So the `LinearInterface::is_optimal()` flag is **not lying about the
cell** — it's reporting the LIVE BACKEND state (no solution loaded).
The cached state (semantically still valid for the cell's structural
LP) is silently ignored on this branch.

A fresh-flag fix (track "backend solved since last reload") makes
`is_optimal()` defer to the cached flag in this window.  This yields
correct `vini` reads under compress, but iter-1 fails downstream
in juan in a way that off mode does not — the failure mode requires
further investigation but is **not** an SDDP / α-algorithm bug
(SDDP works correctly under `LowMemoryMode::off`, so the
machinery is correct as-is).  The fix must be **purely in the
LinearInterface / backend lifecycle** to make compress's reads
align with off's reads, without changing any SDDP semantics.

Cache-related changes attempted in this session (all reverted):
1. `m_backend_solution_fresh_` flag + `is_optimal()` gate +
   `get_col_sol_raw()` gate — produces correct vini reads under
   compress, but iter-1 fails at p51 with "degenerate cut family"
   (8 reservoirs all clamp at source_upp).  This failure is NOT
   reproducible under off, even though off and compress should
   now have identical LP states post-fix.  Mechanism unclear:
   LP coefficients should match (same vini → same update_lp
   writes), solver is deterministic (verified at iter 0 forward
   p50, byte-identical col_sol), yet iter-0-backward solves
   produce different cuts.
2. Unconditional `cache_and_release` populating col_sol/col_cost/
   row_dual — slow (~5x) and didn't change convergence.
3. `CplexSolverBackend::cache_solution` checking `is_proven_optimal`
   first — defensive but didn't address the bug (which is in the
   higher-level `is_optimal` semantic).

The remaining question: **why** does iter-0-backward p51 produce
different cuts under compress (with the fresh-flag fix) vs off,
when the LPs should be identical?  Hypotheses to investigate next:

- Solver basis carry-over between forward and backward solves.
  Off retains the iter-0-forward p51 basis; compress reconstructs
  fresh basis from `load_flat`.  CPXgetx might be deterministic
  on a fresh basis but the basis itself isn't carried over →
  different starting vertex → different optimum if LP is even
  marginally degenerate.  But the user noted SDDP is fine, so
  this shouldn't matter.

- Some `physical_eini` / `physical_efin` read takes a different
  branch (not the cross-phase one).  E.g., own-phase, warm, or
  default.  Need to verify all four branches give same value
  in both modes post-fresh-flag-fix.

- Some other LinearInterface state isn't preserved across
  release/reconstruct that off has continuously.  E.g., col bound
  pins set by `propagate_trial_values`, scale vectors, etc.
  Audit each member that could be lost on reconstruct vs kept
  on off's continuous backend.

## Issue 2 — P4 (snapshot bake-in) iter 1 phase 10 infeasibility

### Symptom

When P4 was applied (commits `feed6ce3` / `8d7dddd8`), juan/iplp iter 1
forward phase 10 went infeasible (relaxed clone infeasible at solver
status 2).  iter 0 completed correctly with UB=2.113G LB=18.40M
(matches off).  Forward iters 1-9 of iter 1 had positive obj/α.
At p10 the LP became infeasible.

### What P4 changed

* Added `m_pending_coeff_updates_: std::map<{RowIndex, ColIndex}, double>`
  to `LinearInterface`.
* `set_coeff_raw(r, c, v)` records `(r, c) → v` in the map (gated by
  `!m_replaying_ && low_memory != off`).
* `apply_post_load_replay` re-applies the map after `replay_active_cuts`.

### Diagnosis (in-progress — captured for follow-up)

Trace from the failing P4 run (`/tmp/juan_p4_trace.log`):

```
[s4 p10] LP_QUALITY: nnz=32346 max=1.00 min=2.88e-03 ratio=347
SDDP UpdateLP [s4 p10]: updated=70 elements (prev_sys=1)
SDDP Forward [i2 s4 p10]: elastic filter produced no feasibility cut
LI release [mode=2]: backend numrows=5434 numcols=25351 optimal=0 active_cuts=1
```

Key facts:

* LP_QUALITY metrics are byte-equivalent between iter 0 and iter 1
  forward phase 10.  Aggregate matrix shape is identical.
* `update_lp` reports `updated=70` (vs 80 at iter 0 forward p10).
  Some elements skip via the `current_slope == new_slope` early return.
* The element-count drop (80→70) is consistent with elements that
  computed identical coefficients to iter 0.
* The LP becomes infeasible AFTER `update_lp_for_phase` completes —
  the elastic-clone solve fails too.

### Hypothesis

Some `(row, col)` in `m_pending_coeff_updates_` references a row
index that was VALID at iter 0 forward p10 (when set_coeff was
called) but is now INVALID at iter 1 forward p10 because:

* `replay_active_cuts` adds cut rows AFTER the structural ones; if
  any prior `set_coeff` recorded a row index ≥ `base_numrows`, the
  replay corrupts a cut row.
* The pending replay may write a coefficient that conflicts with a
  cut's bound, making the relaxed clone infeasible.

Direct test (TODO): re-apply P4, dump iter 1 forward p10's LP BEFORE
and AFTER the pending replay, diff against the off-mode equivalent.
The trace instrumentation from commit `994b327d` already covers
release/reconstruct events; need to add a "post-replay coefficient
sample" probe.

### Recommended next step

P5 (`BackendSession` RAII) eliminates the protocol drift that allowed
this bug class.  Once the lifecycle invariants are explicit at the
API surface, P4 can be re-implemented as part of the session's
`finalize_release` step where `(row, col)` indices are guaranteed to
be in the structural region.

## Status of plan phases

| Phase | Status |
|-------|--------|
| P1 (backward update_lp_for_phase fix) | ✅ landed `3e73f68c` |
| P2 (Tests 1, 4) | ✅ landed `1b9949aa`, P3 invariant test `b2777125` |
| P3 (collections persist across release) | ✅ landed `24fe1b99` — **5-8× speedup on juan compress** |
| P4 (snapshot bake-in) | ❌ tried twice, reverted both — needs P5 first |
| P5 (BackendSession RAII) | open |
| P6 (trace instrumentation) | ✅ landed `994b327d` |
| P7 (retire `m_collections_built_`) | open |

## Files captured for follow-up

* `/tmp/diag_bwd_off_i0_s4_p51_preresolve.lp` — off-mode iter 0 backward
  p51 LP pre-resolve (size 3,695,881 bytes).
* `/tmp/diag_bwd_compress_i0_s4_p51_preresolve.lp` — compress-mode same
  cell (size 3,696,756 bytes; 362 diff lines vs off).
* `/tmp/juan_p4_trace.log` — captured during the second P4 attempt;
  shows the iter-1-p10 infeasibility trajectory with the
  `LI release` / `LI reconstruct` / `SDDP UpdateLP` traces.

These files are kept around (not committed) for the next debug
session.
