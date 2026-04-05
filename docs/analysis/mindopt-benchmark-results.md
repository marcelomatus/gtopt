# MindOpt LP Solver Benchmark Results

- **Date**: 2026-04-04
- **File**: `support/lps/feasible_scene_1_phase_14.lp` (132,722 lines, 7.2MB)
- **MindOpt version**: 2.3.0
- **Platform**: Linux 6.6.87 (WSL2)
- **Expected optimal objective**: 1.3907191126e+09
- **Runs per config**: 3-5

## Method Comparison (4 threads)

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| Auto (Method=-1, selects dual) | 4,475 | 4,315 | 4,836 | 12,826 | 1.3907191126e+09 |
| Dual simplex (Method=1) | 4,642 | 4,341 | 5,027 | 12,826 | 1.3907191126e+09 |
| Primal simplex (Method=0) | 8,624 | 8,301 | 10,408 | — | 1.3907191126e+09 |
| **Barrier/IPM (Method=2)** | **4,601** | **4,369** | **4,972** | **30** | **1.3907191126e+09** |
| Barrier, PostScaling=0 | 3,306 | 3,276 | 4,467 | 31 | 1.3907191129e+09 |
| Barrier, IPM/GapTolerance=1e-10 | 4,761 | 3,381 | 5,155 | 44 | 1.3907191126e+09 |
| Barrier, IPM/NumericFocus=1 | 8,460 | 6,856 | 9,430 | 29 | 1.3907191126e+09 |

## Thread Sweep — Barrier

| Threads | Median (ms) | Min | Max | Iters | Notes |
|---------|-------------|-----|-----|-------|-------|
| 1 | 6,853 | 6,490 | 8,315 | 30 | slowest |
| 2 | 6,766 | 6,431 | 7,044 | 30 | moderate |
| **4** | **6,488** | **6,421** | **6,724** | **30** | **best, stable** |
| 8 | 8,468 | 7,459 | 9,166 | 30 | contention |

## Thread Sweep — Dual Simplex

| Threads | Median (ms) | Min | Max | Notes |
|---------|-------------|-----|-----|-------|
| 1 | 4,568 | 4,439 | 4,995 | |
| 2 | 4,541 | 4,332 | 4,887 | |
| 4 | 4,770 | 4,398 | 5,028 | |
| 8 | 4,641 | 4,434 | 5,401 | |

Threads have minimal impact on simplex — MindOpt's simplex is essentially single-threaded.

## Presolve

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| **Presolve=on (default)** | **4,437** | **4,434** | **4,609** | **32** | **1.3907191148e+09** |
| Presolve=off | 8,543 | 7,262 | 9,533 | 209-265 | 1.3907191234e+09 |

Presolve is critical — disabling it causes ~2x slowdown and 7-8x more iterations.

## Dualization

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| Auto (default) | 4,663 | 4,324 | 4,747 | 30 | 1.3907191147e+09 |
| **Off** | **4,426** | **4,325** | **4,709** | **30** | **1.3907191147e+09** |
| On | 10,762 | 4,953 | 18,720 | 58-71 | 1.3907185e+09 |

Forced dualization hurts badly on this LP (2-4x slower, worse accuracy).

## NumericFocus

| Config | Median (ms) | Min | Max | Iters |
|--------|-------------|-----|-----|-------|
| NumericFocus=0 (default) | 4,676 | 4,326 | 4,819 | 30 |
| NumericFocus=1 | 4,724 | 4,440 | 4,886 | 30 |

No meaningful difference.

## IPM Tolerances

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| GapTolerance=1e-8 (default) | 4,373 | 4,277 | 4,817 | 30 | 1.3907191147e+09 |
| GapTolerance=1e-6 (relaxed) | 6,346 | 6,272 | 6,718 | 20 | 1.3907191479e+09 |
| GapTolerance=1e-10 (tight) | 3,256 | 3,239 | 3,378 | 44 | 1.3907191126e+09 |

