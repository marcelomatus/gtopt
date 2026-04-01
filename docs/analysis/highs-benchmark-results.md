# HiGHS LP Solver Benchmark Results

- **Date**: 2026-03-31
- **File**: `support/lps/feasible_scene_1_phase_14.lp` (132,722 lines, 7.2MB)
- **HiGHS version**: 1.13.1 (git hash: 3be639f)
- **Platform**: Linux 6.6.87 (WSL2)
- **All configs reach optimal objective**: 1.3907191126e+09

## Results (5 runs each, clean system)

| Config | Median (ms) | Min | Max | Solver Time (median) |
|--------|-------------|-----|-----|----------------------|
| **Simplex, parallel=on** | **4,840** | 3,900 | 5,026 | 4.39s |
| Choose (auto) | 5,382 | 4,238 | 7,030 | 5.00s |
| Simplex (auto) | 5,832 | 4,704 | 6,683 | 5.40s |
| IPM, parallel=on | 6,870 | 6,288 | 9,539 | 6.45s |
| IPM (barrier) | 9,119 | 7,111 | 10,218 | 8.73s |
| IPM, presolve=off | 19,706 | 18,059 | 20,487 | 18.72s |

## Key Findings

1. **HiGHS simplex is faster than HiGHS IPM** on this LP — opposite of CPLEX where
   barrier dominates. The `choose` mode correctly selects simplex.

2. **`parallel=on` helps both simplex and IPM** — ~17% improvement for simplex.

3. **Presolve is critical** — disabling it makes IPM 2.5x slower (19s vs 8s).

4. **HiGHS is ~5-7x slower than CPLEX** on this LP:
   - Best HiGHS: simplex+parallel = 4,840ms
   - Best CPLEX: barrier+opportunistic = 733ms

5. **Recommendation for HiGHS**: let the solver choose (`choose` or `simplex`),
   do not force IPM/barrier. The default `LPAlgo::barrier` in SolverOptions is
   CPLEX-optimal but not HiGHS-optimal.

## Comparison: CPLEX vs HiGHS (best configs)

| Solver | Best Config | Median (ms) | Ratio |
|--------|-------------|-------------|-------|
| CPLEX | barrier + opportunistic + 4t | 733 | 1.0x |
| HiGHS | simplex + parallel=on | 4,840 | 6.6x |
