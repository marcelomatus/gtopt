# Results: 2026-04-12 warm-start experiment — ticks table + NET verdict

Companion to [`mipstart-0412-experiment.md`](mipstart-0412-experiment.md). Closes
task #8 of the experiment brief. All numbers are **deterministic CPLEX ticks**
(machine-independent), captured from `~/tmp/mipstart_0412/case/*.log`.

## Setup

- Case: CEN PLEXOS daily UC **2026-04-12** (the "clean reference"), converted with
  `--lift-line-caps`, K=8 loss secants, S=1 (single secant per lift), real
  Kirchhoff (`use_kirchhoff=true`, cycle-basis mode). Optimum obj ≈ 9.73e8.
- Solver: CPLEX, Release/-O3 binary.
- Runs executed one at a time (governor SIGKILLs at host load > ~50).

## Ticks table (final runs)

| Run            | Seed                      | MIP-start accepted?                         | Root-relax   | Terminal state                          |
|----------------|---------------------------|---------------------------------------------|--------------|-----------------------------------------|
| `cold_final`   | none                      | —                                           | 191,148 tk / 603 s | timed out at root (exit 125), no close |
| `ws_uni_final` | uninodal (copper-plate)   | **YES** — `MIP start 'm1' … obj 984977.6`   | 191,148 tk   | 2,822,629 tk / 7,478 s (exit 125), no close |
| `ws_tra_final` | transport-reduced         | **NO** — `No solution found from 1 MIP starts` | 191,148 tk | fell back to cold, no close             |

Root-relaxation ticks are **identical** (191,148) across all three runs: the seed
never touches the LP relaxation, only the incumbent.

## NET verdict on the hard case: **NEGATIVE**

A reduced-network MIP-start seed gives **no net speedup** on 04-12:

1. **Acceptance ≠ closure.** The uninodal seed *is* accepted as an incumbent
   (obj 984,977.6), and it does push CPLEX past the root into branch-&-cut (tree
   grew to ~13 MB, `solutions = 1`). But the search still fails to close the gap
   within budget — 2.82 M ticks vs cold's stalled 191 k-tick root. Handing the
   solver a feasible incumbent does not shrink the tree here.
2. **The bottleneck is gap-closing, not the network or the root warm-start.** The
   relaxation cost is fixed (191,148 tk) regardless of seed; what fails to
   converge is the branch-&-cut proof of optimality (commitment combinatorics),
   which a warm incumbent cannot accelerate.
3. **Transport seed was rejected outright** by the plain (pre-elastic) file path —
   the exact `No solution found from 1 MIP starts` failure the elastic completion
   (below) now repairs.

This confirms the earlier standalone finding: on hard cases the reduced network
does **not** help the MIP — the neck is commitment feasibility / gap-closing, not
the network.

## POSITIVE result on the small/fast case: elastic completion validated

The one mechanism that *did* move: **elastic in-process seed completion**
(`dfb058a5`, `source/mip_start.cpp`). On the small/fast case, the same imperfect
seed that the plain file path throws away is repaired in-process and accepted:

| Path                         | Result                                          |
|------------------------------|-------------------------------------------------|
| plain file seed (imperfect)  | `No solution found from 1 MIP starts` (rejected) |
| elastic in-process completion| `1 of 1 MIP starts provided solutions` (accepted); warm-started off the basis (0.32-tk barrier → primal simplex) |

This fixes the `ws_tra_final` rejection **at the mechanism level**: a seed no
longer has to be perfectly feasible to be accepted. It removes the
"depend on a perfect seed" constraint — but note (per the NET verdict) that
acceptance alone is not a speedup on hard, gap-bound cases.

## Where the speed lever actually is

- Making the seed *closer* or *more acceptable* is necessary plumbing but is **not**
  a lever on hard 04-12: the tree is lower-bound / gap bound, not incumbent-bound.
- Remaining leverage sits on the branch-&-cut itself:
  - **relax-and-fix rolling window** (attacks the B&B combinatorics directly,
    seed-independent),
  - **cut-parameter tuning** (fewer/cheaper cuts at the root),
  - **two-gap checkpointing** (bank a usable incumbent at a coarse gap, then keep
    grinding at no visible cost).
