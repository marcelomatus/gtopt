# LP / SDDP audit — fix plan and test ladder
**Date:** 2026-04-29
**Source audit:** `lp-numerics-expert` agent, 2026-04-28
**Companion doc:** `support/lp_scale_audit_2026-04-26.md`

This plan formalises which findings to act on, in what order, and what
unit/integration tests must exist **before** each fix lands so the bug
state is captured by a failing test first, then released by the fix.

> ⚠️ **Hold for incoming fixes.** The user has unrelated fixes in flight
> that may touch `sddp_boundary_cuts.cpp` and the surrounding paths.  Do
> not start applying fixes from this plan until those land — re-validate
> every finding against the post-merge code, then proceed.

---

## Findings status (post-review, 2026-04-29)

After re-validating every claim against the live tree the actionable
list shrinks substantially: 5 of 10 findings are already resolved or
were never bugs.

| ID    | Severity | File:line                          | Status after manual review        | Plan |
|-------|----------|------------------------------------|-----------------------------------|------|
| P0-1  | Severe   | `source/sddp_boundary_cuts.cpp:361,389` | **Probably real** — pre-scaling still in code; `add_cut_row` doc explicitly says rows must be physical. **Not yet test-confirmed.** | Phase 1A → 2A → 3A |
| P0-2  | (retracted) | —                              | Agent self-retracted; alpha_val math is correct | drop |
| P0-3  | Latent   | `benders_cut.cpp:157` + `state_variable.hpp:281` | Only triggers under ruiz equilibration; default is `none`, no active impact | track-only |
| P0-4  | Not P0   | `sddp_forward_pass.cpp:760-777`    | NaN guard observation only        | drop |
| P1-1  | **invalid** | `sddp_aperture_pass.cpp:141`    | Agent had math direction wrong; `reduced_cost_physical = LP_rc × scale_obj / var_scale` (multiply, not divide); filter operates in physical units, exactly where it should | drop |
| P1-2  | **invalid** | `sddp_types.hpp:143` (field missing) | `alpha_max=1e15` field was removed when α became a free LP variable (`planning_options_lp.hpp:651`); agent hallucinated stale code | drop |
| P1-3  | **resolved** | `planning_lp.cpp:239-249`     | Already uses `median(X/V²)` (`x_taus`); the `median(X)` form the agent claimed exists has been replaced — the docstring at line 153-160 explicitly walks through *why*. Likely fixed by recent commit `938d1bac refactor(line_lp,bus_lp): adaptive theta_max + LineLP cleanups` | drop |
| P1-4  | Defensive hardening | `linear_interface.cpp:138-222` | Cache contract is actually robust: `release_backend()` clears caches on non-optimal exit (lines 187-192) and only populates when each backend pointer is non-null (lines 169-180). No active bug; agent's "latent hazard" framing is over-cautious | track-only |
| P2-1  | Cleanup  | `sddp_boundary_cuts.cpp:399`       | `add_cut_row` defaults to `eps=0`; bundled with the P0-1 fix as a minor follow-up | Phase 2C (with P0-1) |
| P2-2  | Real bug | `sddp_boundary_cuts.cpp:120-126`   | Verified: `name_to_class_uid` only has Junction + Battery; lookup at lines 156-176 has no fallback path, no Reservoir entry exists. Hydro state-vars silently dropped. No "CutLabel" or alternative resolution mechanism in tree | Phase 1B → 2B → 3B |
| lint  | Clean-up | strong-type `static_cast<Uid>` | 4 violations — patch already in working tree, build green | Phase 5A |

### Net actionable items

After re-validation only **three** real follow-ups remain:

1. **P2-2** — add `reservoir_array` entries to `name_to_class_uid` (one-line fix + capture test).
2. **P0-1** — confirm with a round-trip regression test that boundary cuts ARE double-scaled today; if so, switch the assembly to physical units. *If the test passes on current code*, the bug is already masked or the agent's analysis is wrong, and we drop the fix and just keep the round-trip test as a contract pin.
3. **lint** — commit the 4 already-coded `static_cast<Uid>` cleanups.

Everything else from the audit is invalid, retracted, already resolved,
or a no-op.

### P1-1 review detail (why dropped)

Agent's claim: `build_benders_cut_physical` overload 2 *divides* rc by
`scale_obj`, leaving the eps filter in LP-space units.

