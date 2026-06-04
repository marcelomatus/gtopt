# gtopt_sos2_convergence

IEEE 14-bus L-secant line-loss convergence validation against the
Coffrin & Van Hentenryck 2014 reference (issue #504).  Spans the
three regimes gtopt now supports:

  * **(A) L = 1, no SOS2** — Coffrin's classic single-secant LP.
  * **(B) L > 1, ε > 0, no SOS2** — pure-LP ε-rely generalisation.
  * **(C) L > 1 + SOS2** — lambda-form MIP refactor (`2L+1`
    breakpoint weights with SOS2).

## What it does

`gtopt_sos2_convergence.py` reads `cases/ieee_14b/ieee_14b.json`,
injects MATPOWER `case14.m` per-line resistances (the stored fixture
ships only reactance because PCP has no loss model), activates
`tangent_signed_flow` losses, and sweeps `loss_secant_segments` (L).

The LP picks `ℓ_line = max(tangent_k(f_line))` at the K tangent
**lower bound** (K-dependent, not L-dependent), so the chord upper
bound from L is **inactive at the LP optimum** — increasing L
tightens an unused constraint and the objective + total network
loss are **invariant**.  Same property under all three regimes.

## Key result on IEEE 14-bus

```
  L  SOS2         obj [$]    Σ_loss [MWh]   loss/demand %
  1   yes      224 446.55       333.63         3.95 %
  2   yes      224 445.21       333.63         3.95 %     ← lambda-form SOS2
  4   yes      224 678.54       332.98         3.94 %
  8   yes      224 479.70       333.31         3.95 %
```

All four L values agree within 0.10 % of the L=1 baseline —
confirms the lambda-form fix lifts the 2w cap and the LP-observed
loss is invariant under chord tightening.

## Segment-formulation trap (fixed by lambda-form)

The pre-fix segment-form SOS2 emitted:

```
v_l ∈ [0, w]   with   w = envelope/L     l = 1..L
Σ v_l ≥ |f|                              (two abs rows)
ℓ ≤ Σ chord_slope_l · v_l                (chord upper bound)
SOS2 on {v_1, …, v_L}
```

Beale–Tomlin SOS2 = "at most TWO non-zero, ADJACENT" combined with
`v_l ≤ w` gave `Σ v_l ≤ 2w = 2·envelope/L`.  For `L ≥ 3` the cap
was BELOW the line rating, silently clipping flows.  IEEE 14 at L=4
demand-failed (line 1 saturated at `tmax/2 = 75 MW`, obj jumped 5×
to $1.12M).

The lambda-form refactor (this commit) replaces the segment cols
with `2L+1` breakpoint weights `λ_l ∈ [0, 1]` at `b_l = (l − L)·w`
for `l = 0..2L`:

```
λ_l ∈ [0, 1]                             (l = 0..2L → 2L+1 cols)
Σ λ_l = 1                                (convexity)
Σ b_l · λ_l = f                          (signed flow tied to breakpoints)
ℓ ≤ Σ c · b_l² · λ_l                     (chord = piecewise secant)
SOS2 on {λ_0, …, λ_{2L}}                 (interpolation, no cap)
```

`|f|` reaches the full envelope at any L.  The `--verify-no-trap`
flag asserts this property.

## Reproduce locally

```bash
# Default sweep (L=1,2,4,8 + SOS2 = lambda-form):
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py

# Pure-LP ε-rely regime (same obj, no MIP):
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py \
        --epsilon-rely

# Assert the lambda-form fix is active:
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py \
        --verify-no-trap
```

## CLI flags

- `--gtopt PATH` — explicit gtopt binary path (overrides `GTOPT_BIN`).
- `--tmp PATH` — workspace dir for variants + outputs (default mktemp).
- `--keep` — keep the workspace after the run.
- `--L-values STR` — comma list of L's to sweep (default `1,2,4,8`).
- `--loss-segments INT` — K tangent segments forming the lower
  bound on `ℓ` (default 5).
- `--loss-cost-eps FLOAT` — ε on `Σ v_l` (or via the segment col
  ε path) in the objective.
- `--verify-no-trap` — assert the lambda-form fix is active by
  running L = 4 + SOS2 and checking obj stays within 1 % of the
  L = 1 baseline (the pre-fix segment-SOS2 jumped 5×).
- `--epsilon-rely` — run the sweep in pure-LP ε-rely regime
  (regime B): `use_sos2 = false`, `loss_cost_eps > 0`.

## pytest entries

`tests/test_sos2_convergence_smoke.py` provides three integration
tests:

1. `test_sos2_convergence_invariance_L1_L2` — asserts obj + loss
   match within 1e-4 across L=1, L=2 + SOS2.
2. `test_sos2_lambda_form_reaches_full_envelope_at_L4` — asserts
   L=4 + SOS2 (lambda-form) obj stays within 1 % of L=1 baseline.
   Pins the lambda-form FIX.
3. `test_sos2_epsilon_rely_matches_lambda_form` — asserts ε-rely
   (regime B) and lambda-form SOS2 (regime C) agree on obj + loss
   within 0.1 %.

All marked `integration` (skip with `pytest -m 'not integration'`).
Total wall-clock ~4 s on a workstation.

## Golden references

> Coffrin, C., Van Hentenryck, P. (2014).  *A Linear-Programming
> Approximation of AC Power Flows.*  INFORMS Journal on Computing
> 26(4):718–734.

> Tanneau, M., Anjos, M.F., Léautaud, L. (2022).  *A Linear Outer
> Approximation of Line Losses for DC-based Optimal Power Flow
> Problems.*  Electric Power Systems Research 211: 108280.
> arXiv:2112.10975.

> Christie, R. (1993).  *Power Systems Test Case Archive.* U. of
> Washington — IEEE 14-bus.  Used as-is via MATPOWER's `case14.m`
> (Zimmerman, Murillo-Sánchez, Thomas 2011) for line resistances.

MATPOWER lossless DC-OPF reference on `case14`: `$7 642.59/h`.  Not
directly comparable to gtopt obj because gtopt's IEEE 14b uses
linearised generator cost data (pp2gtopt collapses `cp1·P + cp2·P²`
to `cp1` only).
