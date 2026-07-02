# RCA: Nondeterministic NaN in SDDP Backward-Cut Construction — 2026-07-02

**Test:** `SDDPMethod - ElasticFilterMode comparison on 2-reservoir fixture`
**Symptom:** `SDDP Backward [iN s0 p2/2]: cut z=nan (α≥nan, links=2/2)` followed by
multi-hour CPLEX hang; or, in the guarded build (bd269f80),
`SDDP Backward [iN s0 p2/2]: NON-FINITE cut dropped (z=-nan, rhs=-nan, links=3)`.
**Occurrence rate:** ~1-in-3 runs; always in the `chinneck` elastic-filter-mode sub-run.
**Containment guard committed:** bd269f80 (drops NaN cuts, promotes to LP_VALIDATION error).

---

## 1. Symptom

```
SDDP Forward [i1 s0 p1]: solve returned optimal but forward_objective is NaN (obj=-nan, alpha=0)
  [i1 backward: 0 optimality cuts — scene infeasible]
SDDP Forward [i2 s0 p2]: opex=30.812000K  ← FINITE, correct
SDDP Backward [i2 s0 p3/2]: cut z=30.812000K (α≥31.208000K, links=2/2)  ← OK
SDDP Backward [i2 s0 p2/2]: cut z=nan (α≥nan, links=2/2)  ← WEDGE / NON-FINITE
```

After the NaN cut is built without the guard: `src_li.resolve()` enters CPLEX with a row
containing NaN coefficients and spins for 13000+ seconds (CPU 20–80 %) until the test
runner kills the process.  With the bd269f80 guard the NaN cut is dropped and the run
converges normally (SDDP still finds the optimal solution with one fewer underestimator
that iteration).

---

## 2. Evidence chain from the logs

### 2a. Trigger (logs: `efx_3.log` lines 149-153, `wedged_test.log` lines 150-153)

In the `chinneck` sub-run (third call to `run_mode()` in the test), iteration i1:

1. Phase p1 solved OK: `opex=19.328000K`.
2. Phase p2 is **directly infeasible** on the first attempt (no prior p2 success in i1).
3. `chinneck_filter_solve` fires for p2's elastic clone.
   Log: `chinneck_filter_solve: IIS = 1 essential / 2 relaxed bounds (filtered 1 non-essential, obj=324000.0000)`
4. `build_multi_cuts` is called with the chinneck result, installs 2 multi-cuts on p1.
5. Phase backtracks to p1 (`--phase_idx`).
6. p1 is re-solved:
   - **Wedged run:** `SDDP Forward [i1 s0 p1]: solve returned optimal but forward_objective is NaN (obj=-nan, alpha=0)`.
   - **Guarded run:** same NaN, same log line (`sddp_forward_pass.cpp:887`).

The NaN is in `li.get_obj_value()` for the ORIGINAL p1 LP, not the elastic clone.

### 2b. NaN propagation at i2

The forward-pass NaN guard (line 887) fires and returns `std::unexpected(Error)` declaring
i1's scene infeasible (feasible=0/1 in iter stats).  The backward pass at i1 generates 0
optimality cuts.

At i2 forward, the elastic backtrack chain re-occurs: p3 infeasible → chinneck → fcut on
p2; p2 infeasible → chinneck → fcut on p1; all phases eventually resolved **with finite
objectives** (`opex=20.120K`, `30.812K`, `30.812K`).  The forward log for p2 reads
`opex=30.812000K` — a value written to `phase_states[p2].forward_full_obj_physical` at
`sddp_forward_pass.cpp:814`.

Yet the backward step `p2/2` reports `z=-nan`:

```
Backward [i2 s0 p3/2]: cut z=30.812000K ✓
Backward [i2 s0 p2/2]: cut z=nan  ← OVERWRITE AFTER FORWARD
```

`z` in the log is `target_state.forward_full_obj_physical`
(`sddp_method_iteration.cpp:683`), where `target_state = phase_states[p2]`.
This value was 30.812K after the forward pass.  It was **subsequently overwritten with
NaN** by the `backward_resolve_target` path at `sddp_method_iteration.cpp:494`.

### 2c. The wedge

