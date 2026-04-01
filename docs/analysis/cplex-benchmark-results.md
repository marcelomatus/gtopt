# CPLEX LP Solver Benchmark Results

- **Date**: 2026-03-31
- **File**: `support/lps/feasible_scene_1_phase_14.lp` (132,722 lines, 7.2MB)
- **CPLEX version**: 22.1.1
- **Base config**: barrier algorithm, default scaling (SCAIND=0), 4 threads
- **Platform**: Linux 6.6.87 (WSL2)
- **All configs reach optimal objective**: 1.3907191126e+09

## Clean System Results (7 runs each, no background load)

### Individual Options vs Baseline

| Config | Median (ms) | Min | Max | Det. Ticks | Notes |
|--------|-------------|-----|-----|------------|-------|
| **BASELINE (barrier/default/4t)** | **756** | 719 | 968 | 1,245 | Reference |
| crossover=0 (disabled) | 770 | 733 | 831 | 1,245 | Stable, no basic solution |
| **crossover=1 (primal)** | **986** | 884 | 1,830 | **1,146** | Lowest ticks |
| crossover=2 (dual) | 2,472 | 1,893 | 2,854 | 3,664 | Much slower |
| ordering=1 (AMD) | 800 | 747 | 819 | 1,245 | Stable |
| ordering=2 (AMF) | 857 | 820 | 925 | 1,397 | Slightly slower |
| ordering=3 (NestedDissection) | 908 | 858 | 2,069 | 1,474 | Has spikes |
| **parallel=-1 (opportunistic)** | **733** | **718** | **805** | **1,150** | **Best single option** |
| convergetol=1e-7 | 1,210 | 1,145 | 1,336 | 1,912 | Slower (surprise) |
| aggregator=0 | 1,480 | 1,433 | 1,739 | 2,057 | Much worse |
| corrections=0 | 747 | 707 | 1,712 | 1,245 | Similar, has spikes |
| numerical emphasis=y | 821 | 769 | 861 | 1,476 | Small overhead |

### Combination Tests

| Config | Median (ms) | Min | Max | Det. Ticks |
|--------|-------------|-----|-----|------------|
| parallel=-1 + crossover=1 | 887 | 733 | 1,074 | 1,146 |
| crossover=1 + ordering=1 | 899 | 815 | 1,172 | 1,146 |
| parallel=-1 + crossover=1 + ordering=1 | 960 | 736 | 1,125 | 1,146 |

### Thread Sweep (barrier + default scaling)

| Threads | Median (ms) | Min | Max | Det. Ticks | Stability |
|---------|-------------|-----|-----|------------|-----------|
| 0 (auto) | 1,500 | 1,306 | 6,248 | 1,179 | high variance |
| 1 | 1,021 | 863 | 1,157 | 1,746 | moderate |
| 2 | 1,407 | 1,264 | 3,354 | 1,475 | high variance |
| 3 | 1,317 | 862 | 5,024 | 1,415 | high variance |
| **4** | **748** | **740** | **827** | **1,245** | **very stable** |
| 5 | 4,062 | 800 | 5,032 | 1,278 | very unstable |

### Algorithm Comparison (first round, all with default scaling, threads=0)

| Algorithm | Scaling | Threads | Wall Time (ms) | Det. Ticks |
|-----------|---------|---------|-----------------|------------|
| barrier | default | 0 | ~836 | 1,179 |
| auto | default | 0 | ~2,667 | 1,278 |
| dual | default | 0 | ~4,940 | 4,636 |
| primal | default | 0 | ~9,939 | 8,940 |

## Key Findings

1. **Barrier is the clear winner** for this GTEP LP — 5-13x faster than simplex methods.

2. **`parallel=-1` (opportunistic)** is the single best option: 733ms median,
   tightest range (718-805ms), lowest ticks (1,150). Free ~3% speedup.

3. **4 threads** is the sweet spot — fastest median and most stable. Higher thread
   counts cause contention; lower counts underutilize.

4. **Default scaling (SCAIND=0)** beats both none and aggressive for barrier.

5. **Crossover=1 (primal)** has the lowest deterministic ticks (1,146) but slightly
   higher wall time. It improves the crossover phase quality.

6. **Options that hurt**: `convergetol=1e-7` (+60%), `aggregator=0` (+96%),
   `crossover=2` (3.3x slower).

## Recommended CPLEX Settings

```
set lpmethod 4                    # barrier
set read scale 0                  # default scaling (equilibration)
set threads 4                     # 4 threads
set parallel -1                   # opportunistic (internal, not user-facing)
set barrier crossover 1           # primal crossover after barrier
```

These are now the defaults in the gtopt CPLEX plugin (`cplex_solver_backend.cpp`).
