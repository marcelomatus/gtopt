# gtopt_ots_ieee118

IEEE 118-bus OTS infrastructure smoke test for issue #509.

## What it does

`gtopt_ots_ieee118.py` reads `cases/ieee_118b/ieee_118b.json`, generates
two OTS-compatible variants (baseline + LineCommitment on every line in
LP-relax mode), runs `gtopt` on each, and reports the savings ratio
against the published Fisher-O'Neill-Ferris 2008 golden value (25 %).

## Quick start

```bash
GTOPT_BIN=$PWD/build/standalone/gtopt \
    python scripts/gtopt_ots_ieee118/gtopt_ots_ieee118.py
```

CLI flags:

- `--gtopt PATH`     — explicit gtopt binary path (overrides `GTOPT_BIN`).
- `--tmp PATH`       — workspace dir for variants + outputs.  Defaults
  to a fresh `mktemp` dir.
- `--keep`           — keep the workspace after the run (for debugging).
- `--line-limit-scale FLOAT` — uniform thermal-limit scale.  Default
  `0.02` (~ 200 MW per line, Fisher 2008 congestion regime).

## pytest entry

`tests/test_ots_ieee118_smoke.py` runs the script end-to-end and
asserts monotonicity (`obj_ots ≤ obj_baseline`).  Marked
`integration`, so excluded by `pytest -m 'not integration'`.

The test auto-skips if `gtopt` cannot be located or the IEEE 118-bus
case JSON is missing.  Prefers the in-tree `build/standalone/gtopt`
over a possibly-outdated `$PATH` lookup to avoid the v0 schema-
mismatch (an older binary that doesn't know about
`line_commitment_array` will fail to parse the OTS variant).

## Why no savings (yet)

The LP-relax of OTS rarely produces non-zero savings on this case
because (a) fractional `u_l ∈ [0, 1]` half-opens lines without
recovering Fisher's integer-OTS topological benefit, and (b) the
stored `ieee_118b` case ships with a uniform 9 900 MW thermal limit
that's ~2× the total system demand — no congestion to relieve.

See the script docstring for what a meaningful 25 % reproduction
would need: full MIP solve (drop `relax: true`) on a backend with
MIP support (CPLEX / HiGHS), plus realistic per-line thermal limits
in the source JSON.

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
