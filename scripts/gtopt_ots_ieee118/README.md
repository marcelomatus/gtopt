# gtopt_ots_ieee118

IEEE 118-bus Optimal Transmission Switching validation against
Fisher-O'Neill-Ferris 2008's golden 25 % savings.

## What it does

`gtopt_ots_ieee118.py` reads `cases/ieee_118b/ieee_118b.json`, generates
two OTS-compatible variants (baseline + LineCommitment on selected
candidate lines), runs `gtopt` on each as a **MIP** (binary `u_l`),
and reports the savings ratio against the published Fisher 2008
golden table.

## Quick-start: reproduce ~3 % savings in ~5 minutes

```bash
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python scripts/gtopt_ots_ieee118/gtopt_ots_ieee118.py \
        --time-limit 600 --mip-gap 0.001 \
        --line-limit-scale 0.01 \
        --candidate-lines \
          'l26_30,l38_65,l89_92,t8_5,t68_69,t38_37,t65_66,t116_68,l8_9,l9_10,t30_17,l82_83,l110_111,l23_25,t65_68,l64_65,l25_27,l77_82,t81_68,t81_80'
```

Observed result on the current branch:

```
obj_baseline = 95,551  (200 MW caps × 20-line OTS candidate set)
obj_ots      = 92,586  ⇒ +3.10 % savings
```

The 20-line candidate set is the top-20 most-utilised lines at
the baseline solve.  Increasing to all 186 lines (drop
`--candidate-lines`) approaches Fisher's 25 % but the MIP is
genuinely hard (Fisher reported hours of solve time with
mid-2000s solvers).

## CLI flags

- `--gtopt PATH` — explicit gtopt binary path (overrides `GTOPT_BIN`).
- `--tmp PATH` — workspace dir for variants + outputs (default mktemp).
- `--keep` — keep the workspace after the run.
- `--line-limit-scale FLOAT` — uniform thermal-limit scale.  Default
  `0.02` (~200 MW per line, Fisher 2008 regime).
- `--lp-relax` — LP-relax `u_l` to `[0,1]`.  Fast smoke test that
  rarely produces non-zero savings.
- `--mip-gap FLOAT` — target relative MIP optimality gap.
- `--time-limit FLOAT` — per-solve wall-clock cap.
- `--candidate-lines NAME[,NAME...]` — restrict OTS candidates
  to a subset (default: all 186 lines).

## Key configuration details (caught during bring-up)

1. **Stage must be `chronological: true`.**  `LineCommitmentLP`
   silently skips on non-chronological stages.  The stored
   `ieee_118b` defaults to non-chronological; the script overrides
   `chronological = true` on every stage when building the OTS
   variant.

2. **Cascade method incompatible with OTS.**  Zou-Ahmed-Sun 2019
   showed Benders cuts on a MIP subproblem are unsound.  The
   stored case uses `method: "cascade"` with a multi-level
   model_options stack; the script flattens to
   `method: "monolithic"` with explicit `kirchhoff_mode:
   "node_angle"`.

3. **Stored line limits are unrealistically loose.**  All 186
   lines have `tmax_ab = 9 900 MW` (~2× system demand).  Tighten
   with `--line-limit-scale 0.02` (or smaller) to expose
   congestion.

## pytest entry

`tests/test_ots_ieee118_smoke.py` runs the script in MIP mode
on a small candidate subset and asserts monotonicity (`obj_ots ≤
obj_baseline`).  Marked `integration`, excluded by `pytest -m
'not integration'`.

## Golden reference

> Fisher, O'Neill, Ferris (2008).  "Optimal Transmission Switching."
> *IEEE Transactions on Power Systems* 23(3):1346–1355.

| Topology               | Cost $/h | Savings |
|------------------------|---------:|--------:|
| No switching           |    2 054 |  0.0 %  |
| line 153 only          |    1 925 |  6.3 %  |
| lines 132, 153         |    1 800 | 12.4 %  |
| lines 132, 136, 153    |    1 646 | 19.9 %  |
| 38 lines (MIP optimum) |    1 543 | 25.0 %  |
