# LP Scaling Stack Audit — 2026-04-26

## 1. Scope

**Target:** The `add_row(SparseRow&, eps)` compose_physical path fixed at
`source/linear_interface.cpp:990-1017`.  Covers the full round-trip from
`build_benders_cut_physical` through `add_row` insertion, solver solve, dual
readback via `get_col_cost` / `get_row_dual`, and the next backward-pass cut
construction.

**Files read:**

| File | Key lines read |
|---|---|
| `include/gtopt/linear_interface.hpp` | 59-180 (ScaledView), 1100-1200 (row getters), 1290-1454 (col_cost, col_scale, row_scale, scale_objective accessors), 1483-1516 (get_row_dual) |
| `source/linear_interface.cpp` | 870-1071 (add_row dispatch + compose_physical path) |
| `source/linear_problem.cpp` | 340-431 (flatten col_scale / obj assembly), 550-730 (equilibration + scale_objective application) |
| `source/benders_cut.cpp` | 99-300 (all three `build_benders_cut_physical` overloads + feasibility cut) |
| `source/sddp_method.cpp` | 400-478 (alpha column setup) |
| `source/sddp_aperture.cpp` | 370-405 (cut construction call site) |
| `test/source/test_linear_interface_cut_scaling.cpp` | full |
| `test/source/test_linear_interface_scale.cpp` | full |
| `test/source/test_linear_interface.cpp` | 1-80 |
| `test/source/test_linear_problem.cpp` | 1-80 |

---

## 2. Symptom Summary (pre-fix, for reference)

- **Global coefficient range:** Not re-read from a fresh log — empirical data
  from the context: LMAULE cut coefficient 4.2e+5 → 4.2e+8 → 4.2e+11 across
  three consecutive backward phases under `scale_objective=1000`.
- **Growth pattern:** compounding (× 1000 per backward phase), not present with
  `scale_objective=1`.
- **Max kappa:** ≈ 8.87e+41 at SDDP iteration 2 forward (juan/gtopt_iplp).
- **Verdict at time of symptom:** SEVERE (ratio ≥ 1e8, kappa ≥ 1e11).

---

## 3. Section 1 — Scale-Convention Correctness

### 3.1 Constraint-row convention

`source/linear_problem.cpp:344-398` establishes:

```
LP_coeff(row i, col j) = phys_coeff(i,j) × col_scale(j) / row_scale(i)
```

where `col_scale(j) = SparseCol.scale` and `row_scale(i)` is the row
equilibration divisor (max-abs per row, or Ruiz iterative).  At line 369:

```cpp
const auto v = v_raw * cs * inv_rs;   // cs = col_scale[j], inv_rs = 1/row.scale
```

This is correct: `physical = LP × col_scale / row_scale_inv = LP × col_scale ×
row_scale`, which inverts to `LP = physical × col_scale_inv ... ` — wait, let
me be precise.

**Convention (from the code comment at lp 346-347):**
```
physical_value = LP_value × col_scale
```
So `LP_value = physical_value / col_scale`.  The stored LP matrix coefficient
for a physical constraint `Σ a_ij · x_phys_j ≥ b_i` is:

```
LP_coeff(i,j) = a_ij × col_scale(j)   [pre-equilibration]
LP_coeff(i,j) = a_ij × col_scale(j) / row_scale(i)  [post-equilibration]
```

because `Σ a_ij × x_phys_j = Σ a_ij × col_scale(j) × x_LP_j`, and then the
whole row is divided by `row_scale(i)`.  **Correct.**

### 3.2 Objective-row convention

`source/linear_problem.cpp:424-430`:
```cpp
objval[i] = col.cost * s;   // s = col_scale
```

Then `source/linear_problem.cpp:725-729`:
```cpp
for (auto& v : objval) { v *= inv_scale; }   // inv_scale = 1 / scale_objective
```

Net LP objective coefficient: `cost_phys × col_scale / scale_objective`.

Verification: the primal objective value = `Σ LP_obj_j × LP_x_j`
= `Σ (cost_phys_j × col_scale_j / scale_obj) × (x_phys_j / col_scale_j)`
= `(1/scale_obj) × Σ cost_phys_j × x_phys_j`.
So `obj_phys = obj_LP × scale_obj`.  **Correct.**

### 3.3 SDDP cut-row convention (what it should be)

