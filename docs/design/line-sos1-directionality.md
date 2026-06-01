# SOS1-Based Line Directionality Enforcement

**Status:** draft
**Owner:** marcelo
**Last updated:** 2026-05-31
**Tracks:** LP numerics / dual quality; bidirectional-line wash-flow fix

---

## 1. One-sentence summary

Enforce at-most-one-direction flow on designated transmission lines
by registering an SOS1 constraint on `(fp, fn)`, then re-solving as
a pure LP with the losing direction pinned to zero so that KVL and
KCL duals are computed cleanly.

---

## 2. Why now

### 2.1 The wash-flow problem

In `line_losses_mode = none` (the common lossless case) the LP
assembler emits a **single bidirectional column** `fp` per line
(`source/line_losses.cpp:647-688`, `add_none`). `fp` carries the
range `[−tmax_ba, +tmax_ab]` with `+fp` meaning A→B and `−fp`
meaning B→A. There is no separate `fn` column in this mode, so
wash flow is structurally impossible.

Wash flow IS possible in the modes that emit SEPARATE `fp` and `fn`
columns, each non-negative:

| Mode | fp col | fn col |
|---|---|---|
| `none` | bidirectional `[-tmax_ba, tmax_ab]` | absent |
| `linear` | `[0, tmax_ab]` if `tmax_ab > 0` | `[0, tmax_ba]` if `tmax_ba > 0` |
| `piecewise` | `[0, tmax_ab]` | `[0, tmax_ba]` |
| `bidirectional` | `[0, tmax_ab]` | `[0, tmax_ba]` |
| `piecewise_direct` | absent (seg cols only) | absent |

(`source/line_losses.cpp:718-770` for `linear`,
`source/line_losses.cpp:841-992` for `piecewise`.)

When BOTH `fp` and `fn` are non-zero at the same LP optimal basis
the net flow is `fp − fn`, which is physically correct at every bus
balance row. However:

- **Complementary slackness is violated in spirit.** The KVL row
  (Kirchhoff voltage law, `theta_a − theta_b − x · f = 0`) has
  `f = fp − fn`. When `fp = fn = q > 0` the net flow `f = 0` but
  both `fp` and `fn` are at interior points. If `tcost > 0` the
  objective penalises both and the LP naturally avoids wash. When
  `tcost = 0` (the common default) the LP is indifferent to `q`,
  and any `q ∈ [0, min(tmax_ab, tmax_ba)]` is optimal. The solver
  picks an arbitrary basis, often choosing one where both
  directions are non-zero.

- **Dual quality degrades.** A degenerate basis with `fp, fn` both
  basic and at interior points yields duals on the KVL and KCL
  rows that depend on which degenerate basis the simplex selected.
  For SDDP, these duals feed the backward-pass cut coefficients;
  noise in the duals propagates into noisy cuts, slowing convergence
  (see LP-numerics-expert system prompt §"Why conditioning matters").

- **Affected configurations in practice:**
  - Any mode other than `none` where `tmax_ab > 0` AND
    `tmax_ba > 0` AND `tcost = 0`.
  - Lines in loops or meshed networks where the LP has a
    continuum of optimal net flows — e.g. a line in a cycle where
    two paths have identical susceptance; the LP can push arbitrary
    equal-and-opposite flows around the cycle without cost.
  - Renewable-rich cases where LMPs are at zero for extended
    periods: all line-cost duals vanish, wash-flow degeneracy is
    pervasive.

The user characterises these as lines "very hard to keep
bidirectional." The SOS1 constraint `(fp, fn) ∈ SOS1` enforces
`fp · fn = 0` at any MIP-feasible solution, eliminating wash
structurally while adding no binary variable.

---

## 3. Prior art

| Tool / Reference | Mechanism | Analogy to gtopt / caveats |
|---|---|---|
| **Fisher & O'Neill (2008)** "Optimal Transmission Switching" | Binary `z_ij` selects line in/out of service; enforces flow = 0 or unconstrained. | Different goal (topology switching) but same LP structure: binary multiplied on `tmax` bound. Our SOS1 on `(fp, fn)` is a softer variant — the line stays in service but chooses a direction. |
| **Hedman et al. (2008)** "Optimal Transmission Switching with Contingency Analysis" | Same binary-indicator structure, studied for N-1 security. | Background reference for MIP cost of adding transmission binaries; confirms the combinatorial overhead is manageable for moderate network sizes (≤ 500 lines). |
| **PLEXOS `Line Hurdle Rate`** | Asymmetric per-direction cost; drives the LP to prefer one direction. | A soft fix: it works when the cost differential is large enough to overcome degeneracy. It does NOT prevent wash in zero-cost loops. SOS1 is a hard fix. |
| **PLEXOS `Line Flow Limit` directional enforcement** | Hard per-direction thermal cap; PLEXOS enforces `flowAB ≤ tmax_AB` and `flowBA ≤ tmax_BA` via separate UC-style period constraints. | Equivalent to what gtopt already does via `fp`/`fn` column upper bounds. Wash still possible. |
| **PyPSA `committable` lines** | Binary `committed` on a line toggles the KVL constraint on/off. | Transmission switching, not directionality. Not analogous. |
| **PowerSimulations.jl line-direction binaries** | No native feature; users add a binary `z` and reformulate as a MISOCP for AC OPF. | DC OPF context differs; the clean SOS1 approach has no equivalent in the PowerSim API. |
| **SOS1 LP background — Beale & Tomlin (1970)** | Original SOS1 definition: at most one variable in the set is non-zero. No binary variable added; solver branches on the set structure. | Directly applicable. CPLEX, HiGHS, CBC all support SOS1 natively via their respective APIs. |

**What holds:** SOS1 on `(fp, fn)` is mathematically equivalent to
the tight Big-M reformulation (b) below when `z ∈ {0,1}` but avoids
adding a binary column to the problem. Native SOS1 branching in
CPLEX / HiGHS is empirically faster than Big-M for small sets (size 2).

**What breaks:** CLP has no SOS1 API; the fallback is hard error or
explicit Big-M binary.

---

## 4. Formulation

### 4.1 Candidate reformulations

**(a) SOS1 on `(fp, fn)` — RECOMMENDED**

Register `{fp_col, fn_col}` as an SOS1 set of type 1. At any
MIP-feasible solution, at most one of `fp_col` and `fn_col` is
non-zero. No extra variable added. Solver branches on the set.