Actual code (`state_variable.hpp:281-284`):
```cpp
[[nodiscard]] constexpr double
reduced_cost_physical(double scale_objective) const noexcept {
    return m_reduced_cost_ * scale_objective / m_var_scale_;
}
```
This **multiplies** by `scale_obj`.  Dimensional check:
- `LP_obj = phys_obj / scale_obj`, `LP_var = phys_var / col_scale`
- ⇒ `LP_RC = (col_scale / scale_obj) × phys_RC`
- ⇒ `phys_RC = LP_RC × scale_obj / col_scale`

So the formula is correct.  The `cut_coeff_eps = 1e-8` filter operates
on `rc_phys` (physical units, $/physical-unit) — exactly where a
"drop near-zero coefficient" filter should live.  No fix needed.

The same analysis applies to `to_flat(eps)` inside `compose_physical`:
the row entering `to_flat` is in **physical units** and the eps filter
is therefore physical too.  The user's instinct that the test tolerance
should be parameterised by `cut_coeff_eps` is correct: any new test
that compares a round-tripped LP coefficient to its physical input
should use `cut_coeff_eps` (absolute, $/unit) as its absolute
tolerance, not a hand-picked literal.

---

## Phase 1 — Pre-fix tests that capture the bug

These are **failing tests under current code**.  They land in commits
*before* any production-code fix, so the regression history clearly
shows "introduced failing test → applied fix → test now green."

### Phase 1A — `test_sddp_boundary_cuts_round_trip` (P0-1 capture)

**File:** `test/source/test_sddp_boundary_cuts_round_trip.cpp` (new)

**Fixture:**
```cpp
auto planning = make_3phase_hydro_planning();
planning.options.scale_objective = OptReal{1000.0};
planning.options.variable_scales.push_back(VariableScale{
    .class_name = "Reservoir",
    .variable   = "energy",
    .scale      = 5.0,                 // non-trivial col_scale
});
PlanningLP plp(std::move(planning));
```

**Boundary CSV (in-memory tmp file):**
```
name,iteration,scene,rhs,j_up
phys_cut,1,0,100.0,3.0
```
- `phys_rhs   = 100.0`
- `phys_coeff = 3.0`

**Expected post-load LP-row contract** (per
`linear_interface.cpp:1142-1146`'s round-trip invariants):

| Read-back accessor                | Expected (physical-space)    |
|-----------------------------------|------------------------------|
| `li.get_row_low(cut_row)`         | `100.0`                      |
| `li.get_row(cut_row)[svar_col]` (composed-out value) | `-3.0` |
| `li.get_row_dual(cut_row)`        | physical π — only checked if cut binds |

Tolerance: `doctest::Approx(100.0).epsilon(cut_coeff_eps_default)`
where `cut_coeff_eps_default = 1e-8` (the production default).
For coefficients we use the same absolute eps (`epsilon` ≈ relative,
but for a coefficient of magnitude ~3 the absolute floor `1e-8`
gives `1/3e8` relative which is safe).

**Behaviour under current code (predicted):**
- `get_row_low(cut_row)` returns `0.1` (= `100 / scale_obj`) instead
  of `100.0` → **fails by exactly factor `scale_obj`**.

**Sub-cases to cover:**
1. `scale_obj = 1`, `col_scale = 1` → trivial, must still pass on
   current code (no double-scaling triggered when both factors = 1).
2. `scale_obj = 1000`, `col_scale = 1` → fails by `1/1000` on RHS.
3. `scale_obj = 1`, `col_scale = 5` → fails by `1/5` on coefficient
   only (RHS unaffected).
4. `scale_obj = 1000`, `col_scale = 5` → both effects compound.

The 2×2 cross-product makes the bug's exact factor visible (and
discriminates "double-scale" from any other arithmetic error).

**Helper to re-use:** `test_sddp_cut_io.cpp` already builds
`make_3phase_hydro_planning()` and writes temp CSVs.  Lift the temp-
file helper into `sddp_helpers.hpp` if it's worth de-duplicating.

### Phase 1B — `test_sddp_boundary_cuts_reservoir_name_map` (P2-2 capture)

**File:** `test/source/test_sddp_boundary_cuts_reservoir_name_map.cpp` (new)

**Fixture:** A planning where the boundary-cut state variable is on a
**Reservoir** (NOT a Junction or Battery) — e.g.
`make_3phase_hydro_planning()` already has reservoir `rsv1` (uid 1)
distinct from junction `j_up`.  Build the CSV referencing the reservoir
under any name pattern that goes through `name_to_class_uid`:
- bare element name `rsv1`
- ClassName-prefixed `Reservoir:rsv1`

**Boundary CSV:**
```
name,iteration,scene,rhs,Reservoir:rsv1
res_cut,1,0,50.0,2.0
```

**Expected behaviour after fix:** cut is loaded successfully.
Assertion: `result->count == 1`.