A Benders optimality cut in physical space is:

```
α_phys ≥ Z_phys + Σ_j rc_phys_j × (state_j_phys − v̂_j_phys)
```

Equivalently, treating α as any other column:

```
1 × α_phys − Σ_j rc_phys_j × state_j_phys ≥ Z_phys − Σ_j rc_phys_j × v̂_j_phys
```

This is a constraint row with:
- Physical coefficient for α: `1.0`
- Physical coefficient for state_j: `−rc_phys_j`
- Physical RHS: `Z_phys − Σ_j rc_phys_j × v̂_j_phys`

Applying the LP-space conversion (`LP_coeff = phys_coeff × col_scale /
scale_objective`):

```
LP_coeff(α)       = 1.0 × col_scale_α / scale_objective
LP_coeff(state_j) = −rc_phys_j × col_scale_j / scale_objective
LP_RHS            = phys_RHS / scale_objective
```

**The RHS division by `scale_objective` is the key insight.**  Without it,
the cut row is `scale_objective` times too large relative to the OBJ row.

### 3.4 Does the fix produce the correct LP-space form?

The compose_physical path at `linear_interface.cpp:979-1017`:

**Step 1 (col scaling, line 983-988):**
```cpp
elements[k] *= m_col_scales_[col];   // phys_coeff × col_scale
```

**Step 1b (/scale_objective, lines 1006-1017):**
```cpp
v *= inv_so;   // × (1/scale_objective)
lb *= inv_so;
ub *= inv_so;
```

After steps 1 and 1b:
```
element_k = phys_coeff_k × col_scale_k / scale_objective   ✓
lb        = phys_RHS / scale_objective                      ✓
```

**Step 2 (row.scale composition, lines 1022-1034):**
```cpp
v *= inv_rs;   // divide by SparseRow::scale (user-supplied row divisor)
```

For `build_benders_cut_physical`, `row.scale = 1.0` (the field is never set in
any of the three overloads at `benders_cut.cpp:114,146,180`).  So step 2 is a
no-op for production cuts.  **Correct.**

**Step 3 (row-max equilibration, lines 1041-1058):**
```cpp
max_abs = max over elements after steps 1+1b+2;
elements[k] *= 1/max_abs;
lb *= 1/max_abs;
composite_scale *= max_abs;  // total row_scale = row.scale × max_abs
```

After step 3, the raw LP matrix entry is:
```
LP_coeff_k = phys_coeff_k × col_scale_k / (scale_objective × composite_scale)
LP_lb_raw  = phys_RHS / (scale_objective × composite_scale)
```

`composite_scale` is stored as `m_row_scales_[row_idx]` via `set_row_scale()`.

### 3.5 get_col_cost round-trip (`linear_interface.hpp:1313-1330`)

```cpp
// ScaledView with Op::divide, global_factor = m_scale_objective_
// operator[](i) = data[i] / scale[i] * global
//              = LP_rc[i] / col_scale[i] * scale_objective
```

So `get_col_cost()[j] = LP_rc_j × scale_objective / col_scale_j`.

For the LP: `LP_rc_j = partial_LP_obj / partial_LP_x_j`.  At optimality for a
non-basic variable, `LP_rc_j = LP_obj_coeff_j − Σ dual_i × LP_coeff(i,j)`.

The LP obj coefficient for state_j is `cost_phys_j × col_scale_j /
scale_objective`.  So:

```
LP_rc_j ~ LP_obj_j / scale_objective = (cost_phys_j × col_scale_j / scale_obj) / ...
```

The dual-based part is a constraint combination and involves `row_scale`.  The
exact value is model-specific, but the physical RC recovered by:

```
phys_rc_j = LP_rc_j × scale_objective / col_scale_j
```

is correct for any LP where coefficients were installed as
`phys × col_scale / scale_objective`.  **Consistent with the fix.**

### 3.6 Round-trip: solve at phase p+1 → extract rc_phys → build cut → install at phase p → solve at p

**Phase p+1 (solved):**
- `get_col_cost()[state_j]` = `LP_rc_j × scale_objective / col_scale_j` = `rc_phys_j`

**build_benders_cut_physical (benders_cut.cpp:99-133):**
```cpp
row[alpha_col] = 1.0;
row[link.source_col] = -rc_phys;
row.lowb = obj_phys - Σ rc_phys × v_hat_phys;
```

