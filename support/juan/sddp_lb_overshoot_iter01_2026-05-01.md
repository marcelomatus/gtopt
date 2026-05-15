# SDDP LB-overshoot localisation — empirical iter 0 vs iter 1 cut data

**Date:** 2026-05-01
**Run:** `support/juan/gtopt_iplp/gtopt_iplp.json` with `-T /tmp/juan_cuttrace.log`
**Binary:** built from commit `d923cbbb` (cut-math TRACE actually emitting)
**Trace file preserved:** `/tmp/juan_cuttrace_iter01.log` (10.6 MB),
`/tmp/juan_stdout_iter01.log` (90 kB)

## Executive summary

The LB-overshoot bug is **not** in the cut formula — both
`build_benders_cut_physical` overloads were audited and produce
arithmetically identical SparseRow structures.  Empirical per-link
data from iter 0 → iter 1 backward passes shows:

1. **Cut RHS (`row.lowb`) max compounds 8.86 ×** per iteration
   ($1.16e8 → $1.02e9).
2. The cut RHS is **dominated by `obj_phys`** (the aperture clone's
   `get_obj_value()`), not by the `Σ rc·v̂` coupling.
3. **`v_hat_phys` is essentially stationary** — the top-ratio links'
   trial values move only 1.04 × – 1.09 × across iterations.
4. **`rc_phys` grows asymmetrically** per link: a cluster of 11 links
   compound at 9 × – 12 × per iter; ~30 links at 2 × – 3 ×; ~30 links at
   1 × – 1.5 ×; 7 links exactly 1.000 × (stationary).

The 8.86 × cut-RHS growth matches the master's LB compounding from
$1.4 M (iter 0) to $1.10 B (iter 1).  The α-bound forced by these
cuts overshoots the true future-cost surface, violating the SDDP
optimality-cut underestimator property (already flagged by the run-time
SIGNIFICANT-negative-gap warning).

## Iter-end summary lines (from juan stdout)

```
[00:17:28.054] SDDP Iter [i0]: done in 370.579s (fwd 47.27s + bwd 323.31s)
               UB=163,861,652.94  LB=1,424,815.64  gap=99.13 %
               cuts=300  infeas_cuts=10
[00:24:18.325] SDDP Iter [i1]: SIGNIFICANT negative gap = -6.21
               UB=153,195,345.86  LB=1,104,568,559.98  ← LB > UB
               gap=-621 %  cuts=300  infeas_cuts=10
```

LB ratio iter1/iter0 = $1.10 B / $1.42 M = **775 ×**.

## Aggregate cut RHS per iter (from `build_benders_cut_physical[ovld2]`
trace lines)

```
iter 0: 4 800 cut summaries
        max row.lowb = 1.16e+08 (obj_phys = 1.155e+08, n_links = 8)
        median       = 8.56e+07
iter 1: 3 708 cut summaries
        max row.lowb = 1.02e+09 (obj_phys = 1.024e+09, n_links = 8)
        median       = 5.62e+08

max row.lowb ratio iter1/iter0 = 8.86 ×
```

`row.lowb ≈ obj_phys` for the dominant cuts, confirming the cut RHS
is set by the aperture clone's objective value, not by the rc·v̂
correction term.

## Per-link rc_phys distribution (mean |rc| ratio iter1/iter0)

100 links seen in both iters.  Histogram of ratios:

```
[ 0.5,   1.0):   2    rc_phys shrank (numerical noise)
[ 1.0,   1.5):  23    well-conditioned (last-phase couplings)
[ 1.5,   2.0):  10
[ 2.0,   2.5):  11
[ 2.5,   3.0):  10
[ 3.0,   4.0):   5
[ 4.0,   6.0):   4
[ 6.0,   8.0):   2
[ 8.0,  10.5):   5    ★ "10× cluster" matching LB compounding
[10.5, 100.0):   2    extreme (12.6 ×, 12.5 ×)

Median ratio: 2.082 ×
Mean ratio:   3.012 ×
```

## Top 11 links in the 10× cluster (mean |rc_phys| ratio ≥ 9.4 ×)

```
ratio  ovld   src    dep    mean|rc|@i0  mean|rc|@i1   n0   n1
12.58  ovld2 44808  44798  1.825e-03    2.295e-02    174  252
12.50  ovld2 44808  44928  4.227e-03    5.283e-02     90   90
10.04  ovld2 45498  45488  1.019e-02    1.022e-01    442  450
10.00  ovld2 44730  44841  3.579e+00    3.579e+01     66   66
10.00  ovld2 44730  44720  2.189e+00    2.189e+01    102  102
 9.92  ovld2 44646  44636  1.185e+02    1.176e+03    192  192
 9.78  ovld2 44896  44886  3.307e+01    3.234e+02    642  642
 9.65  ovld2 45456  45446  2.853e+00    2.752e+01    516  528
 9.43  ovld2 44851  44841  1.053e+01    9.929e+01    582  582
 9.34  ovld2 44787  44907  1.459e-01    1.363e+00     78   78
```

All ten are ovld2 (aperture path, `clone.get_col_cost()`).
The bcut fallback (ovld1, state_var-based) **did not fire at all** in
iter 0 or iter 1 of this run — every backward step accepted the
aperture cut.

