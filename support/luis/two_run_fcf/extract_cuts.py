#!/usr/bin/env python3
"""Extract a gtopt SDDP terminal-stage cut family (parquet) into a
boundary_cuts.csv that the NCP-side run reads as terminal cost-to-go.

Interface contract:
  * The NCP-side case defines the CANONICAL reservoir set + names; we emit
    exactly those columns (header = 'Reservoir:<name>'), matched by reservoir
    NAME (the SDDP-side parquet carries SDDP uids; we map uid->name via the
    SDDP-side case, then re-key by name to the NCP set).
  * CSV reservoir value = -(parquet coeff val): the loader negates it
    (sddp_boundary_cuts.cpp: row[col] = -coeff), so this round-trips the LP
    coefficient exactly.  rhs is passed through (loader adds the alpha term).
  * Units unchanged: coeff is the LP efin coefficient (= water value), already
    in the SDDP run's scale_objective (footer carries it; here 1).

Usage:
  extract_cuts.py CUT_PARQUET SDDP_CASE_JSON NCP_CASE_JSON OUT_CSV [TERMINAL_PHASE]
"""

import csv
import json
import sys

import pyarrow.parquet as pq

cut_pq, sddp_json, ncp_json, out_csv = sys.argv[1:5]
forced_phase = int(sys.argv[5]) if len(sys.argv) > 5 else None

# SDDP-side: reservoir uid -> name (the parquet coeffs carry SDDP uids)
sddp_res = {
    r["uid"]: r["name"] for r in json.load(open(sddp_json))["system"]["reservoir_array"]
}
# NCP-side: canonical reservoir name set + emission order
ncp_names = [r["name"] for r in json.load(open(ncp_json))["system"]["reservoir_array"]]

tbl = pq.read_table(cut_pq)
scale = float((tbl.schema.metadata or {}).get(b"scale_objective", b"1").decode())
d = tbl.to_pydict()

# Terminal stage = max phase unless overridden.
phases = sorted(set(d["phase"]))
term = forced_phase if forced_phase is not None else phases[-1]

# Iteration selection: 'last' (default) = only the CONVERGED final-iteration cut
# (correct for a deterministic run — all iterations visit the same state, so the
# last is the tightest; earlier iterations are pre-convergence, looser cuts).
# 'all' = every iteration's cut = the full multi-cut envelope (needed once the
# run is STOCHASTIC and iterations visit different reservoir states).
iter_sel = sys.argv[6] if len(sys.argv) > 6 else "last"
iters_at_term = [
    d["iteration"][i] for i in range(len(d["phase"])) if d["phase"][i] == term
]
last_iter = max(iters_at_term) if iters_at_term else None

rows = []
for i in range(len(d["phase"])):
    if d["phase"][i] != term:
        continue
    if iter_sel == "last" and d["iteration"][i] != last_iter:
        continue
    row = {
        "iteration": int(d["iteration"][i]),
        "scene": int(d["scene"][i]),
        "rhs": float(d["rhs"][i]),
    }
    for c in d["coeffs"][i]:
        if c["cls"] != "Reservoir":
            continue  # alpha term is re-added by the loader; skip non-reservoir
        name = sddp_res.get(c["uid"])
        if name is None:
            sys.exit(f"ERROR: parquet Reservoir uid {c['uid']} not in SDDP case")
        if name not in ncp_names:
            sys.exit(f"ERROR: SDDP reservoir {name!r} not in NCP canonical set")
        # CSV carries PHYSICAL coefficients: the loader negates (row = -coeff)
        # and applies the NCP run's own ÷scale_objective via compose_physical,
        # so we emit -val*scale_objective(sddp).  scale=1 for sddp/cascade (a
        # no-op, round-trip-verified); the *scale path is unverified for scale!=1.
        row[name] = -float(c["val"]) * scale
    rows.append(row)

# Emit exactly the NCP canonical columns (zero-fill reservoirs absent from a cut).
header = ["iteration", "scene", "rhs"] + [f"Reservoir:{n}" for n in ncp_names]
with open(out_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(header)
    for r in rows:
        w.writerow(
            [r["iteration"], r["scene"], f"{r['rhs']:.10g}"]
            + [f"{r.get(n, 0.0):.10g}" for n in ncp_names]
        )

print(
    f"wrote {len(rows)} terminal-phase-{term} cut(s) over {len(ncp_names)} "
    f"reservoir(s) to {out_csv}  (scale_objective={scale})"
)
print("  header:", header)
for r in rows:
    print(
        "  cut:", {k: round(v, 3) if isinstance(v, float) else v for k, v in r.items()}
    )