Counterintuitively, tighter tolerance (1e-10) is **faster** on this LP — the crossover
phase converges more cleanly from a tighter IPM solution.

## PostScaling

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| **PostScaling=0** | **3,306** | **3,206** | **3,764** | **31** | **1.3907191142e+09** |
| PostScaling=1 | 4,410 | 4,269 | 4,410 | 31 | 1.3907191147e+09 |

PostScaling=0 is ~25% faster, with minor objective drift (~1e-5 relative error).

## IPM NumericFocus

| Config | Median (ms) | Min | Max | Iters | Objective |
|--------|-------------|-----|-----|-------|-----------|
| IPM/NumericFocus=0 | 4,547 | 4,326 | 4,654 | 30 | 1.3907191147e+09 |
| IPM/NumericFocus=1 | 4,368 | 4,311 | 4,452 | 29 | 1.3907191146e+09 |

Marginally faster with NumericFocus=1; one fewer iteration.

## Detailed Solver Output (best configs)

### Barrier, PostScaling=0, 4 threads (fastest)
```
Presolver terminated. Time : 0.093s
Interior point method: 31 iterations, 0.26s
Crossover (simplex): 20,706 pushes, 0.80s
Total time: 0.914s
Objective: 1.3907191129e+09
```

### Dual simplex, 4 threads (exact objective)
```
Presolver terminated. Time : 0.093s
Simplex method: 12,826 iterations, 1.17s
Total time: 1.183s
Objective: 1.3907191126e+09  (exact match)
```

**Note**: Wall times above include ~2-3s of MindOpt startup overhead (license
check, environment initialization). The actual solve times from MindOpt's
internal reporting are 0.9-1.2s.

## Key Findings

1. **Barrier is the fastest method** for GTEP LPs — same as CPLEX, opposite of HiGHS.
   30 IPM iterations vs 12,826 simplex iterations.

2. **PostScaling=0 gives the best wall time** (3.2s) but introduces minor objective
   drift (~1e-5 relative). Acceptable for most GTEP applications.

3. **4 threads** is optimal for barrier. Simplex doesn't benefit from threading.

4. **Presolve is critical** — 2x slowdown without it, similar to all other solvers.

5. **Tighter IPM tolerance (1e-10) paradoxically helps** — cleaner crossover.

6. **Options that hurt**: `Dualization=on` (2-4x slower), `NumericFocus=1` at top
   level (2x slower), `Presolve=off` (2x slower).

7. **MindOpt startup overhead** of ~2-3s per process is unavoidable (license check).
   In SDDP with many resolves this is amortized; for single-solve workflows it
   inflates wall time.

## Comparison: All Solvers (best configs, same LP)

| Solver | Best Config | Median (ms) | Solve Time | Ratio |
|--------|-------------|-------------|------------|-------|
| CPLEX 22.1.1 | barrier + opportunistic + 4t | 733 | ~0.5s | 1.0x |
| MindOpt 2.3.0 | barrier + PostScaling=0 + 4t | 3,306 | ~0.9s | 4.5x |
| HiGHS 1.13.1 | simplex + parallel=on | 4,840 | ~4.4s | 6.6x |

MindOpt sits between CPLEX and HiGHS. Its actual solve time (0.9s) is competitive
with CPLEX (0.5s), but the ~2.3s startup overhead inflates overall wall time.

## Recommended MindOpt Settings

These are set in the gtopt MindOpt plugin `optimal_options()` and `~/.gtopt.conf`:

```ini
[solver.mindopt]
algorithm = barrier
threads = 4
presolve = true
scaling = automatic
max-fallbacks = 2
```

The plugin's `apply_options()` maps these to MindOpt parameters:
- `Method=2` (barrier/IPM)
- `NumThreads=4`
- `Presolve=1`
- Output suppressed by default (`OutputFlag=0`, `LogToConsole=0`)