Formulation impact:
```
fp ≥ 0,  fp ≤ tmax_ab      (existing column bounds)
fn ≥ 0,  fn ≤ tmax_ba      (existing column bounds)
(fp, fn) ∈ SOS1             (new SOS1 constraint — no extra row, no extra col)
```

Net flow: `f = fp − fn` (same as today, interpreted identically in
bus-balance and KVL rows).

**(b) Big-M binary indicator — FALLBACK**

Introduce `z ∈ {0,1}` and add two linking rows:
```
fp ≤ tmax_ab · z
fn ≤ tmax_ba · (1 − z)
```

This is a tight Big-M formulation because `z` is binary and the
bounds `tmax_ab`, `tmax_ba` are the physical flow ceilings, NOT an
artificial large constant. No numerical ill-conditioning from Big-M
coefficient range — the coefficients are `tmax_ab` and `tmax_ba`,
which are already in the LP as column upper bounds.

Downside: adds one binary variable per (line, block), increasing
MIP size. Use only when the backend lacks SOS1 support.

**(c) Lifted complementarity / nonlinear — OUT OF SCOPE**

`fp · fn = 0` as a nonlinear constraint, or lifted to a
second-order cone. Not tractable in the LP/MIP framework without
additional approximation. Mentioned for completeness; not pursued.

### 4.2 Why SOS1 is preferred over Big-M

- No extra variable: SOS1 adds zero columns to the LP, keeping
  the problem size identical for LP solves.
- Native branching: CPLEX (`CPXaddsos`) and HiGHS (`Highs_passSos`)
  implement specialised SOS1 branching that is empirically faster
  than Big-M branch-and-bound on size-2 sets (SOS1 set cardinality
  is 2 per line-block).
- Dual quality post-fix: after fixing the losing direction to 0 and
  re-solving as LP, the SOS1 constraint is gone from the re-solve;
  the LP is tight and the duals are exact. With the Big-M approach
  the `z = 0` or `z = 1` fix leaves the linking rows in the LP
  with a right-hand side of 0 or `tmax`; those rows have non-zero
  duals that must be accounted for separately.

---

## 5. The fix-and-resolve dual-computation pattern

This section documents the CRITICAL user requirement: after the
MIP solve, re-solve as LP with the losing direction suppressed.

### 5.1 The underlying problem

After a SOS1 MIP solve, the returned duals belong to the MIP LP
relaxation at the root node, NOT to the final integer solution.
Row prices at the final node may be unavailable or meaningless
(CPLEX returns duals at the LP root relaxation, not at the
integer node). To extract clean KVL and KCL duals for LMP
computation and SDDP cut construction, we must perform a
separate LP solve with the winning direction enforced.

### 5.2 The fix-and-resolve steps

For each SOS1-enrolled line, per (block):

1. **Read primal.** After the MIP solve:
   ```
   fp_val = li.get_col_sol()[fp_col]
   fn_val = li.get_col_sol()[fn_col]
   ```

2. **Determine winning direction** using a relative tolerance:
   ```
   fmax = max(tmax_ab, tmax_ba)
   tol  = kSos1PrimalTol * max(fmax, 1.0)
   if fp_val > tol and fn_val <= tol:
       winner = AB     # fix fn upper-bound to 0
   elif fn_val > tol and fp_val <= tol:
       winner = BA     # fix fp upper-bound to 0
   else:
       winner = ZERO   # both near-zero: tie policy (see §5.3)
   ```
   Recommended `kSos1PrimalTol = 1e-6` (matches CPLEX EpRHS default).

3. **Suppress losing direction.** Call `set_col_upper` on the
   losing direction's column:
   ```
   if winner == AB:
       li.set_col_upper_raw(fn_col, 0.0)
   elif winner == BA:
       li.set_col_upper_raw(fp_col, 0.0)
   # ZERO case: leave both as-is (see §5.3)
   ```
   This is a per-solve mutation — the LP snapshot for the next
   SDDP iteration is NOT modified (see §9 edge case on
   `low_memory`).

4. **Re-solve as LP.** Drop the SOS1 (it was enforced in step 1;
   the bound fix encodes the decision for the LP re-solve):
   ```
   li.resolve()    # LP-only, no MIP, no SOS1
   ```
   The LP is small (one less degree of freedom), and the basis
   from the forward-pass LP is a warm start.

   NOTE: gtopt does NOT use LP warm-start today
   (`feedback_no_warm_start`). The re-solve is therefore a cold
   LP solve. The suppressed upper-bound ensures the LP is
   equivalent to the solved MIP at the selected integer point.

5. **Extract duals.** After the re-solve, `li.get_row_dual()`
   returns clean duals on KVL and KCL rows. These feed the
   standard `output_context.cpp:482-484` unfolding path
   (physical-dual recovery via `block_icost_factors`) and, in
   SDDP, the Benders cut construction in
   `source/benders_cut.cpp`.

6. **Restore the upper bound** before the next LP build call:
   ```
   if winner == AB:
       li.set_col_upper_raw(fn_col, block_tmax_ba)
   elif winner == BA:
       li.set_col_upper_raw(fp_col, block_tmax_ab)
   ```
   The restore must happen before any `reconstruct()` call in
   `low_memory` mode (see §9).

### 5.3 Tie policy (both directions near-zero)

When `fp_val ≤ tol` AND `fn_val ≤ tol` (the line carries
essentially zero flow), the LP already has a degenerate optimal
with zero duals on any flow-specific row. The recommended policy:

- **Do NOT suppress either direction.** Leave both columns at
  their original upper bounds.
- **Re-solve the LP anyway** (the re-solve is still needed to
  get valid duals from a clean LP, but no bound mutation is
  performed).
- Log at DEBUG level: `"SOS1 tie on line uid={}: fp={}, fn={},
  leaving both directions open"`.

The resulting duals on KVL are physically correct: when the
line carries zero flow, the KVL row dual is the incremental
cost of enforcing the flow constraint, which is zero by
complementary slackness.

### 5.4 Interaction with the existing write-out pipeline

The fix-and-resolve LP returns the same `fp_col` / `fn_col`
solution values as the MIP (one is at most numerically zero),
so `LineLP::add_to_output` at `source/line_lp.cpp:549-550` reads
consistent primal values. The KVL row duals and KCL bus-balance
duals are read via the standard OutputContext chain
(`out.add_row_dual`), unchanged.

No new output columns are needed. The fix-and-resolve is
transparent to the output layer: it is a solve-time operation
only, not a data-model change.

### 5.5 Loss-formulation compatibility — what SOS1 does NOT fix

