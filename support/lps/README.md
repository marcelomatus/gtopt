# Reference LP Files

Reference LP models extracted from gtopt for solver benchmarking and testing.

## Files

| File | Lines | Description |
|------|-------|-------------|
| `feasible_scene_1_phase_14.lp` | 132,722 | Multi-bus GTEP model (scene 1, phase 14). Optimal objective: 1.3907191126e+09 |

## Usage

These files are used by the benchmark script to compare solver performance:

```bash
bash docs/analysis/lp-solver-benchmark.sh support/lps/feasible_scene_1_phase_14.lp
```

See `docs/analysis/cplex-benchmark-results.md` and
`docs/analysis/highs-benchmark-results.md` for results.
