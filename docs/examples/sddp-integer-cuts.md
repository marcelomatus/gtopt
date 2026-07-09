# Strengthened Benders cuts (`integer_cuts_mode`) — user guide

`sddp_options.integer_cuts_mode = "strengthened"` upgrades the SDDP
backward pass on subproblems that carry integer columns.  This page
is the user-facing companion to the
[SDDiP investigation](../analysis/investigations/sddp/sddip_integer_expansion_2026-07.md)
(theory, Theorems SB1/SB2) and documents how to enable the option,
what changes internally, what to expect from the bounds, and the
current limitations.

## 1. When integer cuts matter

Your SDDP subproblems become MIPs when the system carries any of:

- **unit commitment** — `SimpleCommitment` binaries per block (see
  [sddp-simple-commitment.md](sddp-simple-commitment.md)), the full
  `Commitment` element, or line commitment/switching;
- **integer expansion** — `integer_expmod: true` on any expansion
  candidate (Generator, Line, Battery, …): the per-stage module
  count becomes an integer column.

With the default `integer_cuts_mode = "none"` such cells are valued
by their **convexification at best**: aperture-path cuts explicitly
relax integrality before reading duals (they support
`V_LP ≤ V_MIP`), and the aperture-less pure-Benders path reads
reduced costs off a MIP re-solve, which is unsound in general
(investigation doc §1).  Wherever integrality binds — lumpy builds,
binding `pmin` — the policy under-values the true cost-to-go by the
integrality gap.

If your subproblems are pure LPs (the common production case), the
option is inert and byte-identical to `"none"`; there is nothing to
enable.

## 2. How to enable

JSON (planning options):

```json
{
  "options": {
    "method": "sddp",
    "sddp_options": {
      "integer_cuts_mode": "strengthened"
    }
  }
}
```

CLI override on an existing case (dotted `--set` path):

```bash
gtopt case.json --solver highs \
  --set sddp_options.integer_cuts_mode=strengthened
```

`run_gtopt` forwards the same planning JSON; `plp2gtopt`-converted
cases take the option through their generated `*_options.json` the
same way.  Note: the key is routed by its full dotted path;
`gtopt_config`'s bare-key auto-nesting table
(`scripts/gtopt_shared/options_meta.py`) does not list
`integer_cuts_mode` yet, so always spell the `sddp_options.` prefix.

Values: `"none"` (default) | `"strengthened"`.  Anything else
hard-errors at ingestion.

## 3. What changes internally

Per backward step whose **target cell LP carries integer columns**
(the gate is `has_integer_cols()` — pure-LP cells never enter the
path):

1. a clone of the target LP is relaxed to its LP form and solved at
   the trial state — this replaces the unsound reduced-cost read and
   yields the certified Theorem-O1 cut slopes `λ` and intercept
   `b_LP`;
2. integrality is restored, the state pins are relaxed to the state
   box, the multiplier `λ` is charged on the state columns, and the
   resulting **Lagrangian MIP is solved once**, giving `m*`;
3. the emitted cut keeps the LP slopes and takes the intercept
   `max(b_LP, m*)` — never looser than the plain LP-relaxation cut
   (Corollary SB2), still a valid under-estimator of the true MIP
   recursion (Theorem SB1, weak Lagrangian duality).

Cost: **one extra MIP solve per backward cut** on integer-bearing
cells (with `mip_gap = 1e-9` pinned and a 30 s default time limit).

Fallbacks are silent by design:

- LP-relaxation clone solve fails → legacy cut path (WARN logged);
- Lagrangian MIP fails or times out → the step-1 LP-relaxation cut
  is emitted untightened (DEBUG logged).

### Aperture inertness (important)