**SOS1 fixes wash flow. It does not, by itself, guarantee that the
loss reported by the LP equals the physical loss under the chosen
direction.** Whether the formulation is leak-free post-SOS1 depends on
how segments and tangents aggregate fp/fn — different `line_losses_mode`
values have different exposure to residual loss inflation.

This subsection enumerates the failure modes per loss mode, what holds
automatically post-SOS1, and what needs an additional check or
restriction.

#### 5.5.1 The (`fp + fn`) aggregator and why SOS1 makes it physical

The existing piecewise / bidirectional formulations stamp segments and
tangents against `(fp + fn)`, not `|fp − fn|`. Concretely:

- **Piecewise link row** (`source/line_losses.cpp:918-933`):
  `fp + fn − Σ seg_k = 0`. The segment sum equals the SUM of
  directional flows, not the magnitude of net flow.
- **Bidirectional tangent rows** (`source/line_losses.cpp:514-532`):
  `loss · s − slope_coef · s · (fp + fn) ≥ −k_loss · t_k² · s`
  (where `s = loss_row_scale`, `slope_coef = 2 · k_loss · t_k`).
  Each tangent stamps fp and fn with the SAME coefficient — the
  effective tangent variable is `(fp + fn)`.

Pre-SOS1, with wash flow `fp = fn = q > 0`:

- Aggregator: `Σ seg_k = 2q`, so segments fill to `2q` total.
- Tangent: `loss ≥ 2 · k_loss · t_k · 2q − k_loss · t_k²` — a tangent
  to `k_loss · (2q)²` at `f = t_k`, NOT to the physical `k_loss · 0²`.

So the LP's lower bound on `loss` is the convex envelope of
`k_loss · (fp + fn)²`, not `k_loss · (fp − fn)²`. Under wash this
INFLATES the lower bound. The LP minimises `loss`, so it actively
avoids wash when there is any positive marginal penalty on `loss`.
But when the loss column carries zero marginal cost and the LP is
indifferent, wash CAN occur and the reported loss is inflated.

**Post-SOS1**, `fp · fn = 0`, so `fp + fn = max(fp, fn) = |fp − fn|`.
The aggregator and the physical magnitude coincide exactly. The same
piecewise / bidirectional formulation that was loss-inflating under
wash becomes physically correct without any change to the segment or
tangent rows. **This is the main reason SOS1 is the right Phase 1
fix: it makes the existing aggregator algebra honest.**

#### 5.5.2 Per-mode compatibility matrix

| Mode | Pre-SOS1 wash risk | Post-SOS1 loss correctness | Action needed |
|---|---|---|---|
| `none` | Structurally impossible (single signed `fp` col, no `fn`). | n/a — SOS1 not applicable to this mode. | If the user wants wash-free loss accounting in `none` mode, they must first switch to `linear` or `piecewise`. SOS1 cannot fix a missing `fn` column. |
| `linear` | LP can split flow into fp + fn without bound coupling. Loss is `λ · fp + λ · fn` per `apply_linear_allocation` (`source/line_losses.cpp:729-757`); under wash this DOUBLE-COUNTS loss. | After SOS1, exactly one of fp/fn is non-zero; the per-direction linear-loss term equals the physical `λ · |f|`. Clean. | No additional check needed. |
| `piecewise` | Wash inflates the segment-sum aggregator (§5.5.1). Loss tangents stamp `(fp + fn)` linearly. | After SOS1, `fp + fn = |fp − fn|`. Aggregator and tangents collapse to the physical magnitude. Clean. | No additional check needed. |
| `bidirectional` | Same as `piecewise` plus per-direction segments. The tangent rows again use `(fp + fn)` for the linear part. Same wash exposure as `piecewise`. | After SOS1, collapses to physical magnitude. Per-direction segments under SOS1 reduce to the active direction only. Clean. | No additional check needed. |
| `piecewise_direct` | NO `fp` / `fn` aggregator columns — segments carry flow directly via `flowp_seg_k` / `flown_seg_k`. SOS1 on `(fp, fn)` is structurally inapplicable. | **Not addressed by Phase 1.** Wash on this mode would require SOS1 on the SET of direction-A segments vs the SET of direction-B segments (an SOS1 on K + K elements, combinatorially expensive). | The Phase 1 resolver returns `false` for `piecewise_direct` and emits a WARNING if the user requests SOS1 on a `piecewise_direct` line. A future Phase 4 may add per-segment SOS1 if real cases demand it. |
| `adaptive` | Resolves to `piecewise` or `bidirectional` per the `has_expansion` switch (`source/line_losses.cpp:50-65`). | Same as the resolved target mode. | None — adaptive resolves before SOS1 dispatch. |
| `dynamic` | Currently a placeholder that demotes to `piecewise` (`line_losses.cpp:60`). | Same as `piecewise`. | None. |

#### 5.5.3 Failure modes that survive SOS1

These are the cases where, even with SOS1 enforced exactly, the
LP-reported loss CAN diverge from the physical loss for the chosen
direction's `|f|`. They are independent of wash and require their own
mitigations:

1. **Loss-allocation asymmetry between buses**
   (`apply_linear_allocation`, `source/line_losses.cpp:729`).
   The lossfactor is split as "sender loses 1, receiver gains
   `1 − λ`". When sender LMP ≪ receiver LMP, the LP CAN have an
   incentive to INFLATE flow on the active direction to gain more
   `(1 − λ) · fp` injection at the receiver. The total loss reported
   is physical, but the DISPATCH is distorted: the LP wants more loss
   because it values the receiver-side injection more than the
   sender-side cost. Mitigation: this is a market-model issue, not
   a loss-formulation bug. Document as a known trade-off; SOS1 does
   not change the incentive structure.

2. **Numerical near-zero on the SOS1-loser side.**
   The fix-and-resolve step zeroes the loser's upper bound at a
   tolerance of `kSos1PrimalTol = 1e-6 · max(fmax, 1.0)` (§5.2).
   Inside the tolerance the LP may have `fn = 5e-7` while `fp = 100`;
   the aggregator sees `fp + fn = 100.0000005` and the loss tangent
   bound is inflated by `~1e-6 · k_loss · t_k` — negligible.
   Mitigation: the existing tolerance is sufficient; document the
   bound and assert in tests.

