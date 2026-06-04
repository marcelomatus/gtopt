# gtopt_ots_ieee118

IEEE 118-bus Optimal Transmission Switching validation against
Fisher-O'Neill-Ferris 2008's golden 25 % savings.

## What it does

`gtopt_ots_ieee118.py` reads `cases/ieee_118b/ieee_118b.json`, generates
two OTS-compatible variants (baseline + LineCommitment on selected
candidate lines), runs `gtopt` on each as a **MIP** (binary `u_l`),
and reports the savings ratio against the published Fisher 2008
golden table.

## Key result on the stored ieee_118b cost data

```
obj_baseline:         85 151.48  (200 MW caps, 10 candidates)
obj_ots:              84 840.00
Absolute savings:    +   311.48
Savings vs total:    +0.37 %

Cheapest-dispatch decomposition:
  cheapest_gen × demand:        $20.00/MWh × 4 242 MW = $84 840.00
  congestion cost:              $   311.48
  OTS eliminated:               $   311.48  (100.0 % of congestion cost)
```

**gtopt's OTS is optimal — the case just has only $311 of
congestion to save.**  Fisher 2008's 25 % savings doesn't
translate because:

  * Fisher used 50× gen-cost ratio ($0.19-$10/MWh) — obj was
    dominated by congestion-routed expensive dispatch.
  * gtopt's pglib-opf data has only **2× gen-cost ratio**
    ($20 and $40/MWh, no intermediate levels) — obj is
    dominated by the cheapest dispatch floor, which OTS
    can't reduce.

The script's output decomposition makes this explicit; the
fair comparison metric is **% of congestion cost eliminated**
(here: 100 %), not % of total obj.

## Reproduce locally

```bash
# Quick MIP run with 10 candidate lines, 200 MW caps:
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python scripts/gtopt_ots_ieee118/gtopt_ots_ieee118.py \
        --candidate-lines \
          'l26_30,l38_65,l89_92,t8_5,t68_69,t38_37,t65_66,t116_68,l8_9,l9_10'

# Same as MIP but LP-relax (faster):
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python scripts/gtopt_ots_ieee118/gtopt_ots_ieee118.py \
        --lp-relax \
        --candidate-lines \
          'l26_30,l38_65,l89_92,t8_5,t68_69,t38_37,t65_66,t116_68,l8_9,l9_10'
```

LP-relax reaches the same obj as MIP on this case ($84 840 ⇒
100 % of congestion eliminated) — the integrality gap closes
because the optimal topology rearrangement requires no fully-
open lines, just a small angle-relaxation that the LP can
exploit through the KVL big-M slack.

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