**Behaviour under current code:** `name_to_class_uid` only contains
junctions + batteries; the lookup at `sddp_boundary_cuts.cpp:148-175`
silently drops the column.  With `MissingCutVarMode::skip_cut`
(default), the entire cut is skipped → `result->count == 0`.

Add an additional sub-case under `MissingCutVarMode::skip_coeff`:
without the fix, the cut is loaded but the reservoir's coefficient is
zero (zero-gradient cut, mathematically valid but useless).  Verify
post-fix that the coefficient lands on the correct LP column.

### Phase 1C — Strong-type lint coverage

The 4 violations were already fixed in working tree; commit them as
the first preparatory commit so subsequent fixes don't tangle with
unrelated cleanup.  CI: `tools/lint_strong_types.sh` should be
unconditionally green after this lands.

---

## Phase 2 — Production-code fixes

Each fix is its own commit, *paired* with the failing tests from
Phase 1 graduating to passing.  Order: P2-2 first (smallest, no
algebra debate), then P0-1.

### Phase 2A — P0-1 boundary-cut double-scaling

**File:** `source/sddp_boundary_cuts.cpp:361,389`

**Diff sketch:**
```diff
-     auto row = SparseRow {
-         .lowb = rc.rhs * bc_discount / scale_obj,
+     auto row = SparseRow {
+         .lowb = rc.rhs * bc_discount,
          .uppb = LinearProblem::DblMax,
-         .scale = sa,
+         .scale = 1.0,
          ...
      };
-     row[alpha_svar->col()] = sa;
+     row[alpha_svar->col()] = 1.0;
      ...
-         row[*col_opt] = -coeff * scale * bc_discount / scale_obj;
+         row[*col_opt] = -coeff * bc_discount;
```

The intent: feed `compose_physical` rows that are *exactly* in
physical units (RHS in $, coefficients in $/physical-unit, alpha
coefficient = 1.0).  `compose_physical` handles the `× col_scale`
and `÷ scale_obj` once.  The `row.scale = 1.0` removes the redundant
`× sa` / `÷ sa` round-trip on the alpha column — `compose_physical`
applies col_scale[alpha_col] = scale_alpha and ÷ scale_obj, exactly
what the equivalent `add_cut_row` for an SDDP-built optimality cut
produces.

**Cross-check:** read `build_benders_cut_physical(alpha_col, links,
obj_phys, ...)` overload 1 at `benders_cut.cpp:100-134`.  It sets
`row[alpha_col] = 1.0`, `lowb = obj_phys`, coefficients
`-rc_phys`.  Boundary cuts must produce the *same* physical-space row
shape — that's the canonical reference.

### Phase 2B — P2-2 missing reservoir_array

**File:** `source/sddp_boundary_cuts.cpp:120-126`

**One-line fix:**
```diff
  map_reserve(name_to_class_uid,
-             sys.junction_array.size() + sys.battery_array.size());
+             sys.junction_array.size() + sys.battery_array.size()
+             + sys.reservoir_array.size());
  for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
  }
  for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
  }
+ for (const auto& rsv : sys.reservoir_array) {
+     name_to_class_uid[rsv.name] = {"Reservoir", rsv.uid};
+ }
```

**Risk:** name collisions across classes.  `make_3phase_hydro_planning`
already names a junction `j_up` and a reservoir `rsv1` (different
spellings) — no collision in fixtures.  In production data, a junction
and a reservoir could share a name; the existing two-class map already
has the same risk between Junction and Battery.  The CSV header allows
`Reservoir:name` to disambiguate (`sddp_boundary_cuts.cpp:148`); when
the user writes a bare name and the map has duplicates, last-write
wins.  Document this in the function header.

### Phase 2C — P2-1 plumbing `cut_coeff_eps` through `add_cut_row`

**File:** `source/sddp_boundary_cuts.cpp:399-400`

