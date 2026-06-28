# plp2gtopt output directory

This directory contains the output of a `plp2gtopt` run.

| File / dir | Purpose |
|---|---|
| `<case_stem>.json` | Converted gtopt planning JSON — the primary output. |
| `<case_stem>/` | Per-class input-data directory (Generator/, Line/, Demand/, Reservoir/, Battery/, Waterway/, …) with Parquet time-series. |
| `logs/` | Per-stage / per-block converter logs. |
| `boundary_cuts.csv` | Future-cost-function cuts emitted alongside the planning JSON. |
| `solvers/` | Solver parameter files (e.g. `cplex.prm`) copied from defaults. |
| `plp2gtopt_state.json` | **State snapshot** — full CLI invocation + every parsed option, written at the START of the run.  See *Reproducing this run* below. |
| `README.md` | This file. |

## Regenerating `gtopt_case_2y`

This case is built with `plp2gtopt` from the PLP source
`support/plp_2_years` (a 2-year hydrothermal case: 52 weekly stages,
hydrologies 51–68, 112 batteries). From the repository root:

```bash
plp2gtopt -i support/plp_2_years -o cases/gtopt_case_2y \
    -y 51,52 -a all --method sddp
```

| Flag | Meaning |
|---|---|
| `-i support/plp_2_years` | PLP `.dat[.xz]` source directory. |
| `-o cases/gtopt_case_2y` | Output dir; its basename becomes the case stem (`gtopt_case_2y.json`). |
| `-y 51,52` | `--hydrologies`: pick 2 of the dataset's 1-based hydrology indices (available 51–68) → **2 scenarios**. Use `-y all` for all 18. |
| `-a all` | `--num-apertures`: keep every SDDP aperture. |
| `--method sddp` | Planning method (note the plp2gtopt default is `cascade`). |

Gotchas:

- `-s` / `--last-stage` is a **stage count**, not a scenario count; it
  defaults to `-1` (all 52 stages). Do **not** pass `-s 1` — that
  truncates the horizon to a single stage.
- Parquet input is written in **long** layout
  (`[<index cols>, uid, value]`), the default and the only shape gtopt
  reads.
- Run `plp2gtopt --help` for the full flag list.

## Reproducing this run

Every `plp2gtopt` invocation writes a **state snapshot** to
`plp2gtopt_state.json` at the START of the run, capturing the exact
command-line + every parsed option (after defaults).

### Re-run with the same options

```bash
plp2gtopt --from-state plp2gtopt_state.json
```

The snapshot file is self-contained; the `--from-state` flag rebuilds
the argparse namespace from it and dispatches the converter as if you
had typed the original command.

### Override individual options on reload

```bash
plp2gtopt --from-state plp2gtopt_state.json --some-flag NEW_VALUE
```

CLI flags passed alongside `--from-state` override the values stored
in the snapshot (the snapshot is a baseline; the current CLI line
wins).

### Inspect the snapshot

```bash
jq '.args' plp2gtopt_state.json            # all parsed options
jq '._meta' plp2gtopt_state.json           # tool / version / timestamp / argv / cwd
jq '.extra' plp2gtopt_state.json           # parsed options dict the converter consumes
```