All values are physical.

**add_row at phase p (compose_physical path):**
- Step 1: `1.0 × col_scale_alpha`, `−rc_phys_j × col_scale_j`
- Step 1b: divide all by `scale_objective`
- LP element for alpha: `col_scale_alpha / scale_objective`
- LP element for state_j: `−rc_phys_j × col_scale_j / scale_objective`
- LP_lb: `(obj_phys − Σ rc_phys × v_hat) / scale_objective`

**LP at phase p solved:**
- The cut now lives in the same LP-space scale as all constraint and objective
  rows.
- No compounding: the `/scale_objective` is applied exactly once at insertion,
  regardless of how many times the backward pass runs.

**Conclusion: the fix is dimensionally correct for the full round-trip.**

### 3.7 Alpha column analysis

From `sddp_method.cpp:434-453`:
```cpp
.cost = scale_alpha,       // = 1.0
.scale = scale_alpha,      // = 1.0  → col_scale_α = 1.0
```

OBJ row: `LP_cost_α = scale_alpha × col_scale_α / scale_objective = 1/scale_objective`.

Cut row (post-fix): `LP_coeff_α = 1.0 × col_scale_α / scale_objective = 1/scale_objective`.

**Both are identical.**  If `scale_objective=1000`, both carry `1/1000` in the
LP.  The LP sees consistent relative weight between the OBJ row and every cut
row's α coefficient.  **Alpha is fully consistent post-fix.**

### 3.8 Equilibration interaction

When `have_equilibration = true`, the row-max pass (step 3) fires after the
`/scale_objective` step.  The `max_abs` it picks is computed on
`phys_coeff × col_scale / scale_objective`, which is already in LP-space.
Then `composite_scale = row.scale × max_abs`.

`get_row_low()` returns `raw_lb × composite_scale` (the ScaledView at
`linear_interface.hpp:1133` uses `Op::multiply` with `m_row_scales_`).

So: `get_row_low()[cut_row] = (phys_RHS / (scale_objective × composite_scale)) × composite_scale = phys_RHS / scale_objective`.

This is **NOT** the physical RHS — it is `phys_RHS / scale_objective`.

