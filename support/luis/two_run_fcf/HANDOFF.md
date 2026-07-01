# Handoff → NCP-side agent: gtopt's own FCF for run #2

**Deliverable:** `boundary_cuts_gtopt_fcf.csv` — gtopt's OWN future-cost cuts,
computed by a self-contained multi-week SDDP run (run #1). It replaces the
watervcp single-cut proxy so **no PSR solver output feeds gtopt** anymore.

Also copied to `support/luis/ncp_pdd_22jun/boundary_cuts_gtopt_fcf.csv` (next to
the existing watervcp-proxy `boundary_cuts.csv`, so you can A/B them).

## Use it in run #2

Point the NCP monolithic run at this file instead of the watervcp proxy:

```
gtopt support/luis/ncp_pdd_22jun/ncp_pdd_22jun.json --solver cplex \
  --set monolithic_options.boundary_cuts_file=<abs path>/boundary_cuts_gtopt_fcf.csv \
  -d <out> -f csv
```

(Or set `monolithic_options.boundary_cuts_file` in the JSON. Use an ABSOLUTE
path — the loader resolves it as given.)

## Your NCP-side removals (make run #2 fully self-contained)

These currently inject PSR *outputs* into the NCP model; drop them so only the
gtopt FCF prices the terminal water:

1. **watervcp-derived per-release hydro `gcost`** — `dat_loader.py:396`.
2. **watervcp → boundary cut** — the single-cut proxy write path (superseded by
   this file).
3. **volfincp → reservoir `efin`** — `dat_loader.py:420-435` (PSR end volumes;
   let the FCF price the final volume instead).

## Interface contract (locked — must match on both sides)

- **Columns** = `Reservoir:<name>` where `<name>` = the reservoir's `name`
  (here `rs_<plant>`), matched by NAME by the loader
  (`source/sddp_boundary_cuts.cpp`). This file carries all **44** NCP-side
  reservoirs; the extractor emitted exactly the NCP canonical set. If you change
  the NCP reservoir set, re-extract (the SDDP-side reservoir set must be a
  superset by name).
- **Sign/units** = PHYSICAL space, `value = -water_value` (`= -val*scale_objective`
  from the parquet). The loader negates (`row = -coeff`) and applies the NCP
  run's own `÷scale_objective` via `compose_physical`. `scale_objective=1` here.
- **Multi-cut**: 2 rows (`iteration` 1,2) = the terminal cut family.

## What it is (run #1)

- 12-week deterministic SDDP built from the one-week NCP JSON via
  `build_multiweek.py` (weekly-mean aggregation to 1 block/week — PSR's
  `NUMERO DE BLOQUES DEMANDA 1` — replicated across 12 weekly stages, reservoirs
  coupled as state variables). PSR's 25 stochastic series aren't shipped
  (`hinflw.dat` is an empty header), so inflow is the deterministic replicated
  caudcp forecast.
- Terminal cut = **phase 1** (FCF entering week 2 = end-of-week-1), extracted
  with `extract_cuts.py <cut.parquet> <sddp.json> <ncp.json> <out.csv> 1`.
- gtopt's own water values are broadly PSR-comparable: `rs_CHX-H1=0` (PSR≈0),
  `rs_AGU-H1≈208k` (PSR 186k); some reservoirs differ (deterministic vs
  stochastic). See `../../.claude/.../project_sddp2gtopt_psr_io` memory.

## Expected result (I already validated end-to-end)

Feeding this FCF to run #2 gives day-1 LMP mean **90.9, corr +0.70** — the SAME
as the watervcp proxy (91.0). So the FCF swap is a **methodological** win (self-
contained) but does **not** close the day-1 gap vs PSR (141). Phase 0 showed the
day-1 residual is **thermal commitment**, not the FCF — that's the separate lever
to pursue. Compare your run with `cmp_day1.py <out_dir>`.

## Regenerate

```
python build_multiweek.py support/luis/ncp_pdd_22jun/ncp_pdd_22jun.json luis_12wk.json 12
gtopt luis_12wk.json --solver cplex --set sddp_options.max_iterations=15 -d luis_12wk_out
python extract_cuts.py luis_12wk_out/cuts/sddp_cuts.parquet luis_12wk.json \
    support/luis/ncp_pdd_22jun/ncp_pdd_22jun.json boundary_cuts_gtopt_fcf.csv 1
```

GTOPT_BIN = `build-release/standalone/gtopt` (CPLEX). Do NOT use
`~/.local/bin/gtopt` unless its RPATH/libspdlog fix is in place.