## Top-link rc / v̂ ratio comparison

For the four highest-ratio links, mean |rc_phys| grew dramatically
while mean |v̂_phys| stayed essentially put:

```
link 45498 → 45488:  rc 10.04 ×    v̂  1.04 ×
link 45456 → 45446:  rc  9.65 ×    v̂  1.09 ×
link 45136 → 45126:  rc  5.70 ×    v̂  1.38 ×
link 45115 → 45105:  rc  3.16 ×    v̂  0.94 ×  (shrunk)
```

Confirms `v_hat_phys` is not the driver — the master's optimal state
trial values are stable across iterations.

## "Stationary" cluster (ratio = 1.000 ×)

7 links have rc_phys exactly identical between iter 0 and iter 1.
Their dep_col values (45604, 45581, 45536, 45514, 45491, 45469, 45447)
are all in the high-col range — these are the **last-phase** state
variables.  No future-cost cuts couple to them, so `clone.get_col_cost`
returns the same physical $/unit each iter.  This is a useful control:
the cut machinery itself is stable when no future cuts inflate
`obj_phys`.

## Mechanism — α-compounding in the aperture clone

The aperture clone `phase_li.clone(CloneKind::shallow)` carries the
master's accumulated optimality cuts (rows already added to the base
LP).  After iter 0:

- Master holds 300 + 10 cuts forcing `α ≥ 1.16e8` for some scenes.
- Iter 1 forward pass solves master with these cuts → forward-pass
  α is at the cut floor.
- Iter 1 backward pass aperture clone has the same cuts → its
  `get_obj_value()` returns first-stage cost + α (which is now ≈ 1e9).
- `build_benders_cut_physical[ovld2]` writes
  `row.lowb = obj_phys + Σ -rc·v̂_phys ≈ 1e9`.
- New cut goes into the master with RHS ≈ 1e9 → master's LB solve
  finds α at the new floor ≈ 1e9.
- Iter 2 will see α ≈ 1e10, producing 1e10 cuts, etc.

This is the classic Benders cut-overshoot loop, not a simple LP-solver
numerical issue.  The 10× compounding factor matches the per-iter
ratio of `obj_phys` (the master's cut-induced α floor at backward-pass
time).

## What this rules out / confirms

| Hypothesis | Verdict |
|---|---|
| Cut formula has a sign / scale error in benders_cut.cpp | **Rules out** — paths audited, both correct |
| col_scale=10 + ruiz numerical asymmetry | **Partial** — variable rc growth, but not col_scale-bucketed |
| α-cut compounding loop driven by aperture clone | **Confirms** — obj_phys = 8.86 × per iter, dominates row.lowb |
| `v_hat_phys` from forward pass is wrong | **Rules out** — v̂ stable across iters |
| Bcut fallback (ovld1) is the problem | **Rules out** — never fires in iter 0/1 |

## Suggested next investigations (not done)

1. **Look at the cuts that were installed at iter 0 — are they valid
   underestimators of the true future-cost surface?**  If iter 0 cuts
   already overshoot the true optimum, every subsequent iter
   compounds the overshoot.  Pick one (scene, phase) cut from iter 0,
   evaluate its LHS at the iter 1 forward-pass state value, compare to
   the iter 1 aperture clone's `get_obj_value()` at the same state.
   A valid cut should satisfy LHS ≤ obj_phys; if LHS ≫ obj_phys, the
   cut overshoots.
2. **Check whether the aperture clone receives the master's cuts
   correctly.**  `phase_li.clone(CloneKind::shallow)` semantics: does
   it copy cut rows + their col_scaled coefficients, or only the base
   LP without later-added cuts?  If the latter, iter 1 apertures are
   solving the iter-0 LP without iter-0's cuts → α can drift up
   freely.
3. **Disable cut sharing entirely** (`sddp_options.cut_sharing = none`
   per the [cut-sharing-unsafe](feedback_cut_sharing_unsafe.md)
   feedback).  If the LB still compounds, the bug is in single-scene
   cut accumulation, not in cross-scene broadcast.
4. **Compare to the `aperture_use_manual_clone=true` path** (commit
   f541556a / merge 27f4a927).  If manual clone does not compound,
   the standard `phase_li.clone()` path is the bug surface.

## Reproducibility

Run with the build at commit `d923cbbb` (or later — once the SPDLOG
fix is on master):

```bash
cd /home/marce/git/gtopt/support/juan
/home/marce/git/gtopt/build/standalone/gtopt \
  -T /tmp/juan_cuttrace.log gtopt_iplp/gtopt_iplp.json
```

After ~13 minutes (iter 0 + iter 1 complete), run:

```bash
python3 /tmp/analyze_cut_traces.py
```

The script prints the link-level ratio table and histogram shown
above.

## Files

- **Analysis script:** `/tmp/analyze_cut_traces.py`
- **Trace log iter 0+1:** `/tmp/juan_cuttrace_iter01.log` (10.6 MB)
- **Stdout log iter 0+1:** `/tmp/juan_stdout_iter01.log` (90 kB)