After P0-1 lands, the boundary cut row carries physical coefficients
that may include numerical noise from the upstream PLP solver.  Pass
the SDDP `cut_coeff_eps` to the loader so `to_flat(eps)` filters
near-zero physical coefficients consistently with the regular SDDP
path.  Threading: lift `cut_coeff_eps` from the caller's
`SDDPOptions` (already accessible at the loader's outer scope).

```diff
- std::ignore = add_cut_row(
-     planning_lp, scene_index, last_phase, CutType::Optimality, row);
+ std::ignore = add_cut_row(
+     planning_lp, scene_index, last_phase, CutType::Optimality, row,
+     opts.cut_coeff_eps);
```

---

## Phase 3 — Post-fix verification

### Phase 3A — Tests from Phase 1 graduate

Both `test_sddp_boundary_cuts_round_trip` and
`test_sddp_boundary_cuts_reservoir_name_map` flip from `XFAIL`
(documented under doctest's `should_fail`) to fully passing.  In the
fix commit, drop the `should_fail` and assert clean.

Alternative pattern (preferred): mark the new tests with a clear
`SUBCASE("BUG: P0-1 capture, fix in next commit")` and run them as
**failing in their own preparatory commit**.  Use git bisect
discipline so a future bisect over the fix commit lands on a tree
where the test demonstrates the exact scope.

### Phase 3B — Existing test sweep

Run:
```bash
cd /tmp/gtopt-build-XXXX && ctest -j20 \
   -R "boundary|sddp_cut|cut_io|planning_method"
```
Expect every existing test still green.  Especially watch
`test_sddp_cut_io.cpp` — its "load_boundary_cuts_csv … shared mode
broadcasts" / "iteration filtering" / "name aliasing" cases are the
existing acceptance suite.

### Phase 3C — End-to-end regression

If the user's fix unblocks the juan/IPLP UB regression, ensure that
after Phase 2 the LB / UB convergence behaviour is **identical** —
i.e. our fix doesn't re-introduce the runaway.  Comparison artefact:
`support/juan/IPLP/...` LP/JSON outputs before and after.

---

## Phase 4 — Conditioning follow-ups (P1-3 / P1-4)

Schedule these as separate PRs after Phase 1-3 lands clean.

### Phase 4A — `scale_theta = median(X/V²)` (P1-3)

**File:** `source/planning_lp.cpp:56-90`

This is the unfinished fix from `support/lp_scale_audit_2026-04-26.md`
(noted in `feedback_infinity_scaling` memory).  Adds a regression
test on a mixed-voltage 66/220/500 kV fixture asserting kappa < 1e6
post-fix (current ratio ≈ 3.75e7).

### Phase 4B — release-then-read contract (P1-4)

**File:** `system_lp.cpp:1083-1086` + `linear_interface.cpp:138-222`

Document the post-`release_backend()` contract for `get_row_dual()` /
`get_col_sol()`: under `low_memory_mode != off`, the cached vectors
are valid only when `is_optimal() && !is_backend_released() ||
m_phase_2a_cached_`.  Add a `[[nodiscard]] is_release_cache_valid()`
predicate and assert it inside accessor preconditions.

This is purely a hardening commit — no current production path
violates the contract, but the lp-numerics audit flagged it as a
latent hazard.

---

## Phase 5 — Cleanup

### Phase 5A — Land strong-type lint fixes

Already coded in working tree (4 violations under `lp_context.hpp` +
`benders_cut.cpp`).  Build green.  Commit as the first preparatory
landing of this plan.

### Phase 5B — Re-run cpp26-modernizer

The previous run returned an incomplete report.  Re-run with a
tighter prompt scoped to ONE topic (e.g. just the `sddp_*` family)
to get a deliverable summary instead of an open-ended audit.

---

## Sequence summary (commit landing order)

1. `chore(strong-types): fix 4 redundant static_cast<Uid> sites` (Phase 5A)
2. `test(sddp): xfail boundary-cut round-trip + reservoir-name capture (P0-1, P2-2)` (Phase 1A + 1B)
3. `fix(sddp): include reservoir_array in boundary-cut name_to_class_uid (P2-2)` (Phase 2B → 1B graduates)
4. `fix(sddp): emit physical-space boundary cut rows; let compose_physical scale once (P0-1)` (Phase 2A → 1A graduates)
5. `refactor(sddp): thread cut_coeff_eps through boundary-cut add_cut_row (P2-1)` (Phase 2C)
6. (separate PR cycle) Phase 4A → 4B
7. `chore(modernization): cpp26 audit pass 2` (Phase 5B)

Steps 2–5 should ideally land within one calendar week so the bug
window in master is short.

---

## Open questions for the user before Phase 1 starts

1. The user mentioned the juan/IPLP UB error "seems fixed" — is that
   visible on master right now, or in a private branch?  If on
   master, the symptom is masked by *something* even though the
   double-scaling math still appears wrong; the failing test in
   Phase 1A would still demonstrate the underlying defect.
2. Are the in-flight fixes the user mentioned aimed at boundary cuts
   specifically?  If yes, hold the entire plan until they land and
   re-validate every finding line by line.
3. Any preference for `XFAIL`-style preparatory commits vs squashed
   "test+fix" commits?  Default proposal is preparatory commits for
   easier bisect.
