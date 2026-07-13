# Task: reproduce + measure the 2026-04-12 warm-start experiment (uplift + uninodal + transport-reduced)

Repo: gtopt. `git pull` first (master is pushed concurrently from other worktrees →
fetch+rebase, never ff a dirty tree). Everything below is in the repo — you build
the case yourself; there are NO pre-made artifacts to reuse.

## Goal

On the CEN PLEXOS daily UC case **2026-04-12** (`support/plexos/pcp_2026-04-12/
DATOS20260412.zip.xz`, the clean reference where warm-start seeds are ACCEPTED),
run the full **A-vs-B** warm-start experiment and answer, by DETERMINISTIC TICKS:
does a reduced-network MIP-start seed give a NET MIP speedup over the cold baseline?
Test BOTH reduced models — **uninodal** (copper-plate) and **transport-reduced**.
Also measure the orthogonal **barrier-skip** on the LP relaxation.

Metric: **`GTOPT_SOLVE_EFFORT ... ticks=`** (deterministic, contention-proof), NOT
wall (noisy — several solves may share the host). Run heavy solves ONE AT A TIME
(a governor SIGKILLs at load >~50). Outputs under `~/tmp` (never `/tmp`). Build a
**-O3/Release** gtopt for timing (CIFast is fine only for correctness). Default
solver CPLEX. The 04-12 optimum obj ≈ 9.73e8.

## Pipeline

### 0. Binary
Build build_release (`-DCMAKE_BUILD_TYPE=Release`, g++-15, ccache, `-S all`) or use
`python tools/get_gtopt_binary.py`. Confirm the cplex plugin loads (`gtopt --solvers`;
NB `--solvers` can hang on the gurobi validator with no license — run with a
`timeout` guard).

