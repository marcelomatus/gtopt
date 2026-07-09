# SimpleCommitment under SDDP — a worked use case

This example shows how to run a unit on/off commitment
(`SimpleCommitment`) inside an SDDP planning study, what the solver
does with the binary columns in the forward and backward passes, and
when to turn on `integer_cuts_mode = "strengthened"`.

Scope: the *simple* commitment variant only — one binary status `u`
per block with `pmin·u ≤ g ≤ pmax·u`.  No minimum up/down times, no
ramping, no startup/shutdown costs (those belong to the full
`Commitment` element; see
[§5.15–5.16 of the formulation](../formulation/mathematical-formulation.md)).

The numbers below are reproduced by
`test/source/test_sddp_simple_commitment.cpp`.

## 1. The model

A single bus with a reservoir hydro unit, one thermal unit carrying
the commitment, and a constant demand, solved over 3 phases (one
stage of four 1-hour blocks each) and 2 hydrological scenes:

| Element | Data |
|---------|------|
| Hydro   | 50 MW, $5/MWh, reservoir box [0, 200] dam³, eini = 100 |
| Thermal | 200 MW, $50/MWh, `dispatch_pmin` = 40 MW (binary `u`) |
| Demand  | 60 MW constant, `demand_fail_cost` = $1000/MWh |
| Inflows | wet scene 28 dam³/h (p = 0.6), dry scene 22 dam³/h (p = 0.4) |

JSON fragment (element + options):

```json
{
  "options": {
    "method": "sddp",
    "sddp_options": {
      "max_iterations": 50,
      "convergence_tol": 1e-6,
      "integer_cuts_mode": "none"
    }
  },
  "system": {
    "simple_commitment_array": [
      {
        "uid": 1,
        "name": "sc_thermal",
        "generator": 2,
        "dispatch_pmin": 40.0,
        "relax": false
      }
    ]
  }
}
```

`relax: true` turns the binary into a continuous `u ∈ [0, 1]` (the LP
relaxation); `must_run: true` pins `u = 1`.  Phases marked
`continuous: true` (or covered by `model_options.continuous_phases`)
also relax the binary — cascade lower levels use this.

## 2. Why the pmin geometry matters

With inflow ≥ 22 dam³/h the reservoir always sustains more than
20 MW of hydro, so the thermal *residual* `60 − h` is below
`pmin = 40` at **every** reservoir state:

- **MIP dispatch** (what SDDP's forward pass solves): the unit is
  either OFF — stranding 10 MW at the $1000 fail cost (hydro caps at
  50 MW) — or ON at exactly `pmin`.  The optimum is pinned to
  `(h, t) = (20, 40)` in every block: $2,100/h, $25,200 over the
  12-hour horizon (probability-folded across both scenes).
- **LP relaxation** (`relax: true`): fractional `u = t/200` lets the
  unit serve the residual exactly, `t = 60 − h ∈ (0, 40)` — cheaper
  ($17,676 folded) but **integer-infeasible**.  The $7,524 difference
  is the integrality gap of this fixture.

The test asserts the *off-or-min* invariant on every solved block of
every (scene, phase) LP across iterations: `u ∈ {0, 1}` and
`g ∈ {0} ∪ [pmin, pmax]`, never inside `(0, pmin)` — and that the
relaxed twin genuinely lands inside `(0, pmin)`, proving the geometry
discriminates.

## 3. What SDDP does with the binaries

- **Forward pass**: each (scene, phase) subproblem is solved as a
  true MIP (branch-and-bound in the LP backend), so simulated
  dispatch and the UB estimate always honour the commitment logic.
- **Backward pass, default (`integer_cuts_mode = "none"`)**:
  - with **apertures** enabled, the backward clones explicitly relax
    integrality before reading duals (finding F5 of the
    [cut-validity ledger](../analysis/investigations/sddp/sddp_cut_validity_findings_ledger_2026-07-08.md)).
    The resulting cuts support the *convexified* value function
    `V_LP ≤ V_MIP` — valid for the true recursion, but they
    systematically under-value the commitment cost (the LB carries
    the integrality gap as permanent looseness);
  - on the **pure-Benders** path (no apertures) the legacy code reads
    reduced costs off a MIP re-solve — an unsound path in general
    (§1 of the
    [SDDiP investigation](../analysis/investigations/sddp/sddip_integer_expansion_2026-07.md)).
    On this fixture `V` happens to be *constant* in the reservoir
    state (the dispatch is pinned at every level), so the flat cut at
    the MIP incumbent is accidentally exact — do not rely on this in
    production.
- **Backward pass, `integer_cuts_mode = "strengthened"`**: the
  certified mode for integer-bearing subproblems.  Each backward cut
  is built from an explicit LP relaxation and its intercept is
  tightened by one extra MIP solve (Theorem SB1/SB2 in the
  investigation doc).  See
  [sddp-integer-cuts.md](sddp-integer-cuts.md) for the mechanism,
  costs, and caveats.

On this fixture both modes keep every iteration's LB below the
extensive-form MIP optimum ($25,200) and both converge onto it
(looseness 0 at the last iteration).  A subtlety worth knowing:
Corollary SB2 orders the strengthened cut against the *LP-relaxation
cut*, not against whatever the legacy path emitted — here the legacy
flat cut is accidentally exact from iteration 1 (constant `V`), so
the strengthened run briefly trails it (observed $23,256 vs $25,200
at iteration 1) before matching at convergence.  On fixtures whose
integrality gap varies with the state, the legacy path is the one
that over- or under-shoots.

## 4. Commitment × cut sharing

`cut_sharing = "none"` and `"multicut"` both preserve integer
feasibility of the commitment decisions — sharing only changes which
future-cost columns the cuts bound, never the subproblem integrality.
Bounds sanity follows the usual conventions: `LB ≤ UB` is strict
under `none` and under `multicut` with identical scenes; multicut
with heterogeneous scenes compares a resampled-process LB against a
persistent-process UB, which is not ordered in general (see
[§7–8 of the theorem doc](../formulation/sddp-cut-validity.md)).

## 5. Caveats

- **Solver**: binary columns need a MIP-capable backend (CPLEX,
  HiGHS, SCIP, CBC, …).  LP-only backends (CLP, and the CI default
  `GTOPT_SOLVER=clp`) refuse to load integer columns; the unit tests
  pin `solver_test::first_mip_solver()` and skip when none is
  loaded.  Check with `gtopt --solvers`.
- **Convexification**: with the default `integer_cuts_mode = "none"`
  the policy *values* the future as if commitment were fractional.
  Expect the LB to stall an integrality gap below the UB on
  commitment-heavy systems; `strengthened` recovers part of that gap
  at one extra MIP per backward cut.
- **`dispatch_pmin = 0`** (or `relax: true` with `pmin = 0`) makes
  the element inert: the SDDP run reproduces the no-commitment twin
  bit-for-bit — a useful A/B switch when calibrating.

## Cross-references

- [Mathematical formulation §5.16](../formulation/mathematical-formulation.md)
  — the SimpleCommitment LP rows.
- [SDDP method guide](../methods/sddp.md) — options reference
  (`sddp_options` table) and algorithm walk-through.
- [SDDP cut-validity theorem doc](../formulation/sddp-cut-validity.md)
  — what each cut certifies.
- [SDDiP investigation](../analysis/investigations/sddp/sddip_integer_expansion_2026-07.md)
  — integer recourse analysis, strengthened-cut theory (SB1/SB2).
- [Integer cuts example](sddp-integer-cuts.md) — enabling and tuning
  `integer_cuts_mode = "strengthened"`.
- Tests: `test/source/test_sddp_simple_commitment.cpp`,
  `test/source/test_sddp_strengthened_cuts.cpp`.
