# SDDP aperture warm-start on full-network LPs — empirical analysis

**Date:** 2026-06-08
**Case:** juan IPLP (`~/tmp/juan-run`), build-release (`-O3`), CPLEX backend
**Branch:** `feat/two-tier-workpool`
**Verdict:** **Use cold start for the full-network / plain-SDDP regime.** SDDP
aperture warm-start is net-negative on big cut-laden LPs, and **no CPLEX
warm-start parameter (algorithm, `AdvInd`, dual pricing) overcomes it.** Warm-start
only pays off on the small-LP cascade levels (uninodal). _Do not re-run this
parameter sweep — it is decided._

## What aperture warm-start does

When the SDDP backward pass solves a chunk of apertures that differ only in
**column bounds**, the opt-in `sddp_options.aperture_warm_start=true` reuses the
previous aperture's optimal simplex basis instead of refactoring from scratch
(`SolverOptions::advanced_basis`, CPLEX `AdvInd=1`). Each backward chunk logs, at
debug level (requires `-v`):

```
SDDP Aperture warm-start [s<scene> p<phase>]: 1 cold X.Xms/avg + N warm Y.Yms/avg (warm/cold=Z)
```

`Z = warm/cold`. **`Z > 1` means a warm-started solve is _slower_ than a cold
one** — the opposite of the intended effect.

## Result 1 — cascade (mixed LP sizes): warm helps only on small LPs

Per-level split of the dual cascade run (`output_dual`):

| level         | LP size  | warm/cold (mean) | warm/cold (time-weighted) |
|---------------|----------|------------------|---------------------------|
| uninodal      | ~40 ms   | **0.50**         | 0.55                      |
| transport     | ~840 ms  | 0.97             | 1.085                     |
| full_network  | ~2030 ms | 1.25             | 0.887                     |

Whole-run aggregate: per-tile mean **0.811** but **time-weighted 1.034** — i.e.
the typical (cheap) solve is ~19 % faster warm, yet in total wall-clock warm-start
is ~3 % *slower*, because the few expensive full-network solves dominate the clock
and warm-start does not help them.

## Result 2 — plain SDDP recovery (every cell a big LP): the clean test

To isolate the full-network regime, we recovered the dual run's **12 800
converged cuts** and re-solved as a single SDDP pass (`--set method=sddp
--recover --set sddp_options.cuts_input_file=output_dual/cuts/sddp_cuts.parquet`),
so every cell is a full-network LP already loaded with cuts. Settled values:

| config                          | chunks | cold (ms) | warm (ms) | warm/cold |
|---------------------------------|--------|-----------|-----------|-----------|
| dual, `AdvInd=1`                | 3 414  | 1460      | 1764      | **1.208** |
| primal, `AdvInd=1`              | 1 987  | 1491      | 1842      | **1.235** |
| dual, `AdvInd=2`                | 821    | 1284      | 1712      | **1.333** |
| dual, `AdvInd=1`, `DGradient=2` | 972    | 932       | 1236      | **1.326** |

Warm is **1.2–1.4× cold in every configuration.** Cold start wins.

### Apples-to-apples (same first 272 chunks, iteration-controlled)

Because later chunks carry more cuts and solve slower, absolute ms are only
comparable across configs on the **same** chunk window:

| config (first 272 chunks)       | cold (ms) | warm (ms) | warm/cold |
|---------------------------------|-----------|-----------|-----------|
| dual, `AdvInd=1`                | 949       | 1307      | 1.378     |
| primal, `AdvInd=1`              | 1336      | 1912      | 1.432     |
| dual, `AdvInd=2`                | 1574      | 2163      | 1.374     |
| dual, `AdvInd=1`, `DGradient=2` | 1020      | 1448      | 1.419     |

The warm/cold ratio is statistically flat (~1.37–1.43) across all configs — none
of the parameters moves it.

## Result 3 — CPLEX parameter sweep (`solvers/cplex_warmstart.prm`)

