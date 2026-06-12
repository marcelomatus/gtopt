# `--no-scale` and reservoir variable scaling — read-only audit

Investigation triggered by an empirical 10× factor observed between the
JSON-emitted `Reservoir.flow_conversion_rate` (0.04167) and the LP
storage-balance coefficient on `reservoir_extraction` (0.41667), under
`gtopt --no-scale --no-mip --lp-debug` on
`/tmp/gtopt-pcp-fcr24/DATOS20260422.json`.

## Q1. Does `--no-scale` leave any scale on reservoir LP variables?

**Answer: NO.** Under `--no-scale` every reservoir variable
(`reservoir_extraction`, `reservoir_energy`, `reservoir_eini`,
`reservoir_efin`) keeps `col.scale = 1.0`.

`--no-scale` is wired in `include/gtopt/main_options.hpp:884-905`:
it forces `scale_objective = 1`, `scale_theta = 1`, `scale_loss_link = 1`,
`equilibration_method = none`, and `auto_scale = false`.

Effects of `auto_scale = false`:

* `PlanningLP::auto_scale_reservoirs` (`source/planning_lp.cpp:712-833`)
  early-returns at line 720 — never appends a `Reservoir`/`energy`
  entry to `variable_scales`. The `emax / 1000` divisor on line 815 is
  dead code in this path.
* `PlanningLP::auto_scale_theta` and `_lng_terminals` also early-return
  (`source/planning_lp.cpp:272`, `:607`, `:843`).

Resolution of `energy_scale` / `flow_scale` for reservoirs
(`source/reservoir_lp.cpp:70-95`):

* `energy_scale = sc.options().variable_scale_map().lookup("Reservoir",
  "energy", uid)` → 1.0 because the map is empty for that key.
* `flow_scale = lp.get_col_scale(rc)` — read back after `add_col`. The
  `add_col` auto-resolution path (`include/gtopt/linear_problem.hpp:337-349`)
  is a no-op when the VSM is empty.

`PlanningOptionsLP::populate_variable_scales`
(`include/gtopt/planning_options_lp.hpp:1778-1817`) injects only
`Bus.theta` and (conditionally) `Sddp.alpha`. It NEVER injects any
`Reservoir`/`energy` or `Reservoir`/`extraction` entry. There is no
hidden per-class default scale for reservoirs.

A per-element `variable_scales` entry CAN slip past `--no-scale` —
the kill switch only blocks the auto-scale heuristics, not user-supplied
entries. The `PlanningOptions::variable_scales` array is preserved as-is
and the VSM is built from it (`include/gtopt/planning_options_lp.hpp:192`).
In the audited JSON `options.variable_scales == null`, so this is moot
here.

The reservoir `eini` LP-file bound `[0, 9890.5990991425]` matches the
JSON `Reservoir.eini` for UID 4 (COLBUN) exactly, confirming
`col.scale = 1.0` on the eini column under `--no-scale`.

## Q2. Is there a hardcoded `1000` base scale anywhere on the reservoir path?

**Answer: NO active hardcoded `1000` factor under `--no-scale`.**

* `Reservoir::default_flow_conversion_rate = 0.0036`
  (`include/gtopt/reservoir.hpp:93`) — only used when the JSON omits
  the field. The audited JSON sets it explicitly.
* `Reservoir::default_mean_production_factor = 5.0`
  (`reservoir.hpp:95`) — feeds `scost` cost coefficient only, not the
  storage-balance coefficient.
* `StorageOptions::energy_scale` default = 1.0
  (`include/gtopt/storage_lp.hpp:68`). The docstring claim "For
  reservoirs the default is 100000 (dam³→Gm³)" is STALE — there is no
  such field on `Reservoir`, and `ReservoirLP::add_to_lp` always passes
  `energy_scale = vsm.lookup(...)` which returns 1.0 unless the user
  populated the map.
* `auto_scale_reservoirs::scale_for(*emax / 1000.0)`
  (`planning_lp.cpp:815`) — the `/1000` is INSIDE the early-returned
  branch and never executes under `--no-scale`.
* `flatten()` (`source/linear_problem.cpp:444-475`) initialises
  `col_scales = 1.0`, then writes `col_scales[i] = col.scale`. With
  `equilibration_method = none`, no further multiplications occur.

## The actual source of the 10× factor

It is **not** in gtopt C++. It is in the PLEXOS converter:

`scripts/plexos2gtopt/gtopt_writer.py:706` (uncommitted working-tree
change vs `cb4dcf7e8`):

```python
"flow_conversion_rate": 10.0 / 24.0,
```

Comparison of the two JSON files in `/tmp/gtopt-pcp-fcr24/`:

| File | `Reservoir 4.flow_conversion_rate` |
|---|---|
| `DATOS20260422.json` (current input) | `0.041666666666666664` = 1/24 |
| `output/planning.json` (gtopt's dump after parse) | `0.41666666666666663` = 10/24 |

`gtopt_lp_runner.cpp:663` writes `planning.json` by serialising the
exact `Planning` struct that drove the LP build (no transformation,
verified in `gtopt_json_io_write.cpp`). The LP coefficient
`0.416666666666667` matches `planning.json` exactly.

The mismatch between the *current* `DATOS20260422.json` (1/24) and the
gtopt-dumped `planning.json` (10/24) is explained by the input file
having been overwritten AFTER the gtopt run (timestamps: input
22:13:18, planning.json 22:14:05 — but the converter's
`10.0/24.0` line was already in place during the 22:13 run). Both the
LP file and the gtopt-emitted planning.json are self-consistent: they
reflect the JSON the parser actually saw, namely FCR = 10/24.

The `gtopt_writer.py` comment block at lines 696-705 attributes the 10×
to "an extra 10× in the LP build pipeline" — that diagnosis is
incorrect. The 10× is solely the writer's own `10.0 / 24.0` literal,
which is a stand-in for the (PLEXOS-native) `1/24` CMD ⇄ m³/s·h
conversion times an unjustified factor of 10.

## Recommendation (informational, no code changed)

Revert `gtopt_writer.py:706` to `1.0 / 24.0` (or the dimensionally
correct `3600.0 / 86400.0`). The LP coefficient
`flow_conversion_rate × duration × dc_stage_scale` in
`include/gtopt/storage_lp.hpp:690` is faithful to the input — no
hidden multiplier — so the writer should emit the physical value
directly.
