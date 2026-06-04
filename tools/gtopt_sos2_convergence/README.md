# gtopt_sos2_convergence

IEEE 14-bus L-secant + SOS2 line-loss convergence validation against
the Coffrin & Van Hentenryck 2014 reference (issue #504).

## What it does

`gtopt_sos2_convergence.py` reads `cases/ieee_14b/ieee_14b.json`,
injects MATPOWER `case14.m` per-line resistances (the stored fixture
ships only reactance because PCP has no loss model), activates
`tangent_signed_flow` losses, and sweeps `loss_secant_segments` (L)
to compare:

  * **L = 1** — Coffrin's classic single-secant LP approximation.
  * **L = 2 + SOS2** — gtopt's L-secant tightening (issue #504).

The LP picks `ℓ_line = max(tangent_k(f_line))` at the K tangent
**lower bound** (K-dependent, not L-dependent), so the chord upper
bound from L is **inactive at the LP optimum** — doubling L tightens
an unused constraint and the objective + total network loss are
**invariant**.  That's the chord-tightening invariance the script
validates.

## Key result on IEEE 14-bus

```
  L  SOS2         obj [$]    Σ_loss [MWh]   loss/demand %
  1   yes      224 446.55       333.63         3.95 %
  2   yes      224 446.55       333.63         3.95 %     ← exact match (LP invariant)

  4   yes    1 120 006.54       108.14         1.28 %     ← --probe-sos2-trap
```

L=1 and L=2+SOS2 agree to 1e-4 — confirms the LP-observed loss is
the max-tangent lower bound and is invariant under chord tightening.
L=4 is the **segment-formulation trap** documented below.

## SEGMENT-FORMULATION TRAP (L ≥ 3)

gtopt's L-secant uses a **segment-fill** form:

```
v_l ∈ [0, w]   with   w = envelope/L     l = 1..L
Σ v_l ≥ |f|                              (two abs rows)
ℓ ≤ Σ chord_slope_l · v_l                (chord upper bound)
SOS2 on {v_1, …, v_L}                    (issue #504)
```

Canonical Beale–Tomlin SOS2 = "at most TWO non-zero, ADJACENT".
Combined with `v_l ≤ w` that gives `Σ v_l ≤ 2w = 2·envelope/L`.

| L | cap on `|f|`     | line headroom         |
|--:|:-----------------|:----------------------|
| 1 | `2·envelope`     | unconstrained         |
| 2 | `envelope`       | exactly at line max   |
| 3 | `2·envelope/3`   | **clipped at ⅔ tmax** |
| 4 | `envelope/2`     | **clipped at ½ tmax** |

For IEEE 14 at L=4 the LP can't deliver demand → demand-fail kicks
in → obj jumps ~5×.  The `--probe-sos2-trap` flag reproduces this.

A correct SOS2 formulation needs the **lambda-form** (`Σ λ_k = 1`,
SOS2 on K+1 breakpoint weights) or **explicit fill-order binaries**
(`v_l ≤ w · y_l`, `y_l ≤ y_{l-1}`).  See the script's footer for
the proposed fix.

**Until the fix lands, the safe regime is L ≤ 2** (the script's
default).  L=2 already gives the 4× chord-tightness improvement
over L=1.

## Reproduce locally

```bash
# Default sweep (L=1, L=2 + SOS2):
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py

# Demonstrate the L=4 segment-formulation trap:
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py \
        --probe-sos2-trap

# Compare against L>1 WITHOUT SOS2 (chord no longer a valid UB):
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py \
        --L-values 1,2 --also-no-sos2
```

## CLI flags

- `--gtopt PATH` — explicit gtopt binary path (overrides `GTOPT_BIN`).
- `--tmp PATH` — workspace dir for variants + outputs (default mktemp).
- `--keep` — keep the workspace after the run.
- `--L-values STR` — comma list of L's to sweep (default `1,2`).
- `--loss-segments INT` — K tangent segments forming the lower
  bound on `ℓ` (default 5).
- `--loss-cost-eps FLOAT` — ε on `Σ v_l` in the objective.
- `--probe-sos2-trap` — additionally run L=4 + SOS2 to demonstrate
  the segment cap (line saturation at `tmax/2`, obj jumps ≥ 2×).
  Asserts the trap triggers (will fail when the SOS2 fix lands).
- `--also-no-sos2` — also run L>1 without SOS2 (chord no longer a
  valid UB; LP under-estimates loss).

## pytest entry

`tests/test_sos2_convergence_smoke.py` provides two integration
tests:

1. `test_sos2_convergence_invariance_L1_L2` — asserts obj + loss
   match within 1e-4 across L=1, L=2 + SOS2.
2. `test_sos2_segment_trap_at_L4` — asserts the L=4 trap fires
   (obj > 2× L=1 baseline).  Pins the **known bug** so a future
   SOS2 reformulation will fail this test and force a re-think.

Both are marked `integration` (skip with `pytest -m 'not integration'`).
Total wall-clock ~3 s on a workstation.

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