**This is a subtle invariant break in `get_row_low()` for cut rows.**  The
physical getter for structural rows returns `raw_lb × row_scale = phys_RHS`
(because structural row's `raw_lb = phys_RHS / row_scale` and `row_scale =
equilibration_divisor`).  For a cut row after the fix, `raw_lb = phys_RHS /
(scale_objective × max_abs)` and `row_scale = max_abs`, so
`get_row_low()[cut] = phys_RHS / scale_objective ≠ phys_RHS`.

**Whether this matters** depends on who calls `get_row_low()` on cut rows.
Current usage of `get_row_low()` in the codebase is primarily for debugging and
output; the SDDP backward pass reads `get_col_cost()` (for α and state rc), not
`get_row_low()`.  However: any future caller that uses `get_row_low()[cut]` as
the physical RHS of the cut will get a wrong value (off by `scale_objective`).

The `get_row_dual()` path (used by `build_feasibility_cut_physical` at
`benders_cut.cpp:229`) uses:
```
dual_physical = raw_dual × scale_objective / composite_scale
```
For the cut row: `raw_dual × scale_objective / (row.scale × max_abs)`.  This
correctly recovers the physical dual because the LP solver applies
`row_scale` to the row so the raw dual is `raw_dual = phys_dual × max_abs /
scale_objective`.  **Correct.**

**Summary of equilibration interaction:** The dual readback via `get_row_dual()`
is correct.  The bound readback via `get_row_low()` on a cut row returns
`phys_RHS / scale_objective`, not `phys_RHS`.  This is not wrong for SDDP
(cuts are forward-only; nobody reads back the RHS of an installed cut from the
LP), but it is a naming/documentation inconsistency that should be called out
as a risk for future callers.

---

## 4. Section 2 — Unit-Test Honesty Audit

### 4.1 Is `test_linear_interface_cut_scaling.cpp` SUBCASE "scale_objective>1: stored RHS = phys_rhs / scale_objective()" a real invariant or a tautology?

The check at `test_linear_interface_cut_scaling.cpp:142-143`:
```cpp
CHECK(li.get_row_low_raw()[row_idx] == doctest::Approx(cut.lowb / scale_obj));
```

This asserts `raw_lb = phys_RHS / scale_obj`.  With equilibration disabled
(`opts.equilibration_method = LpEquilibrationMethod::none`), step 3 of
compose_physical is skipped, so `composite_scale = row.scale = 1.0`, and
`set_row_scale()` is not called.  The raw lb stored is exactly:
```
phys_RHS × (col_scale for elements) / scale_obj / row.scale = phys_RHS / 1000
```
(cut only has c0 with col_scale=1.0 from the fixture's col0 having no `.scale`,
and the col1_scale=2.0 is on c1 which is not in this particular cut).

Wait — the cut at line 140: `cut[ColIndex{0}] = 1.0`, so only c0 with
`col_scale=1.0`.  After step 1: element = `1.0 × 1.0 = 1.0`; after step 1b:
`lb = 1.8e+8 / 1000 = 1.8e+5`.  The raw lb is `1.8e+5`.  The assertion checks
`1.8e+8 / 1000 = 1.8e+5`.  **This is a genuine invariant, not a tautology.**

But it is only genuine for the path where:
- Equilibration is off
- `row.scale == 1.0`
- The cut's only non-zero element touches `col_scale=1.0`

The test documents this in its comment at lines 62-68.

### 4.2 Does disabling equilibration hide a real bug?

**Yes, conditionally.**

With equilibration on (`row_max` or `ruiz`), the `composite_scale` absorbs the
`max_abs` of the post-`/scale_objective` elements.  The raw lb becomes
`phys_RHS / (scale_objective × max_abs)`.  `get_row_low_raw()[cut]` returns
this smaller value, not `phys_RHS / scale_objective`.

If the test were run with equilibration on and used `get_row_low_raw()`, it
would **fail** — not because the fix is wrong, but because the assertion's
expected value would need to account for `max_abs`.

More importantly: there is no test that checks the dual readback for a cut row
with equilibration on.  The production paths that matter (SDDP backward pass)
use `get_col_cost()` for rc_phys (confirmed correct above) and
`get_obj_value_physical()` for Z_phys, not `get_row_low_raw()`.  But the gap
remains: no test pins that `get_row_dual()[cut_row]` returns the correct
physical dual after insertion with equilibration active.

### 4.3 The "compounding-bug invariant" SUBCASE (lines 81-106)

```cpp
CHECK(row_lows[row_a] == doctest::Approx(row_lows[row_b]));
CHECK(row_lows[row_b] == doctest::Approx(row_lows[row_c]));
```

This correctly pins the non-compounding property: identical input cuts produce
identical stored raw bounds.  Pre-fix, each successive `add_row` would have
included another `scale_objective` factor because (without the fix) the RHS was
not divided.  **This is a genuine regression pin.**

However, it does NOT verify the *magnitude* — it only checks that three copies
are equal.  A bug that scales all three identically would pass this test.  The
magnitude is checked by SUBCASE "scale_objective>1" above.

### 4.4 Infinite upper-bound SUBCASE (lines 146-161)

```cpp
CHECK(li.get_row_upp_raw()[row_idx] >= 1.0e+19);
```

The fix guards finite bounds with `lb > -infy && lb < infy` at lines 1011-1016.
`DblMax` is passed as `uppb`; it is normalized to solver infinity (≥ 1e20 for
CPLEX/CLP) in `normalize_bound()` during the base `add_row()` call.  The
assertion correctly pins that the upper bound was not divided.  **Genuine
invariant.**

### 4.5 Existing tests that match buggy behavior

Scanning `test_linear_interface_scale.cpp` (Tests 1-8) and
`test_linear_interface.cpp` — no test uses `add_row()` in the cut phase
(i.e., after `save_base_numrows()`) with a physical SparseRow.  All tests
build the LP purely through `add_col` / structural `add_row` paths.

`test_linear_interface_scale.cpp:Test 3` (lines 219-272) verifies
`get_col_cost()` via a solved LP — this is correct and would catch a
wrong formula in `get_col_cost`.  But it does NOT test `add_row` in the
cut phase.

`test_linear_problem.cpp` (lines 1-80 read) — tests basic SparseCol/SparseRow
operations, no scale round-trips.

**No test in the existing suite would have caught the original bug because none
exercise the compose_physical path.**

### 4.6 Gaps in existing tests

| Gap ID | Missing invariant | Relevant existing file |
|---|---|---|
| G-cut-1 | `add_row(physical)` with equilibration=row_max + solve → `get_row_dual()[cut] == phys_dual` | `test_linear_interface_cut_scaling.cpp` (new SUBCASE needed) |
| G-cut-2 | Round-trip: cut row added pre-flatten vs cut row added post-flatten with same physical coefficients yields same primal-dual pair | new TEST_CASE |
| G-cut-3 | SDDP 2-phase compounding regression: 2 backward phases with solved LP → second cut's coefficients are same magnitude as first (actual solve, not just identity check) | new TEST_CASE |
| G-obj-1 | `get_obj_value_physical() == Σ cost_phys_j × sol_phys_j` exactly (existing Test 6 checks `raw × 1000` but not symbolic sum) | `test_linear_interface_scale.cpp` Test 6 (minor) |

---

## 5. Section 3 — Concrete Test Recommendations

### Rec T1: Equilibration + cut dual round-trip (closes G-cut-1)

**File:** `test/source/test_linear_interface_cut_scaling.cpp`
**TEST_CASE:** Add a new SUBCASE within the existing TEST_CASE, or a new TEST_CASE.

```cpp
SUBCASE("equilibration=row_max: cut dual round-trip")
{
  // Build with row_max equilibration ON.
  LinearProblem lp;
  const auto c0 = lp.add_col({.uppb=100.0, .cost=1.0, .scale=2.0});
  const auto c1 = lp.add_col({.uppb=100.0, .cost=0.5});
  SparseRow base_row;
  base_row[c0] = 1.0;
  base_row[c1] = 1.0;
  base_row.greater_equal(10.0);
  std::ignore = lp.add_row(std::move(base_row));

  LpMatrixOptions opts;
  opts.scale_objective = 1000.0;
  opts.equilibration_method = LpEquilibrationMethod::row_max;
  LinearInterface li;
  li.load_flat(lp.flatten(opts));
  li.save_base_numrows();

  // Simulate a backward-pass cut: alpha=1, state_c0=-2.5, RHS=50.0 (physical).
  SparseRow cut;
  cut[c0] = 1.0;   // alpha col (using c0 as proxy)
  cut[c1] = -2.5;  // state col (physical rc = 2.5)
  cut.lowb = 50.0; // physical RHS
  cut.uppb = LinearProblem::DblMax;
  const auto cut_idx = li.add_row(cut);

  // Solve to get a dual on the cut.
  const auto status = li.initial_solve({});
  REQUIRE((status && *status == 0));

  // get_row_dual() must return the physical dual (corrected for scale_obj
  // and composite_scale).  Physical dual of the cut = scale_obj / composite_scale
  // × raw_dual.  Since composite_scale = row.scale × max_abs_LP_elements,
  // and max_abs = max(|col_scale_c0/scale_obj|, |2.5 × col_scale_c1/scale_obj|)
  // = max(2.0/1000, 2.5/1000) = 2.5/1000, composite = 2.5/1000:
  // dual_phys = raw_dual × 1000 / (2.5/1000) = raw_dual × 4e5.
  // We just assert it is non-zero and consistent with raw × scale_obj / row_scale.
  const auto dual_raw = li.get_row_dual_raw();
  const auto dual_phys = li.get_row_dual();
  const double row_scale = li.get_row_scale(cut_idx);
  const double expected = dual_raw[cut_idx] * li.scale_objective() / row_scale;
  CHECK(dual_phys[cut_idx] == doctest::Approx(expected).epsilon(1e-8));
}
```

**Physical invariant pinned:** `dual_phys = raw_dual × scale_objective / composite_scale`.

### Rec T2: Pre-flatten vs post-flatten cut equivalence (closes G-cut-2)

**File:** new `test/source/test_linear_interface_cut_round_trip.cpp`

**TEST_CASE:** `"LinearInterface - physical cut pre/post flatten yields same primal-dual"`

```cpp
// Build:  min α  s.t.  α ≥ 100 + 3·x1, x1 ∈ [0,20], α ∈ [0,∞)
// This is trivially: x1=0, α=100.
// Build once with the cut as a structural row (pre-flatten),
// once with the cut as a post-save_base_numrows add_row (physical).
// The primal solution and objective must match.
```

**Physical invariant pinned:** The primal-dual solution from a physical cut
installed post-flatten is identical (within solver tolerance) to the same cut
installed as a structural row.

### Rec T3: Multi-phase compounding regression with actual solve (closes G-cut-3)

**File:** `test/source/test_linear_interface_cut_scaling.cpp` or new file.

**TEST_CASE:** `"LinearInterface - SDDP 2-phase cut compounding regression (solved)"`

```cpp
// Phase 2 LP: min x, x ≥ 5. Solve → obj_phys = 5.
// Build cut with alpha_col, rc_phys=1.0, v_hat=5: cut RHS = 5 + 1*(x2-5) = x2.
// → cut: α ≥ 5 + 1*(x − 5) for state x.
// Install in Phase 1 LP (same template).
// Solve Phase 1 → check α == 5, NOT 5000 (the pre-fix compounding value).
// Repeat for a second backward pass: install second identical cut.
// CHECK: both cuts have identical raw lb (regression pin).
// CHECK: solve Phase 0 → α == 5 (not compounded).
```

**Physical invariant pinned:** The RHS of the N-th cut equals the RHS of the
first cut (no compounding factor of `scale_objective^N`).

---

## 6. Section 4 — Verdict

### (A) / (B) / (C) Classification

The fix is **numerically correct for the production code path** (equilibration
disabled in juan/gtopt_iplp, `--no-scale`).

However, it is placed at the **insertion point** rather than being an invariant
of the extraction side.  A cleaner design would be:

**Alternative (B-style placement):** `get_col_cost()` already applies
`× scale_objective / col_scale`.  If `build_benders_cut_physical` consumed the
result of `get_col_cost()` directly (which it does — at `benders_cut.cpp:121,
156, 187-188`), then the physical rc is already correct.  The problem was
exclusively on the **insertion** side: the physical RHS/elements from
`build_benders_cut_physical` were passed to `add_row` without the
`/scale_objective` divide.

The fix inserts the divide at `add_row` time, which is correct but makes the
`add_row(SparseRow)` contract implicitly depend on `m_scale_objective_`.  An
alternative would be to have `build_benders_cut_physical` pre-divide by
`scale_objective` and mark the row as LP-space (`already_lp_space = true`), so
the contract of `add_row` stays "physical input → LP output" without needing to
know `scale_objective` inside the insertion path.

**Why the current placement is still acceptable:** The `compose_physical` branch
is specifically gated by `is_cut_phase && (have_col_scales || have_equilibration)`,
meaning it is only entered for post-flatten cuts.  This gate naturally captures
all SDDP cut insertions.  The comment at lines 990-1005 documents the rationale
clearly.

**Verdict: (A) Fix is correct and minimal.  Pin better tests per Section 3.**

No change to production code is recommended.  The alternative (B) placement
would require `build_benders_cut_physical` to receive `scale_objective` as a
parameter on every overload, which is more invasive and less discoverable than
the current single fix point.

---

## 7. Section 7 — Marginal-Theory Impact

### 7.1 Reduced costs (get_col_cost)

`get_col_cost()[j] = LP_rc_j × scale_objective / col_scale_j`.

**Pre-fix:** Cut rows were `scale_objective` times too large.  This distorted
the LP basis — the solver minimised an objective that was partly mis-scaled.
The reduced costs extracted from the mis-scaled LP were wrong.

**Post-fix:** The LP is now dimensionally consistent.  The formula
`rc_phys = LP_rc × scale_obj / col_scale` correctly inverts the LP-space
transformation.  **LMPs derived from `get_col_cost()` are now correct.**

### 7.2 Row duals (get_row_dual)

`dual_phys[i] = LP_dual[i] × scale_objective / row_scale[i]`.

For structural constraint rows (KCL, KVL, reservoir balance, etc.), `row_scale`
is the equilibration divisor and `scale_objective` is the global factor.  These
are unchanged by the fix — the fix only touches cut rows.

For cut rows added in the cut phase: `row_scale[cut] = row.scale × max_abs`.
`dual_phys[cut] = LP_dual[cut] × scale_obj / (row.scale × max_abs)`.

Post-fix this correctly recovers the physical dual of the cut constraint, which
is the SDDP "value-function subgradient" at that trial point.  **Water values
derived from cut row duals are now correctly scaled.**

### 7.3 No downstream changes required

- `output_context` and report post-processing read `get_col_cost()` and
  `get_row_dual()` through the same ScaledView accessors — no change needed.
- The `get_obj_value_physical()` accessor (`= get_obj_value() × scale_objective`)
  is unaffected.
- `propagate_trial_values` reads `col_sol_physical()` not cut row bounds — unaffected.

**No downstream output layer changes are required by this fix.**

---

## 8. Section 8 — Verification Plan

### 8.1 Log signals

After re-running juan/gtopt_iplp with the fix:

1. **Coefficient range log (`|coeff| [...], ratio=...`):** The cut-row entries
   should now be `O(1e-3)` (for `scale_objective=1000`, a coefficient of 1.0
   physical becomes `1e-3` LP), not `O(1e+2)` pre-fix.

2. **Kappa histogram:** The `1e10-1e11` and `>=1e11` bins should drop to zero
   or near-zero by SDDP iteration 2.  Target: all solves in `stable` (κ < 1e7)
   or `suspect` (κ < 1e10) band.

3. **LMAULE coefficient growth:** Each backward phase should show the same
   LMAULE coefficient (within 1% solver tolerance), not growing by 1000×.

4. **Convergence:** SDDP should converge (UB - LB)/UB < 1e-3 within 15 iterations
   rather than stalling/exploding.

### 8.2 Test confirmation

Run:
```bash
./build/test/gtoptTests -tc="LinearInterface add_row cut scaling*"
```

All four existing SUBCASES should pass.

After adding Rec T1 (equilibration round-trip):
```bash
./build/test/gtoptTests -tc="LinearInterface add_row cut scaling*"
```
should include the new SUBCASE passing.

### 8.3 Comparison protocol

Reproduce the compounding: run juan/gtopt_iplp with `scale_objective=1` (no
scaling) and `scale_objective=1000`.  The LMAULE coefficient in the cut log
should be identical (within 1%) across all backward phases for both settings.

### 8.4 Smallest reproduction case

Build a 2-phase toy: `min x` with `x ∈ [0, 10]` as the state variable and a
cut `α ≥ 5 + 1·(x − 5)`.  With `scale_objective=1000` and the fix, the cut's
stored LP RHS should be `5/1000 = 0.005`, not `5`.  Verify via:

```cpp
REQUIRE(li.get_row_low_raw()[cut_idx] == doctest::Approx(5.0 / 1000.0));
```

This is exactly SUBCASE "scale_objective>1" in the new test file — already
present.

---

## 9. Verdict

**LP NUMERICS VERDICT: MAJOR → CLEAN (post-fix)**

Pre-fix: `ratio ≥ 1e8` (cut coefficients grew to 1e+11 vs O(1) structural
coefficients), kappa ≥ 1e+41 → SEVERE.

Post-fix: the coefficient compounding is broken.  With `scale_objective=1000`,
cut elements are `O(1e-3)` matching the OBJ row.  Structural rows are unchanged.
Assuming the rest of the model (kirchhoff, reservoir balance, demand) are within
the normal Gurobi-guideline range `[1e-3, 1e6]`, the global ratio should be
≤ `1e9` after the fix.  Whether it lands below `1e7` (CLEAN) or in `[1e7, 1e8]`
(MAJOR) depends on model-specific kirchhoff susceptances and reservoir balance
coefficients — those are out of scope for this fix.

For the cut compounding specifically: **CLEAN**.

---

## Appendix: Dimensional Algebra Reference

| Quantity | Formula | Location |
|---|---|---|
| LP constraint coefficient | `phys_a_ij × col_scale_j / row_scale_i` | `linear_problem.cpp:369` |
| LP objective coefficient | `cost_phys_j × col_scale_j / scale_obj` | `linear_problem.cpp:426+727` |
| LP cut element (post-fix) | `phys_a_ij × col_scale_j / scale_obj` | `linear_interface.cpp:986+1008` |
| LP cut RHS (post-fix) | `phys_RHS / scale_obj` | `linear_interface.cpp:1011-1013` |
| Physical solution | `LP_x_j × col_scale_j` | `get_col_sol()` Op::multiply |
| Physical reduced cost | `LP_rc_j × scale_obj / col_scale_j` | `get_col_cost()` Op::divide + global |
| Physical row dual | `LP_π_i × scale_obj / row_scale_i` | `get_row_dual()` Op::divide + global |
| Physical objective | `LP_obj × scale_obj` | `get_obj_value_physical()` |