3. **Backward-pass relaxation in SDDP.**
   The backward LP removes SOS1 sets via `clear_sos1()` (§9 edge
   case). The resulting LP relaxation CAN have wash — and therefore
   inflated loss in the (fp+fn) aggregator. This is mathematically
   correct: the relaxation gives a valid LOWER BOUND on the MIP
   value function (`min loss` is smaller when wash is allowed,
   because… wait, wash INFLATES the loss bound; so it INCREASES
   `min loss`, which still produces a valid lower bound on the
   forward MIP because the forward MIP has even fewer wash-feasible
   solutions). Cuts derived from the backward LP are LP-valid and
   therefore LP-MIP-valid by standard SDDP theory. Document
   explicitly that backward-pass duals reflect the relaxed
   (potentially wash-inflated) LP, not the forward-pass post-SOS1
   reality.

4. **Non-monotone PWL specification by the user.**
   If the user supplies a piecewise loss curve with a non-convex
   segment (decreasing marginal loss), the LP exploits the
   non-convexity regardless of SOS1. Mitigation: validate
   `slope_coef > 0` at PWL construction time; reject non-convex
   inputs with a hard error. This pre-dates SOS1 and is independent.

5. **Loss + reserves coupling.**
   When line losses contribute to system-wide reserve headroom
   constraints (currently NOT modelled in gtopt, but PLEXOS does
   this), inflated loss on a wash line would change the reserve
   budget. Out of scope for gtopt today; flagged for future
   awareness.

#### 5.5.4 Recommended validation step

Phase 2 should add a **post-`fix_and_resolve` loss-consistency check**
that runs once per line per (scenario, stage, block):

```text
fp_phys = fp_col.value           # ≥ 0
fn_phys = fn_col.value           # ≥ 0
loss_lp = loss_col.value         # ≥ 0
mag     = abs(fp_phys - fn_phys) # physical magnitude
loss_phys_lb = k_loss * mag**2 - small_tol_buffer

# After SOS1 + fix_and_resolve, exactly one of {fp, fn} is at most
# kSos1PrimalTol. The LP-reported loss should agree with the physical
# quadratic loss EVALUATED AT THE CHOSEN DIRECTION'S MAGNITUDE.
if loss_lp > loss_phys_ub + tol:
    # Loss inflation detected — should not happen post-SOS1 for
    # any of {linear, piecewise, bidirectional}. Indicates a bug
    # in the loss formulation or a numerical issue.
    log_warn("line {}: SOS1-resolved loss inflation: "
             "loss_lp={}, physical_ub={}".format(
                line_uid, loss_lp, loss_phys_ub))
```

`loss_phys_ub` is the upper convex envelope of `k_loss · f²` at the
resolved magnitude, computed from the same segment / tangent geometry
the LP uses. The check is informational only — failures indicate a
formulation bug (not a configuration error), so the recommended action
is to log and continue, not to throw.

This check is folded into the existing post-solve invariants in
`source/line_losses.cpp` rather than the user-facing output. It costs
~O(1) per line per block, negligible overhead.

#### 5.5.5 Symptoms checklist for "loss inflation after SOS1"

When operating a model with SOS1 enabled and seeing system-level loss
totals that exceed expectations, the diagnostic chain is:

1. Confirm `fix_and_resolve` actually ran (check the DEBUG log for
   the winner-direction line per affected line).
2. Confirm `fp_col` AND `fn_col` are not both > tolerance on any
   SOS1-enrolled line (the §5.5.4 invariant).