- **`CPXPARAM_Advance` (AdvInd) ∈ {0, 1, 2} — whole domain spanned.** `0` = cold
  baseline, `1` = warm (original), `2` = warm + presolve-crush. **`AdvInd=2`
  *hurts*** (~1.33–1.39): re-running presolve and crushing the basis through it
  costs more per aperture than it saves, because each aperture differs only by
  many small bound changes. Presolve suppression was *not* the bottleneck.
- **primal vs dual:** dual simplex is the theoretically correct warm-start method
  for bound changes (bound changes preserve dual feasibility). Empirically
  primal ≈ dual; both are net-negative. (Early small samples made primal look
  worse — 1.49 at 319 chunks — but it settled to 1.24; see methodology.)
- **`CPXPARAM_Simplex_DGradient=2`** (steepest-edge dual pricing): **does not
  change the warm/cold ratio** (~1.33).

**Root cause:** basis staleness after large column-bound changes. The inherited
optimal basis is far from optimal for the next aperture's bounds, and re-pivoting
back costs more than a cold refactor. No `AdvInd` / algorithm / pricing parameter
overcomes this for big cut-laden LPs.

## Methodology guardrails (so future measurements stay valid)

1. **Use the warm/cold *ratio*, not absolute ms across runs.** Both cold and warm
   are measured back-to-back within the same chunk at the same iteration, so the
   ratio is iteration- and machine-jitter-robust. Absolute ms are *not* comparable
   across runs with different chunk counts (later chunks have more cuts → slower).
2. **Beware early-sample bias.** The first backward iterations have the stalest
   inherited basis and show the worst ratios (~1.4–1.5); they settle to ~1.2 with
   more data. Do not conclude from < ~1000 chunks.
3. **Warm-start advantage shrinks as cuts accumulate** — another reason the cheap
   early-cascade levels look favourable and the late/full-network levels do not.

## Recommendation

- **Full-network / plain-SDDP runs:** leave `sddp_options.aperture_warm_start`
  **off** (the default) — i.e. cold start.
- **Keep warm-start only** for workloads dominated by small-LP cascade levels
  (e.g. uninodal), where it is a genuine ~2× win.
- `cplex_warmstart.prm` is orthogonal to this decision (it also serves the
  post-MIP `FIXEDMILP` dual-recovery pass); leave it at its committed
  `LPMethod=1, Advance=1`.

## Only unexplored lever

Cold **barrier** (`CPXPARAM_LPMethod=4`, which *cannot* warm-start) might lower
the *cold floor* itself on these big LPs. Untested. Everything about *warm*-start
is settled by the above.

## Reproduction

```bash
# recover the converged cut set as a single full-network SDDP pass
cd ~/tmp/juan-run
GTOPT_PLUGIN_DIR=$REPO/build-release/plugins nice -n 19 \
  $REPO/build-release/standalone/gtopt juan-run.json \
  --set method=sddp --recover \
  --set sddp_options.cuts_input_file=output_dual/cuts/sddp_cuts.parquet \
  --aperture-chunk-size 8 \
  --set sddp_options.aperture_warm_start=true \
  -v -d output_sddp_recover

# warm/cold aggregate from the debug log
grep 'Aperture warm-start' output_sddp_recover/logs/gtopt_1.log | awk '
{ if (match($0,/([0-9]+) cold ([0-9.]+)ms/,c) && match($0,/\+ ([0-9]+) warm ([0-9.]+)ms/,w)) {
    tc+=c[1]*c[2]; nc+=c[1]; tw+=w[1]*w[2]; nw+=w[1] } }
END{ printf "COLD %.0f ms  WARM %.0f ms  warm/cold=%.3f\n", tc/nc, tw/nw, (tw/nw)/(tc/nc) }'
```

The algorithm / `AdvInd` / `DGradient` knobs are flipped via
`solvers/cplex_warmstart.prm` (`CPXPARAM_LPMethod` 1=primal 2=dual;
`CPXPARAM_Advance` 1/2; `CPXPARAM_Simplex_DGradient` 2). The
`tools/run_juan.sh --algo {primal,dual}` helper sets `LPMethod`.
