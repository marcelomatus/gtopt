# Relax-and-fix rolling-window integrality — design + 04-12 perf verdict

## What it is

Relax-and-fix is the classical rolling-window MIP heuristic for unit commitment
(Wolsey, *Integer Programming* §12.5; Pochet & Wolsey 2006). The model stays
**whole** — every constraint row, including reservoir/storage intertemporal
coupling, is in the LP for every solve — but the commitment binaries are integer
only inside a rolling time window:

```
window k solve:   [ fixed 0/1 | integer (overlap + window k) | relaxed ]
                    past          active                        future
```

After window *k*'s MIP, the active binaries that leave the window (all but the
trailing `overlap_hours`) are fixed via bounds to their solved 0/1 values, and
the next window's binaries become integer. The **final** window's solve — every
past window pinned, the last window integer — is the returned MIP solution, so
the backend ends in the exact state a plain MIP solve leaves it in (integrality
intact, incumbent published), and the normal output / dual-recovery / dump paths
apply unchanged.

If any window MIP fails (infeasible after a bad early fix, solver error), the
orchestrator restores every pinned bound and every integrality flag and falls
back to the plain full MIP — relax-and-fix can lose time, never a solution.

## Options (monolithic, off by default)

| Option | Default | Effect |
|--------|---------|--------|
| `monolithic_options.relax_and_fix_window` | `0` (off) | Hours per rolling window. `> 0` enables the loop. CLI: `--set monolithic_options.relax_and_fix_window=24` |
| `monolithic_options.relax_and_fix_overlap` | `0` | Hours re-optimized from the previous window (softens end-of-window myopia) |

## Implementation

- `include/gtopt/relax_and_fix.hpp` / `source/relax_and_fix.cpp` — the
  orchestrator. Operates **in place** on the live `LinearInterface` (LP built
  once, never cloned); only column integrality flags and bounds are mutated,
  through the solver-agnostic raw setters. Re-asserts every originally-integer
  column's bound state after each window (scrubs CBC node-bound residue).
- `source/system_lp.cpp` — `SystemLP::resolve` builds the block-start-hour map
  from `phase().stages()` and partitions the `CommitmentRunInfo` status columns
  into windows via `make_relax_and_fix_cols`.
- `test/source/test_relax_and_fix.cpp` — 6 doctest cases: pure partition
  builder; 3-window solution == full-MIP optimum per MIP plugin; whole-model
  storage/SOC intertemporal chain never violated; single window == plain MIP;
  JSON parse + merge.

### Solver support

- **cplex / scip / highs / mindopt**: drive the in-place rolling loop. cplex
  is the production target (all windowing tests green).
- **cbc**: `solve_relax_and_fix` **gates cbc to a single plain full-MIP solve**
  (`windows == 1`, correct optimum, no rolling). CBC's `OsiCbc` backend cannot
  re-solve a mutated model in place — a later window declares "LP relaxation
  infeasible" and even the fallback returns a corrupted (non-integer) solution
  (known CBC quirk from the MIP-start round-trip campaign). The gate keeps cbc
  users safe and CI green (CI has cbc, not cplex). A dedicated test
  (`relax_and_fix - CBC gates to the plain full MIP`) covers it.

## 04-12 performance verdict — NEGATIVE (root-dominated case)

**Question**: does relax-and-fix accelerate the CEN 2026-04-12 weekly UC MIP
(168 h, 1 stage, real Kirchhoff, 247 buses, 1773 generators, ~53k binaries)
under cplex?

**Answer: no.** relax-and-fix attacks the branch-and-bound *tree*, but 04-12's
bottleneck is the **root node**, not the tree.

Evidence — a `relax_and_fix_window=24` (→ 7 windows) run, window 0 = 24 blocks
integer (7620 binaries vs 53085 for the full MIP, confirming the feature
activates correctly):

```
Root node processing (before b&c):
  Real time = 1800.71 sec. (475229.66 ticks)
Parallel b&c, 18 threads:
  Real time = 0.00 sec. (0.00 ticks)      ← never branched
```

- Window 0's **root node alone consumed the full 1800 s time limit** — root LP
  barrier ≈ 549 s / 174 002.67 ticks, plus CPLEX's root cut passes + RINS
  heuristics — **without ever branching**.
- That root cost is **identical whether 24 or 168 blocks are integer**, because
  relaxing integrality does **not** shrink the LP (all 168 blocks of rows /
  columns stay). relax-and-fix shrinks the *tree* (434 vs 2445 root IInf) but the
  tree is never reached.
- Window 0 not reaching optimality → **fallback to the full MIP** (53085
  binaries) — exactly what the feature is meant to avoid.

The deterministic tick count is byte-identical across the two root solves
(174 002.67 ticks), proving the root work is the same LP every window. Paying
that dominant cost once per window makes relax-and-fix a **net loss** here.

This sharpens the campaign's prior knowledge ("04-12 never converges with real
Kirchhoff on any machine") with the precise mechanism: **the root node (LP +
cuts + heuristics), not the branch tree, is the unaffordable part.**

### Where relax-and-fix *does* help

Cases where the **B&B tree dominates** the root LP: the per-window root is cheap,
each window's small tree closes fast, and the whole-model coupling keeps every
window feasible. relax-and-fix is the right lever there — the unit tests prove it
reproduces the full-MIP optimum on such fixtures. 04-12 is the pathological
exception (root-dominated), not the rule.

### Possible rescue (untried)

The root ate 1800 s but the LP was only ~549 s; the rest was CPLEX's root
cut/heuristic loop (HeuristicEffort 0.3, RINS 50). Injecting per-window solver
options that make CPLEX **branch immediately** (no RINS, minimal cuts, MIP
emphasis feasibility) could let the small window tree be reached and closed.
Requires relax-and-fix to pass distinct solver options for window solves.
Uncertain payoff (the 549 s barrier root is still large per window).

## Notes

- `--set monolithic_options.mip_start.enabled=false` did **not** propagate in the
  test run (the case's default `mip_start: {enabled: true}` still applied a start
  → "No solution found from 1 MIP starts"), while
  `--set monolithic_options.relax_and_fix_window=24` **did**. Nested-object
  `mip_start` merge quirk — pre-existing, unrelated to relax-and-fix; worth a
  separate look.
- relax-and-fix does not change the LP formulation (the model stays whole), so no
  `docs/formulation/` update is required — it is a solve-time heuristic only.