3. If both are within tolerance: the loss inflation comes from the
   loss-allocation incentive (§5.5.3 #1), not from SOS1. Check
   sender vs receiver LMP gradient on the affected lines.
4. If only one is non-zero but reported loss exceeds physical: there
   is a formulation bug — file an issue with the line uid, mode,
   resistance, and the offending (scenario, stage, block).
5. If the inflation appears only in SDDP backward-pass output:
   expected behaviour (§5.5.3 #3); the backward LP is the relaxed
   problem and is consulted only for cut construction, not for
   dispatch reporting.

---

## 6. Configuration surface

This section mirrors commitment-layout §6. The CLI is restricted
to global-only settings (per §6.5 of that doc).

### 6.1 Per-line field

Add `use_sos1` to `include/gtopt/line.hpp`:

```cpp
OptBool use_sos1 {};  ///< Enforce SOS1 directionality on (fp, fn).
                      ///< When set, the LP/MIP adds an SOS1 constraint
                      ///< that prevents simultaneous A→B and B→A flow.
                      ///< After the MIP solve, a fix-and-resolve LP step
                      ///< extracts clean KVL/KCL duals.
                      ///< Requires a backend that supports SOS1
                      ///< (CPLEX, HiGHS, CBC). Hard error for CLP.
                      ///< Overrides the global `use_sos1_lines` option.
```

### 6.2 Global options

Add to `PlanningOptions` (or the `model_options` sub-struct,
wherever `use_kirchhoff` and `line_losses_mode` live):

```cpp
bool use_sos1_lines {false};      // Global enable for all lines
OptReal use_sos1_lines_threshold; // Skip SOS1 on lines where
                                  // min(tmax_ab, tmax_ba) < threshold MW.
                                  // Prevents SOS1 on degenerate
                                  // single-direction lines (see §9).
```

`use_sos1_lines_threshold` defaults to `nullopt` (no threshold;
apply to all lines where SOS1 is enabled). A value of `0.0` is
equivalent to `nullopt`.

### 6.3 Precedence stack

Five levels, highest to lowest:

| Level | Selector | Mechanism |
|---|---|---|
| 4 — per-line explicit | `Line.use_sos1 = true/false` | Overrides everything. Explicit `false` disables SOS1 even if the global is enabled. |
| 3 — per-line implicit | (currently reserved; no `use_sos1_mode` field exists) | Placeholder for future per-line thresholds. |
| 2 — global + threshold | `use_sos1_lines = true` AND `min(tmax_ab, tmax_ba) >= threshold` | SOS1 enabled globally; per-line threshold gates degenerate lines. |
| 1 — global default | `use_sos1_lines = true` (no threshold) | Applies to all bidirectional lines with BOTH fp and fn columns. |
| 0 — built-in default | `false` | SOS1 disabled; LP is unchanged. |

Resolution function signature:

```cpp
// include/gtopt/line_sos1.hpp (new)
namespace gtopt {

[[nodiscard]] bool resolve_use_sos1(
    const Line& line,
    const PlanningOptionsLP& options,
    double tmax_ab_val,
    double tmax_ba_val) noexcept;

}
```

The resolver mirrors `line_losses::resolve_mode`
(`source/line_losses.cpp:70-100`): check per-line field first,
then global, then threshold.

### 6.4 CLI flags

Global-only, mirroring commitment-layout §6.5:

```
--use-sos1-lines true|false       # global enable/disable
--sos1-lines-threshold MW         # min(tmax_ab, tmax_ba) threshold in MW
```

CLI does NOT expose per-line overrides. Per-line `use_sos1` is
set in the JSON input dataset (`Line` array) or CSV schedule.

### 6.5 JSON example

```yaml
model_options:
  use_sos1_lines: true
  use_sos1_lines_threshold: 10.0    # skip lines < 10 MW bidirectional cap

# Per-line override in the Line array:
line_array:
  - uid: 42
    name: "loop_line"
    use_sos1: true     # force SOS1 regardless of threshold
  - uid: 99
    name: "export_only"
    tmax_ba: 0
    use_sos1: false    # explicit disable (resolver would skip anyway via §9)
```

---

## 7. Solver backend requirements

### 7.1 New virtual method on `SolverBackend`

Add to `include/gtopt/solver_backend.hpp` (bump `k_solver_abi_version`
from 8 to 9):

```cpp
// ---- SOS1 support ----

/// Returns true if this backend natively supports SOS1 constraints
/// via add_sos1(). When false, callers must either fall back to the
/// Big-M binary reformulation or report a hard error.
[[nodiscard]] virtual bool supports_sos1() const noexcept { return false; }

/// Add a Special Ordered Set of type 1 (SOS1): at most one variable
/// in `col_indices` may be non-zero at a MIP-feasible solution.
///
/// @param col_indices  LP column indices forming the SOS1 set.
///                     Must contain exactly 2 columns for the
///                     directionality use case (fp_col, fn_col).
/// @param weights      Ordering weights (size == col_indices.size()).
///                     For (fp, fn) use weights {1.0, 2.0} — the
///                     weight ordering signals A→B before B→A to
///                     the branching heuristic.
/// @param name         Optional set name for debug output.
///
/// Pre-condition: backend must have supports_sos1() == true.
/// Calling on a backend that returns false is undefined behaviour;
/// use the supports_sos1() gate.
virtual void add_sos1(
    std::span<const int>    col_indices,
    std::span<const double> weights,
    std::string_view        name = {})
{
  // Default: assert not reached.  Derived classes that don't
  // override must return false from supports_sos1().
  (void)col_indices;
  (void)weights;
  (void)name;
  // Raise a hard error at runtime rather than silently succeeding.
  throw std::logic_error(
      "add_sos1() called on a backend that does not support SOS1; "
      "check supports_sos1() before calling");
}
```

The default implementation of `supports_sos1()` returns `false`;
only backends that override it to `true` AND override `add_sos1()`
are safe to call.

### 7.2 Per-backend implementation plan

**CPLEX** (`plugins/cplex/cplex_solver_backend.cpp`):

```
supports_sos1() → true

add_sos1(cols, weights, name):
  // Single-set call; CPLEX SOS1 type = 1.
  // sostype = 1, numsos = 1, numsosnz = cols.size()
  int sostype[] = {1};
  int sosbeg[]  = {0, (int)cols.size()};
  const char* names[] = {name.data()};
  CPXaddsos(env_, lp_, 1, cols.size(),
            sostype, sosbeg,
            cols.data(), weights.data(), names);
```

Reference: CPLEX Callable Library Reference, `CPXaddsos`.
ASSUMPTION: the `env_` / `lp_` handles are accessible as
private members in the CPLEX plugin — verify in
`plugins/cplex/cplex_solver_backend.cpp`.

**HiGHS** (`plugins/highs/highs_solver_backend.cpp`):

```
supports_sos1() → true

add_sos1(cols, weights, name):
  // HiGHS SOS API: Highs_passSos (or addSos in some versions).
  // SOS type 1, one set.
  int sos_type = 1;
  int num_sets = 1;
  int num_set_entries = cols.size();
  int starts[] = {0};
  Highs_addSos(highs_, num_sets, num_set_entries,
               &sos_type, starts,
               cols.data(), weights.data());
```

ASSUMPTION: the HiGHS API function is `Highs_addSos` (C API
name as of HiGHS 1.5). Verify against the installed HiGHS
header `Highs_c_api.h` — the function may be named
`Highs_passSos1` in older releases. **This is the primary API
assumption that needs verification before implementation.**

**CBC** (`plugins/cbc/` or via OSI):

```
supports_sos1() → true

add_sos1(cols, weights, name):
  // OsiSolverInterface::addSOS via OsiCbcSolverInterface.
  // OsiSOS constructor: type=1, priority=1.
  std::vector<int> idx(cols.begin(), cols.end());
  std::vector<double> wts(weights.begin(), weights.end());
  OsiSOS* sos_set = new OsiSOS(nullptr, 1, idx.size(),
                                idx.data(), wts.data(), 1);
  dynamic_cast<OsiCbcSolverInterface&>(si_).addSOS(1, &sos_set);
  delete sos_set;
```

ASSUMPTION: the CBC plugin exposes an `OsiSolverInterface&
si_` or equivalent access handle. Verify in the CBC plugin
source.

**CLP** (`plugins/clp/` or embedded in CBC plugin):

```
supports_sos1() → false
```

CLP is a pure LP solver with no branch-and-bound engine. It
has no SOS1 branching capability. Phase-1 recommendation:
**hard error** when `resolve_use_sos1()` returns `true` on a
CLP backend. Emit:

```
FATAL: line uid=N: use_sos1=true but backend 'clp' has no
SOS1 support. Switch to 'cplex', 'highs', or 'cbc'.
```

Do NOT silently fall back to Big-M at Phase 1 — the user
cannot detect the fallback unless it is explicit. Phase 2 may
add an opt-in `sos1_fallback_bigm: true` option that enables
the binary-indicator formulation for CLP.

### 7.3 ABI version bump

`k_solver_abi_version` must be incremented from 8 to 9 in
`include/gtopt/solver_backend.hpp:37` when `add_sos1` and
`supports_sos1` are added. All four plugin shared libraries must
be rebuilt and their internal version checks updated.

---

## 8. Coverage — every site this touches

| Site | Change |
|---|---|
| `include/gtopt/line.hpp` | Add `OptBool use_sos1 {}` field to `Line` struct (after `loss_segments`). |
| `include/gtopt/line_sos1.hpp` (new) | `resolve_use_sos1()` resolver function; `kSos1PrimalTol` constant; `fix_sos1_bounds()` helper. |
| `source/line_sos1.cpp` (new) | Implementation of resolver + bound-fix helpers. |
| `include/gtopt/solver_backend.hpp` | `supports_sos1()` + `add_sos1()` virtuals; ABI version 8 → 9. |
| `include/gtopt/linear_interface.hpp` | `add_sos1(cols, weights, name)` wrapper forwarding to backend; `has_sos1_support()` query. |
| `source/linear_interface.cpp` | Implement the wrapper (thin delegation, no extra logic). |
| `plugins/cplex/cplex_solver_backend.cpp` | Override `supports_sos1()` + `add_sos1()` using `CPXaddsos`. |
| `plugins/highs/highs_solver_backend.cpp` | Override using `Highs_addSos` (API name TBD — see §7.2). |
| `plugins/cbc/cbc_solver_backend.cpp` | Override using `OsiSOS` + `OsiCbcSolverInterface::addSOS`. |
| `source/line_lp.cpp :: add_to_lp` | After the block loop: for each block where `resolve_use_sos1()` is true AND both `fp_col` and `fn_col` are present, call `lp.add_sos1({fp_col, fn_col}, {1.0, 2.0}, label)`. Store SOS1 col pairs in a new `m_sos1_pairs_` holder for the fix-and-resolve step. |
| `source/line_lp.cpp :: fix_and_resolve` (new) | Called post-MIP-solve. Reads primal, determines winner per block, mutates upper bounds, invokes `li.resolve()`, restores bounds. |
| `source/system_lp.cpp` or forward-pass driver | Call `fix_and_resolve` on every SOS1-enrolled line after the forward-pass MIP solve, before `add_to_output`. |
| `source/line_lp.cpp :: add_to_output` | No change required. The fix-and-resolve ensures `fp_col` / `fn_col` primal values are already clean when this runs. |
| `scripts/igtopt/_options_meta.py` | Add `use_sos1_lines` and `use_sos1_lines_threshold` to the option sync table (mirrors `project_cpp_python_option_sync` pattern). |

---

## 9. Edge cases

| Case | Resolution |
|---|---|
| **Single-direction line** (`tmax_ba = 0`): `fn_col` is absent (see `add_linear` at `source/line_losses.cpp:745` which gates on `block_tmax_ba > 0`). | `resolve_use_sos1` returns false regardless of the per-line or global setting. No SOS1 emitted. The LP already enforces directionality via the missing `fn` column. |
| **`line_losses_mode = none`**: `add_none` at `source/line_losses.cpp:647` emits only a single bidirectional column `fp` with range `[−tmax_ba, tmax_ab]`. There is no separate `fn`. | SOS1 is not applicable — the single signed column cannot be in an SOS1 pair. `resolve_use_sos1` returns false. If the user requests `use_sos1 = true` on a `none`-mode line, emit a WARNING and skip. The fix for `none`-mode wash is to switch to `linear` mode, which emits separate `fp` / `fn` columns. |
| **Lines with expansion modules**: `tmax_ab` is bounded by `capacity_col` via the capacity row (`source/line_lp.cpp:282`). The SOS1 set is on `(fp_col, fn_col)` with their original upper bounds. The capacity constraint is a separate row. | SOS1 still applies; the capacity row bounds the sum `fp + fn ≤ capacity`, and the SOS1 enforces `fp · fn = 0`. Interaction is clean. |
| **`piecewise_direct` mode**: no aggregator `fp_col` / `fn_col` columns exist. The segments carry flow directly. | SOS1 on per-segment cols is theoretically possible but combinatorially expensive (2K cols per set). Defer; `resolve_use_sos1` returns false for this mode at Phase 1. Emit a WARNING if the user requests SOS1 on a `piecewise_direct` line. |
| **SDDP backward pass**: backward LPs relax all integers via `relax_all_integers()` (`include/gtopt/solver_backend.hpp:274`). SOS1 sets are NOT removed by this call — they are structural SOS objects, not integer column flags. | The backward LP must also REMOVE SOS1 sets before re-solving. Add a `clear_sos1()` virtual alongside `relax_all_integers()`; the backward-pass path calls it. The forward-pass fix-and-resolve does NOT apply in the backward pass (duals from the LP-relaxed backward LP are cut coefficients; the LP relaxation without SOS1 is correct for Benders cuts). |
| **`low_memory` mode (SDDP compress path)**: the LP snapshot at `reconstruct()` restores column bounds to their construction-time values. If `fix_and_resolve` mutated the losing direction's upper bound and `reconstruct()` fires before the restore step, the snapshot reverts to the original (correct) upper bound. | `fix_and_resolve` must restore upper bounds BEFORE `reconstruct()` is called. The natural ordering in the forward pass is: solve MIP → fix-and-resolve → `add_to_output` → next phase. Reconstruct happens at the next LP BUILD call (backward pass). This ordering is safe. Document as an invariant in `fix_and_resolve`. |
| **`chronological = false` stages**: SOS1 is a per-block structural constraint. Non-chronological stages still have `block_array` entries; the SOS1 applies per-block regardless of ordering. | No special handling needed. |
| **Cross-scenario**: the SOS1 constraint is per-scenario-LP; each scene has its own `fp_col` / `fn_col` indices in its own LP. The MIP solve is per-scene. The winning direction may differ across scenes (different hydrology / load). | No aggregation. Each scene's fix-and-resolve operates independently. |
| **`tmax_ab = tmax_ba = 0` line** (disabled line): no `fp` / `fn` columns are emitted. | `resolve_use_sos1` returns false. No SOS1. |

---

## 10. Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| **MIP solve-time blowup** if SOS1 applied to every line | P1 — high if global-enable with no threshold | Per-line opt-in default (`use_sos1_lines = false`); `use_sos1_lines_threshold` gates degenerate lines; the user guide recommends enabling SOS1 only on specific loop-forming lines identified from LP analysis. |
| **LP infeasibility from bound flip** on a numerically dirty primal | P1 — moderate. If `fp = 1e-9` is classified as "active" by a too-tight threshold, `fn` is suppressed unnecessarily | Use `kSos1PrimalTol = 1e-6 × max(fmax, 1.0)` (relative tolerance). Log the decision at DEBUG. The re-solve will fail with INFEASIBLE if the bound is wrong — detect and fallback to the tie policy (restore both bounds, re-solve without suppression). |
| **Dual sign correctness** when both directions round to 0 | P0 — certain to occur on zero-flow lines. | Tie policy: suppress nothing, re-solve with both bounds intact (§5.3). The LP returns zero flow with well-defined KVL dual from complementary slackness. This is correct and requires no special handling. |
| **Plugin ABI break**: `add_sos1` is a new virtual. Old plugins compiled against ABI 8 will crash or silently misbehave if loaded against ABI 9. | P0 — certain without ABI version check. | The existing `k_solver_abi_version` check in `SolverRegistry::load_plugin` rejects incompatible plugins with a clear error. Bump from 8 to 9 as the first change; all four plugins must be rebuilt. |
| **SDDP backward-pass SOS1 removal** not implemented | P1 — will produce wrong backward LP if SOS1 sets are left in | Add `clear_sos1()` virtual before shipping. Default implementation no-op (safe for backends that have no SOS1); concrete implementations iterate the SOS set list and remove. |
| **Cascade / SDDP**: SOS1 in forward pass only. Backward pass relaxes to LP AND clears SOS1. | P0 — must be documented and tested. | The backward LP without SOS1 is a valid lower-bound LP for the Benders cut (LP relaxation is a valid lower bound of the MIP). The cut coefficients are LP-based regardless of integrality in the forward pass — standard SDDP with integer forward. |
| **`supports_sos1()` query not checked before `add_sos1()` call** in `LineLP::add_to_lp` | P1 — would hit the default `throw` in `add_sos1` | Gate: `if (!lp.has_sos1_support()) { /* log warn, skip or bigm */ }` at the call site. |
| **HiGHS API name** `Highs_addSos` vs `Highs_passSos1` | Unknown — must be verified | Mark as API assumption in §7.2; implementation PR must grep the installed HiGHS C API header before writing the override. |

---

## 11. Implementation plan

Each phase is one PR. Phase N+1 depends only on Phase N landing.

### Phase 0 — Backend SOS1 hook (no LP changes)

Add `supports_sos1()` and `add_sos1()` to `SolverBackend` and
`LinearInterface`. Implement in all four plugins (CLP returns
`false`; CPLEX, HiGHS, CBC return `true` with native API calls).
Add `clear_sos1()` virtual alongside. Bump ABI to 9.

Write a trivial 2-variable SOS1 integration test that:
- Builds a 1-line, 2-variable LP (`fp + fn`, minimise cost, bus
  balance).
- Calls `add_sos1({fp_col, fn_col}, {1.0, 2.0})`.
- Solves. Checks that exactly one of `fp`, `fn` is non-zero.

Run on CPLEX, HiGHS, CBC. Verify CLP emits the expected hard
error.

**Risk: LOW.** No LP-shape changes. The hook is a no-op until
Phase 1 calls it.

### Phase 1 — Resolver + `LineLP::add_to_lp` integration

Add `Line.use_sos1` field, `use_sos1_lines` and
`use_sos1_lines_threshold` to options. Implement `resolve_use_sos1`
in `source/line_sos1.cpp`. Wire into `LineLP::add_to_lp` after the
block loop: for each block where the resolver returns `true` and
both `fp_col` and `fn_col` exist, call `lp.add_sos1(...)`.

Store the SOS1 pairs in `m_sos1_pairs_[scenario][stage][block]`
for use in Phase 2.

Update `scripts/igtopt/_options_meta.py`.

Write unit tests:
- Per-line resolver: all 5 precedence levels.
- Wash-flow elimination on a 2-bus 1-line case (see T3 in §12).
- Global threshold gating (T5).

**Risk: LOW-MEDIUM.** LP shape changes only for lines where the
resolver fires. Default is `false`; existing cases are unaffected.
Fingerprint test T6 guards bit-identity.

### Phase 2 — Fix-and-resolve dual extraction

Implement `LineLP::fix_and_resolve` (or a free function in
`source/line_sos1.cpp`). Wire into the forward-pass driver
immediately after the MIP solve and before `add_to_output`. Add
`clear_sos1()` call in the backward-pass LP relaxation path.

Add CLI flags. Validate dual quality (T4 in §12).

**Risk: MEDIUM.** The fix-and-resolve modifies live LP bounds
temporarily. The `low_memory` ordering invariant (§9) must be
verified by the implementation. The `clear_sos1` backward-pass
call must be tested explicitly.

### Phase 3 — Adaptive heuristic (deferred / optional)

From a pilot LP solve (no SOS1), identify lines where
`min(fp_val, fn_val) > threshold * fmax` and automatically set
`use_sos1 = true` for those lines in the next iteration. This
enables a data-driven "which lines are hard to keep bidirectional"
workflow. Deferred to Phase 3 — the per-line and global manual
controls from Phase 1 are sufficient for initial deployment.

---

## 12. Test ladder

| Tier | Scope | Acceptance |
|---|---|---|
| T0 | `SolverBackend::add_sos1` round-trip — CPLEX, HiGHS, CBC | Build a 2-variable LP; add SOS1; solve; verify exactly one variable is non-zero in the MIP solution. |
| T1 | `SolverBackend::add_sos1` error path — CLP | `li.add_sos1()` on a CLP backend throws or returns a clear error; `supports_sos1()` returns false. |
| T2 | `resolve_use_sos1` — all 5 precedence levels | Unit test: Level-4 per-line `use_sos1 = false` overrides Level-1 global `true`; Level-4 `true` overrides Level-1 `false`; threshold gates Level-1 when `min(tmax_ab, tmax_ba) < threshold`. |
| T3 | Wash-flow elimination — 2-bus 1-line case | Construct a 2-bus lossless network (`linear` mode, `tcost = 0`). Without SOS1, the LP returns `fp > 0, fn > 0` (wash). With SOS1, exactly one is non-zero. |
| T4 | Fix-and-resolve dual quality | Same 2-bus 1-line case. After fix-and-resolve, the KVL row dual matches the dual obtained by manually setting `tmax_ba = 0` in the input (forcing the winning direction). Tolerance: `doctest::Approx` with relative 1e-6. |
| T5 | Threshold gating | Global `use_sos1_lines = true`, `threshold = 100 MW`. Line with `min(tmax) = 50 MW` does NOT get SOS1; line with `min(tmax) = 200 MW` does. |
| T6 | Wide LP fingerprint — SOS1 off (default) | `ieee_14b` run with default options: LP fingerprint bit-identical to main branch. SOS1 off must produce zero difference. |
| T7 | `piecewise_direct` mode + `use_sos1 = true` | Emits WARNING, no SOS1 added, LP proceeds unchanged. Test that the warning log line appears. |
| T8 | `line_losses_mode = none` + `use_sos1 = true` | Emits WARNING, no SOS1 added (single bidirectional column). LP proceeds unchanged. |
| T9 | Single-direction line (`tmax_ba = 0`) + `use_sos1 = true` | `fn_col` absent; resolver returns false; no SOS1 emitted. No warning needed (expected case). |
| T10 | CLP backend + `use_sos1 = true` | Hard error emitted at LP build time; run terminates with clear diagnostic. |
| T11 | SDDP backward-pass SOS1 clear | After forward-pass MIP solve, the backward-pass LP relaxation calls `clear_sos1()`; the backward LP solves without SOS1 sets and without MIP engine. |
| T12 | `low_memory` ordering invariant | With `low_memory = true`, verify that `fix_and_resolve` restores all mutated upper bounds before `reconstruct()` fires. Inspect final LP bound via `li.get_col_upper_raw(fn_col)` post-reconstruct. |

---

## 13. Glossary

- **SOS1 (Special Ordered Set of type 1)** — a combinatorial
  constraint asserting that at most one variable in a named set
  is non-zero at any feasible solution. Defined by Beale & Tomlin
  (1970). Implemented natively in CPLEX, HiGHS, and CBC via
  specialised branching; no extra binary variable is needed.
- **wash flow** — a degenerate LP solution where both `fp > 0`
  and `fn > 0` simultaneously on the same line, yielding net flow
  `fp − fn` that is physically correct at bus nodes but carries
  spurious gross flows that distort line duals.
- **fix-and-resolve** — the two-step post-MIP procedure: (1) read
  the MIP primal, suppress the losing direction's upper bound to
  zero, (2) re-solve as a pure LP to extract clean KVL and KCL
  duals.
- **winning direction** — the flow direction (`fp` or `fn`) that
  carries non-zero flow at the MIP solution, as determined by the
  `kSos1PrimalTol` comparison.
- **losing direction** — the flow direction suppressed to zero in
  the fix-and-resolve step.
- **`fp_col`** — the LP column for A→B positive flow, bounded by
  `[0, tmax_ab]` in directional modes. Emitted by
  `source/line_losses.cpp` for `linear`, `piecewise`, and
  `bidirectional` modes.
- **`fn_col`** — the LP column for B→A positive flow, bounded by
  `[0, tmax_ba]` in directional modes. Emitted alongside `fp_col`
  when `block_tmax_ba > 0` (see `source/line_losses.cpp:745`).
- **`tmax_ab`** — magnitude of the maximum allowed A→B flow [MW];
  upper bound on `fp`. Defined in `include/gtopt/line.hpp:159`.
- **`tmax_ba`** — magnitude of the maximum allowed B→A flow [MW];
  upper bound on `fn`. Defined in `include/gtopt/line.hpp:158`.
  Note: both are positive magnitudes by convention — `fn` is a
  positive-valued variable representing B→A flow; the sign of
  the net flow is encoded in bus-balance stamps (`brow_a[fn] = +1`,
  `brow_b[fn] = -1`, confirmed at `source/line_losses.cpp:730-757`).
- **`k_solver_abi_version`** — plugin ABI version constant in
  `include/gtopt/solver_backend.hpp:37`. Must be bumped when the
  vtable changes. Plugins built against a different ABI version
  are rejected at load time.

---

## 14. References

- Beale, E. M. L., Tomlin, J. A. (1970). *Special facilities in a
  general mathematical programming system for non-convex problems
  using ordered sets of variables.* Proc. 5th IFORS International
  Conference on Operational Research, pp. 447–454. (Original SOS1
  definition.)
- Fisher, E. B., O'Neill, R. P., Ferris, M. C. (2008). *Optimal
  Transmission Switching.* IEEE Transactions on Power Systems,
  23(3), pp. 1346–1355. (Binary indicator transmission switching;
  background reference for MIP cost on meshed networks.)
- Hedman, K. W., O'Neill, R. P., Fisher, E. B., Oren, S. S. (2008).
  *Optimal Transmission Switching with Contingency Analysis.* IEEE
  Transactions on Power Systems, 24(3), pp. 1577–1586.
- Energy Exemplar. *PLEXOS Documentation* — `Line Hurdle Rate`,
  `Line Flow Limit` enforcement, directional line limits.
- IBM ILOG CPLEX Callable Library Reference Manual —
  `CPXaddsos` (SOS1/SOS2 constraint addition); ABI compatibility
  notes for plugin loading.
- HiGHS documentation — `Highs_addSos` / `Highs_passSos` (C API).
  **Verify exact function name against installed `Highs_c_api.h`
  before implementing Phase 0.**
- COIN-OR CBC / Osi documentation — `OsiSOS`, `OsiSolverInterface::
  addSOS`; `OsiCbcSolverInterface` SOS branching support.

### Internal artefacts referenced

- `include/gtopt/line.hpp:158-159` — `tmax_ba`, `tmax_ab` field
  definitions.
- `include/gtopt/line_losses.hpp:183-198` — `BlockResult` struct
  with `fp_col` / `fn_col` / `seg_p_cols` / `seg_n_cols`.
- `source/line_losses.cpp:647-688` — `add_none`: single
  bidirectional flow column; no `fn_col` emitted.
- `source/line_losses.cpp:718-770` — `add_linear`: separate
  `fp_col` and `fn_col` gated on `block_tmax_ab > 0` /
  `block_tmax_ba > 0`.
- `source/line_losses.cpp:841-992` — `add_piecewise`: aggregator
  `fp_col` + `fn_col` with segment cols.
- `source/line_lp.cpp:226-284` — block loop in `add_to_lp`;
  `block_tmax_ab` and `block_tmax_ba` resolved from the
  `tmax_ab` / `tmax_ba` per-(stage, block) schedules.
- `source/line_lp.cpp:519-589` — `add_to_output`: primal-solution
  emission; unaffected by fix-and-resolve.
- `include/gtopt/solver_backend.hpp:37` — `k_solver_abi_version`
  (currently 8; must become 9).
- `include/gtopt/solver_backend.hpp:274-285` — `relax_all_integers`;
  SOS1 sets are NOT removed by this call — `clear_sos1()` is a
  separate virtual.
- `source/line_losses.cpp:70-100` — `resolve_mode`; pattern
  mirrored by `resolve_use_sos1`.
- `docs/design/commitment-layout.md` — structural template and
  naming conventions followed by this document.