Without bd269f80, the cut (z=nan, all link coefficients NaN) is installed in p1's LP via
`add_cut_row`.  `src_li.resolve()` at the end of `backward_pass_single_phase` enters
CPLEX with a row whose bound is `lowb = NaN`.  CPLEX's termination tests compare
infeasibility measures against NaN, which never satisfies them — the solver loops
indefinitely at 20–80 % CPU (pool monitor confirms `Active: 0/73  Pending: 0`; the hang
is inside CPLEX, not in gtopt's thread pool).

---

## 3. Root-cause analysis

### 3.1 Root cause A (PRIMARY): non-finite row dual from chinneck re-solve

`chinneck_filter_solve` runs in four phases (source: `benders_cut.cpp:1007-1127`):

| Phase | Action |
|-------|--------|
| 1 | `elastic_filter_solve` — clone p2's LP, relax state-var bounds, solve Phase-1 penalty LP |
| 2 | Classify slacks: identify which relaxed bounds are active (essential) vs inactive |
| 3 | Zero the upper bounds on **non-essential** slack columns, forcing them to zero |
| 4 | `clone.resolve(opts)` — re-solve the tighter LP to confirm the IIS |

Phase 4 resolves an LP that is near (or on) the infeasibility boundary of the ORIGINAL
phase LP.  The tighter bound-box from Phase 3 can produce a **degenerate basis** in
CPLEX: multiple basis matrices are optimal, and CPLEX's warm-start from the Phase-1 basis
can reach a numerically unstable vertex.  At that vertex CPLEX reports status 0 (optimal)
but `get_row_dual()` returns NaN for one or more fixing-equation rows.

`build_multi_cuts` (`benders_cut.cpp:1219-1273`) reads:

```cpp
auto duals = elastic.clone.get_row_dual();          // line 1219
// ...
const double pi = -duals[info.fixing_row];           // line 1273
```

The existing magnitude filter at line 1316:

```cpp
if (std::abs(pi) < cut_coeff_eps * slack_cost_max)  // misses NaN
```

does **not** exclude NaN because `std::abs(NaN) < threshold` evaluates to `false` in
IEEE 754.  The NaN `pi` propagates into:

```cpp
row[link.source_col] = pi;                           // NaN coefficient
row.lowb -= pi * v_hat_phys;                         // NaN RHS
```

The resulting `SparseRow` has NaN entries, which `li.add_rows(mc_cuts, ...)` installs
into p1's LP at `sddp_forward_pass.cpp:517`.

The same defect exists in `build_feasibility_cut_physical` (`benders_cut.cpp:364`), which
also reads `pi = -duals[info.fixing_row]` and filters only by `std::abs(pi) < cut_coeff_eps`.

### 3.2 Root cause B (SECONDARY propagation path): backward_resolve_target overwrites good forward-cached state

`backward_resolve_target` (default `true`) re-solves the **target** LP (p2 in the p2/2
step) inside `backward_pass_single_phase` at `sddp_method_iteration.cpp:485-523`:

```cpp
auto r = tgt_li.resolve(opts);                                     // line 486
if (r.has_value() && tgt_li.is_optimal()) {
    const auto obj_phys = tgt_li.get_obj_value();                  // NaN if fcut poisoned LP
    ...
    phase_states[phase_index].forward_full_obj_physical = obj_phys; // line 494 ← OVERWRITES
```

p2's LP at this point contains the NaN fcut row installed during the forward pass's
elastic path (when p3 was infeasible → chinneck → fcut on p2 with NaN coefficient from
Root Cause A).  When `tgt_li.resolve()` re-solves this LP — now with the freshly-added
optimality cut from the "p3/2" backward step changing the active basis — CPLEX exercises
the NaN row and returns `get_obj_value() = NaN`.

There is **no finiteness guard** on `obj_phys` before it is stored at line 494.  This
overwrites `phase_states[p2].forward_full_obj_physical` from its forward-pass value
(30.812K) to NaN, producing `target_state.forward_full_obj_physical = NaN` when
`backward_pass_single_phase` reads it at line 576 to build the Benders cut.

### 3.3 Why it is nondeterministic (~1-in-3 occurrence rate)

The chinneck Phase-4 re-solve LP is a modified clone of p2's elastic LP with some slack
upper bounds zeroed.  Whether CPLEX hits a degenerate vertex that produces NaN duals
depends on:

- The warm-start basis carried over from Phase 1 (Phase-4 starts from Phase-1's optimal
  basis, modified by the Phase-3 bound changes).
- CPLEX's internal numerics for the specific basis matrix: the LP is in a near-degenerate
  region (the IIS-confirmed infeasibility boundary), so small numerical perturbations can
  cause the dual solution to blow up.
- The solve is single-threaded and deterministic per run, but the basis at the start of
  Phase 4 can differ slightly between runs depending on CPLEX's internal dynamic data
  structure ordering, which is sensitive to memory layout (allocation timing, ASLR).

### 3.4 Why it clusters at s0 p2/2 in early iterations of the chinneck sub-run

The 2-reservoir fixture uses 3 phases with a "forced infeasibility" scenario that makes
p2 infeasible on the first forward attempt in iteration i1 (before any optimality cuts
exist on α^{p1}).  The chinneck mode fires immediately when p2 is first encountered.  No
other mode (`single_cut`, `multi_cut`) uses `chinneck_filter_solve`, so the NaN can only
appear in the chinneck sub-run.  The fixture converges in exactly 2 iterations normally,
so the NaN can only occur in i1 or i2.  At i2, the "p2/2" backward step is the first
step that involves both a NaN fcut row (in p2's LP from i2's forward chinneck on p3) AND
the new optimality cut from "p3/2" that changes the active basis, causing the target
re-solve to hit the NaN path.

---

## 4. Causal chain summary

```
chinneck_filter_solve (benders_cut.cpp:1081)
  → Phase-4 clone.resolve() → degenerate basis → NaN duals
  → build_multi_cuts (benders_cut.cpp:1273): pi = -duals[...] = NaN
  → NaN fcut row installed in predecessor LP (e.g. p1 or p2) via add_rows
       ↓
Forward p1 re-solve with NaN fcut in LP
  → CPLEX returns optimal but obj = NaN
  → sddp_forward_pass.cpp:814: phase_states[p1].forward_full_obj_physical = NaN
  → NaN guard (line 887): returns error, i1 declared infeasible
       ↓
i2 forward chinneck on p3 → NaN fcut also installed on p2's LP
       ↓
i2 backward "p2/2": backward_resolve_target re-solves p2 (now has NaN fcut)
  → sddp_method_iteration.cpp:490: obj_phys = tgt_li.get_obj_value() = NaN
  → sddp_method_iteration.cpp:494: phase_states[p2].forward_full_obj_physical = NaN
       ↓
backward cut built: z = target_state.forward_full_obj_physical = NaN
  → cut.lowb = NaN, cut.cmap[source_col] = NaN
       ↓
WITHOUT bd269f80 guard: NaN cut installed in p1's LP → CPLEX loops forever (WEDGE)
WITH bd269f80 guard:    NON-FINITE cut dropped → error logged → run recovers
```

---

## 5. Fix proposals

### Fix A — Primary fix: guard pi for finiteness in `build_multi_cuts`
**File:** `source/benders_cut.cpp`, line 1273
**Current:**
```cpp
const double pi = -duals[info.fixing_row];

// ... existing filter:
if (std::abs(pi) < cut_coeff_eps * slack_cost_max) {
    continue;
}
```
**Proposed** (insert after line 1273):
```cpp
const double pi = -duals[info.fixing_row];
if (!std::isfinite(pi)) {
    SPDLOG_WARN(
        "build_multi_cuts: non-finite dual pi={} at fixing_row={} link={}"
        " — elastic clone LP has numerically unstable basis; skipping link",
        pi,
        static_cast<int>(info.fixing_row),
        link_idx);
    ++link_idx;
    continue;
}
```

This prevents any NaN or Inf coefficient from entering the fcut row, blocking the
contamination at its source.  The resulting cut will have fewer links (or zero links if
all duals are NaN), which is safe — a cut with no state-variable terms degenerates to a
feasibility constant that is always satisfied and is discarded by the `cmap.empty()` check
downstream.

### Fix B — Also guard pi in `build_feasibility_cut_physical`
**File:** `source/benders_cut.cpp`, line 364
**Current:**
```cpp
const double pi = -duals[info.fixing_row];
if (std::abs(pi) < cut_coeff_eps) {
    continue;
}
```
**Proposed** (add finiteness check after the existing `std::abs(pi) < cut_coeff_eps` line):
```cpp
const double pi = -duals[info.fixing_row];
if (!std::isfinite(pi) || std::abs(pi) < cut_coeff_eps) {
    continue;
}
```

### Fix C — Defense in depth: guard backward_resolve_target against NaN obj
**File:** `source/sddp_method_iteration.cpp`, lines 489-494
**Current:**
```cpp
if (r.has_value() && tgt_li.is_optimal()) {
    const auto obj_phys = tgt_li.get_obj_value();
    const auto sol_phys = tgt_li.get_col_sol();
    const auto rc = tgt_li.get_col_cost_raw();
    capture_state_variable_values(scene_index, phase_index, sol_phys, rc);
    phase_states[phase_index].forward_full_obj_physical = obj_phys;
```
**Proposed:**
```cpp
if (r.has_value() && tgt_li.is_optimal()) {
    const auto obj_phys = tgt_li.get_obj_value();
    if (!std::isfinite(obj_phys)) {
        SPDLOG_WARN(
            "SDDP Backward [i{} s{} p{}/{}]: backward_resolve_target "
            "returned non-finite obj={} — keeping forward-cached z={} "
            "(likely NaN fcut row in target LP from elastic path)",
            gtopt::uid_of(iteration_index),
            uid_of(scene_index),
            uid_of(phase_index),
            bwd_total_phases,
            obj_phys,
            phase_states[phase_index].forward_full_obj_physical);
        // Fall through to use the forward-cached value.
    } else {
        const auto sol_phys = tgt_li.get_col_sol();
        const auto rc = tgt_li.get_col_cost_raw();
        capture_state_variable_values(scene_index, phase_index, sol_phys, rc);
        phase_states[phase_index].forward_full_obj_physical = obj_phys;
    }
```

Fix C alone would have prevented the wedge and the NON-FINITE error, because the valid
forward-pass value (30.812K) would never have been overwritten.  Fix A is the deeper
structural fix that stops NaN from entering the LP at all.  Both should be applied.

### Discriminating experiment (cheap)
To confirm Fix A suffices: add an `assert(std::isfinite(pi))` at
`benders_cut.cpp:1273` in a debug build and run the test 30 times.  If the assertion
fires consistently when the NaN symptom would have occurred, the diagnosis is confirmed.
Alternatively, enable `spdlog::set_level(spdlog::level::trace)` and check for
`get_row_dual() → NaN` log output from the chinneck Phase-4 solve.

---

## 6. Why the existing `std::abs(pi) < threshold` filter misses NaN

IEEE 754 mandates that any comparison involving NaN evaluates to `false` except `!=`.
Therefore `std::abs(NaN) < cut_coeff_eps * slack_cost_max` is `false`, so NaN `pi`
passes the filter.  The fix must use `std::isfinite(pi)` (or equivalently
`!(pi == pi)` as a NaN test, but `std::isfinite` is clearer and also catches Inf).

---

## 7. Effect on marginal theory and reported outputs

The NaN originates in a **feasibility cut** (fcut) row, not in an optimality cut.
Feasibility cuts constrain master state variables to exclude infeasible trial points; they
carry no dual contribution to LMPs or water values.  Dropping a NaN fcut (Fix A or the
bd269f80 guard) does NOT change any marginal-cost output.  Fix C (backward_resolve_target
guard) preserves the forward-cached objective and reduced costs, which ARE used for
optimality cut construction; since the preserved value is the correct forward-pass value,
the cut is unchanged from what would have been built without the NaN contamination.

No downstream output code, post-processing layer, or LMP/water-value computation needs
to be updated for these fixes.

---

## 8. Files and line references (confirmed by source read)

| Location | Role |
|----------|------|
| `source/benders_cut.cpp:1273` | Primary NaN source — `pi = -duals[fixing_row]` in `build_multi_cuts` |
| `source/benders_cut.cpp:364` | Secondary NaN source — same expression in `build_feasibility_cut_physical` |
| `source/benders_cut.cpp:1316` | Existing magnitude filter — does NOT catch NaN |
| `source/sddp_forward_pass.cpp:814` | Writes `forward_full_obj_physical = li.get_obj_value()` (can be NaN if LP poisoned) |
| `source/sddp_forward_pass.cpp:828` | `capture_state_variable_values` called before NaN guard |
| `source/sddp_forward_pass.cpp:879` | NaN guard on `forward_objective` — fires after state already written |
| `source/sddp_forward_pass.cpp:517` | `li.add_rows(mc_cuts, ...)` — installs the NaN fcut row |
| `source/sddp_method_iteration.cpp:489-494` | `backward_resolve_target` overwrites `phase_states[phase].forward_full_obj_physical` without finiteness guard |
| `source/sddp_method_iteration.cpp:576` | `build_benders_cut_physical` reads `target_state.forward_full_obj_physical` as `z` |

---

## 9. Verification plan

1. Apply Fix A (`benders_cut.cpp:1273` finiteness guard) and Fix C (`sddp_method_iteration.cpp:489` guard).
2. Run the failing test 50 times:
   ```bash
   for i in $(seq 50); do
     ./build/test/gtoptTests -tc="ElasticFilterMode comparison on 2-reservoir"
   done
   ```
3. Confirm: no `NON-FINITE cut dropped` error lines, no hangs.
4. Confirm: `cut z=30.812000K` and `cut z=61.624000K` appear for ALL `p3/2` and `p2/2`
   backward steps in every run (the dropped NaN cut does not affect convergence because
   SDDP finds the correct lower bound at i2 even with one fewer underestimator from i1's
   chinneck event).
5. If `SDDP Backward [iN s0 p2/2]: backward_resolve_target returned non-finite obj` is
   ever logged after Fix A, that indicates a second NaN path through the elastic clone
   duals — escalate.

The kappa of the 2-reservoir fixture stays below 200 throughout, so LP ill-conditioning
is not a confounding factor.  The NON-FINITE guard in bd269f80 remains as a last-resort
safety net even after the structural fixes.