The strengthened path is wired into the **pure-Benders backward pass
only** (`sddp_options.apertures` unset/empty, `num_apertures = 0`).
When the aperture backward pass is active, cuts come from the
aperture clones, which relax integrality regardless of this option
(cut-validity ledger F5) — `integer_cuts_mode = "strengthened"` then
changes **nothing**: cuts and bounds are identical to `"none"`.
This is pinned by a regression test
(`test_sddp_simple_commitment.cpp`, "inert under the aperture
backward pass"), so a future aperture-path extension will surface as
a deliberate test change.  If you need strengthened cuts today, run
the affected phases without apertures.

## 4. Solver requirements and the CI blind spot

- A **MIP-capable backend** is required end-to-end (the forward pass
  already solves MIPs whenever integer columns exist): CPLEX, HiGHS,
  SCIP, CBC, Gurobi or MindOpt.  LP-only backends (CLP) refuse
  integer columns at load time.  cuOpt's GPU branch-and-bound is not
  bit-exact and is excluded from the test pins.
- CI runs with `GTOPT_SOLVER=clp`, so **every MIP-gated test skips
  in CI** (`solver_test::first_mip_solver()` + `run_or_skip_license`
  conventions).  The known mitigation idea is a nightly job pinned to
  HiGHS that runs the MIP-gated suites; until that lands, run the
  suites locally against HiGHS/CPLEX after touching this machinery:

  ```bash
  ./build/test/gtoptTests -tc="*strengthened*,*commitment*,*expansion*"
  ```

## 5. Expected LB behaviour (worked numbers)

On the commitment witness fixture of
[sddp-simple-commitment.md](sddp-simple-commitment.md) (demand 60,
`pmin` 40, inflows 28/22, 3 phases):

- extensive-form MIP optimum: **$25,200**;
- extensive-form LP relaxation: **$17,676** — a $7,524 integrality
  gap that pure LP-relaxation cuts can never certify past;
- both modes keep every iteration's LB ≤ $25,200 (validity), both
  converge onto it, and the `"strengthened"` LB is ≥ the `"none"` LB
  at convergence.  (Per-iteration monotonicity vs the legacy path is
  not a theorem — SB2 orders the strengthened cut against the
  LP-relaxation cut; on this particular fixture the legacy flat cut
  is accidentally exact from iteration 1, so `"strengthened"`
  briefly trails it before matching.)

Do not expect the gap to close to zero: strengthened cuts are valid
and tighter, but generally **not tight** at the trial point (only
full SDDiP Lagrangian cuts are, and those are deliberately not
shipped — investigation doc §5).  Typical outcome on
commitment-heavy fixtures is an LB visibly above the LP-relaxation
plateau with a residual gap to the MIP optimum that reflects the
state-dependence of the integrality gap.

At the unit level, the hand-checkable example from
`test_sddp_strengthened_cuts.cpp` shows the mechanics: LP cut
`b_LP = 3.0`, Lagrangian intercept `m* = 5.0`, truth
`V_MIP(x) = 5 − x` — the strengthened cut `α ≥ 5 − 3x` supports the
truth at `x = 0` where the LP cut `α ≥ 3 − 3x` sat $2 below.

## 6. Integer expansion: prefer the investment-master route

For **integer expansion** (lumpy builds), strengthened cuts make the
recourse valuation sound, but the production-grade architecture is
the OptGen-style split: an investment master MIP over builds +
operational SDDP with capacities fixed, exchanging capacity-space
Benders cuts.  The extraction API (`extract_capacity_cuts`) ships
today and is exercised on a production-shaped fixture in
`test_sddp_expansion_sharing.cpp`; the master loop itself is
designed but not yet implemented (investigation doc §7).

> **Fixed — multi-scene CAPEX weighting (found 2026-07-08, fixed
> 2026-07-09).**
> Per-scene SDDP LPs previously priced expansion carrying cost
> (`capacost`) at probability weight 1.0 while dispatch folded by the
> scene probability `p_s` (`capacity_object_lp.cpp`,
> `stage_ecost(stage, 1.0)`); correct in the all-scenario monolithic
> LP, but in each scene-LP the CAPEX was over-weighted by `1/p_s`
> (×N for N equiprobable scenes), shifting the reported objective by
> exactly one duplicated carrying charge (build *decisions* were
> unaffected).  The fix folds the carrying-cost objective coefficient
> by the owning scene's total probability mass —
> `stage_ecost(stage, 1.0, scene_prob)` with
> `scene_prob = SceneLP::probability_factor()` — which equals `p_s`
> per scene and `1.0` in the monolithic single-scene LP (so monolithic
> costs are unchanged).  Summed across scenes the carrying charge is
> now counted exactly once, and multi-scene SDDP LB/UB match the
> extensive form.  Regression-pinned by `test_sddp_expansion_sharing.cpp`
> (tests 4-5, Benders ≡ SDDP objective equalities) and ledger F14.

## Cross-references

- [SDDiP investigation](../analysis/investigations/sddp/sddip_integer_expansion_2026-07.md)
  — theory (SB1/SB2), fit analysis, OptGen design, effort ledger.
- [SDDP cut-validity theorem doc](../formulation/sddp-cut-validity.md)
  — the LP cut family the strengthened cut extends.
- [Cut-validity findings ledger](../analysis/investigations/sddp/sddp_cut_validity_findings_ledger_2026-07-08.md)
  — F5 (aperture MIP relaxation).
- [SDDP method guide](../methods/sddp.md) — `sddp_options` reference.
- [SimpleCommitment example](sddp-simple-commitment.md) — the
  commitment use case these numbers come from.
- Tests: `test/source/test_sddp_strengthened_cuts.cpp`,
  `test/source/test_sddp_simple_commitment.cpp`,
  `test/source/test_sddp_expansion_sharing.cpp`,
  `test/source/test_sddp_capacity_cuts.cpp`.