### 1. Convert WITH the line uplift (the canonical run_mip_loop.sh config)
The lines PLEXOS over-dispatches are curated per date in
`support/plexos/per_case_lifts.json` (~38 lines for 2026-04-12). `--lift-line-caps`
demotes their hard EL=1 caps to SOFT caps: in the JSON a lifted line gets
`tmax_ab ≈ 2× tmax_normal_ab` + `overload_penalty` (was EL=1). This matches PLEXOS's
over-rating dispatch. Command (from `tools/run_mip_loop.sh` L73-80):
```
LIFTS=$(python3 -c "import json;print(','.join(json.load(open('support/plexos/per_case_lifts.json'))['2026-04-12']))")
cd scripts && python3 -m plexos2gtopt.main \
  ../support/plexos/pcp_2026-04-12/DATOS20260412.zip.xz --output-dir <OUT> \
  --nseg-losses 10 --nseg-tangent 6 --nseg-uniform 4 \
  --loss-error-pct 1.0 --loss-cost-eps 1.0 --loss-pwl-layout dynamic \
  --el0-lines strict --no-lift-lines "PMontt220->Chiloe110" \
  --water-value-factor "COLBUN:0.85,RALCO:0.80" --lift-line-caps "$LIFTS"
```
VERIFY the uplift landed: a lifted line (e.g. `Capricornio110->LaNegra110`) has
`tmax_ab > tmax_normal_ab` in `DATOS20260412.json`. (1 lift name per date may be
stale/absent — silently ignored; that's fine.)

### 2. Cold baseline A (the REFERENCE)
```
gtopt DATOS20260412.json --solver cplex --set monolithic_options.mip_start.enabled=false \
  --set solver_options.time_limit=1800 --set output_directory=out_cold
```
Record ticks / wall / obj. **Use crossover=auto (default) — do NOT put
`CPXPARAM_Barrier_Crossover=1` in the prm; it makes the cold MIP ~2.5× slower
(633s vs 247s).** ~247s / ~128k ticks expected. This 247s/128k is THE reference —
"faster than the slow crossover-primal run" is not a valid bar.

### 3. Seed generation — TWO variants (fold BOTH costs into the verdict)
The reduced solve produces the commitment; `build_full_seed.py` makes the DENSE
seed (enumerates committable-generators × blocks, absent → u=0; sparse seeds get
rejected). Seeds map by `(generator_uid, block_uid)` identity (reduction never
renames generators). **`--allow-oversupply` on the reduced solve AND the full MIP**
(relaxes the bus balance `=`→`≥`, gen ≥ demand, free disposal — removes over-supply
as an infeasibility source).

(a) **Uninodal (copper-plate)** — the seed that was ACCEPTED on 04-12:
```
gtopt DATOS20260412.json --no-mip --no-scale --solver cplex --allow-oversupply \
  --set model_options.use_single_bus=true --set solver_options.crossover=none \
  --set output_directory=out_uni -l uni
python support/plexos/build_full_seed.py out_uni DATOS20260412.json seed_uninodal.csv
```

(b) **Transport-reduced** (30% buses, KVL off, constraint lines protected):
```
cd scripts && python -m gtopt_reduce_network.main reduce <OUT>/DATOS20260412.json \
  --bus-ratio 0.30 --partition louvain-mincut --protect-constraint-lines \
  --transport-only --loss-mode uplift --loss-uplift-pct 3 -o <OUT>/reduced.json
# stage reduced.json NEXT TO the parquet inputs (its input_directory = OUT), then:
gtopt reduced.json --no-mip --no-scale --solver cplex --allow-oversupply \
  --set solver_options.crossover=none --set output_directory=out_red -l red
python support/plexos/build_full_seed.py out_red reduced.json seed_transport.csv
```
Reducer facts you can rely on: it reads `tmax_ab` (the UPLIFTED value) natively and
sums it into aggregated lines — so the reduced case keeps the uplifted caps.
`--protect-constraint-lines` re-emits lines referenced by user constraints / uc_*.pampl
/ line_commitment INTACT (only ~5 of the ~38 lifted lines are constraint-referenced;
the rest are aggregated but keep their uplifted `tmax_ab`). `--loss-mode uplift`
writes a per-DEMAND `lossfactor = pct/100` (bus-balance coefficient `-(1+lf)`) and
NEVER changes `lmax` — it's a factor, not a demand change. Try `--loss-uplift-pct 4`
too (more committed generation → seed more likely feasible in the full net).

### 4. Warm-start B — the config that was ACCEPTED on 04-12
For EACH seed (uninodal, transport):
```
gtopt DATOS20260412.json --solver cplex --allow-oversupply \
  --set monolithic_options.mip_start.enabled=true \
  --set monolithic_options.mip_start.seed_solution_file=seed_*.csv \
  --set monolithic_options.mip_start.inject.effort=solve_fixed \
  --set monolithic_options.mip_start.domain_rules.min_up_down=false \
  --set monolithic_options.mip_start.domain_rules.commitment_logic=false \
  --set monolithic_options.mip_start.domain_rules.peak_injection.enabled=false \
  --set solver_options.time_limit=1800 --set output_directory=out_ws_<variant>
```
CONFIRM acceptance in the log: **"N of N MIP starts provided solutions" +
"MIP start 'm1' defined initial solution with objective ..."** (NOT "No solution
found from N MIP starts"). Record ticks / wall / obj.

Effort/domain-rules gotchas (empirical, authoritative):
- `solve_fixed` ACCEPTS a feasible seed (fix ints, solve residual LP). `solve_mip`
  does NOT reliably repair an infeasible seed here (rejects instantly). Use
  `solve_fixed`.
- ALL THREE domain_rules OFF: the rules rewrite the commitment (min_up_down flipped
  ~61% of the seed on one run → made it infeasible), and `peak_injection` seeds
  battery+hydro/reservoir injection which can break feasibility ("no arreglar
  baterías ni embalses"). Keep the seed AS-IS.
- The seed source barely changes the commitment (~same ON count as the LP-relaxation
  rounding).

### 5. Barrier-skip (orthogonal — relaxation only)
```
gtopt DATOS20260412.json --no-mip --root-basis-cache base.dat   # prime (~47.7s, caches the vertex)
gtopt DATOS20260412.json --no-mip --root-basis-cache base.dat   # reuse (~0.71s, primal from cached vertex)
```
~67× on the RELAXATION. It does NOT help the full MIP: the MIP itself won't cache a
base (`CPXgetbase` post-`CPXmipopt` = interior point), and loading a base forces a
simplex method that either (primal) cripples B&C node re-solves or (dual) still hits
a garbage-cliff without a good incumbent → 13× slower. So barrier-skip = relaxation
lever only; keep it separate from the MIP verdict.

## Report (the deliverable)
A ticks table:
| variant | seed | reduced-solve ticks | MIP ticks | total ticks | tick-speedup vs cold | obj | accepted? |
Cold A (128k) as the denominator. Fold the reduced-solve cost into "total". State
plainly whether B (uninodal / transport) is a NET win by ticks. Prior finding to
confirm or overturn: **the seed gives NO net MIP speedup (~125k seed ≈ 128k cold —
noise), even when accepted.**

## Do NOT
- Do not set `CPXPARAM_Barrier_Crossover=1` (slows the cold reference).
- Do not force `solver_options.algorithm=dual/primal` without a loaded base (dual/
  primal from scratch on the root LP is far slower than barrier).
- Do not compare against the slow 633s crossover-primal run — 247s/128k (auto) is
  the reference.
- Do not run two heavy solves at once.
