# Hydro over-dispatch on the CEN PCP weekly bundle — topology review

**Date**: 2026-05-22.
**State**: gtopt = 505 GWh, PLEXOS = 286 GWh (+76 % over).
**Already matching**: reservoir trajectories (`efin` pinned), forced waterways
(`Riego_*` / `Caudal_Eco_*` / `Filt_*`), demand. Gap is in **Vert spillage**
(PLEXOS 7,479 m³/s·h vs gtopt 0) and **B_\*** bypass routing.

## Diagnosis — where the leak is

1. **Vert_\* spillways have no incentive to carry flow.** In
   `parsers.py:1795-1803` we read `Min Flow`, `Max Flow`, and `Max Flow
   Penalty` from the PLEXOS Waterway object. CEN PCP ships
   `Max Flow Penalty = 3.6 $/(m³/s)/h` on most `Vert_*` arcs — that is the
   *upper bound penalty* on Vert flow. Because there is no `fmin` on a
   spillway, the LP's strictly cheaper alternative is to route water
   through a parallel `penstock_<unit>` (zero `fcost`, free MWh) than
   to pay 3.6 $/(m³/s)/h on Vert. The Vert column is therefore a free,
   *optional* outflow that the LP only uses if it must. **PLEXOS forces
   the split a different way**: in CEN PCP a `Waterway.Min Flow` on
   each `Vert_*` (or the head reservoir's `Max Volume` clamp coupled
   with `Spill Penalty`) is the source of the 7,479 m³/s·h. We read
   the `Min Flow` static property (line 1795) but never check
   `Hydro_*Spill*.csv` series and there is no second-order coupling
   from reservoir head to Vert.

2. **Bypass arcs `B_*` are unbounded above with zero cost.** With the
   2026-05-22 patch (`parsers.py:1844-1850` + `entities.py:306`
   `pin_fmax_from_profile`), `B_Maule` is emitted as `fmin = csv,
   fmax = +∞, fcost = 0`. The LP now sees a free, unbounded conduit
   from the head junction to the downstream junction and routes all
   surplus (up to the upstream penstock's cap) through it instead of
   spilling. Critically, `B_Maule.junction_b` is *Post_Maule*, NOT a
   `_ocean` drain — so water flowing through `B_Maule` lands in the
   cascade and reaches downstream turbines. **PLEXOS's behaviour is
   exactly the opposite**: B_\* is bounded above (cap unknown, but
   finite), and surplus that the bypass cannot absorb routes to
   `Vert_B_C_Isla` (2,858 m³/s·h), `Vert_B_M_Isla` (910 m³/s·h), etc.

3. **`Vert_*` ocean drains have unbounded `drain_capacity` and zero
   `drain_cost`.** `parsers.py:1894-1948` emits each `<rsv>_ocean`
   sink as a `JunctionSpec(drain=True)`. The writer
   (`gtopt_writer.py:769-779`) emits only `entry["drain"] = True` —
   **`drain_capacity` and `drain_cost` are never populated** even
   though `Junction.drain_capacity` / `drain_cost` exist (commit
   3d977d57a, `include/gtopt/junction.hpp:70-81`,
   `source/junction_lp.cpp:54-55`). This means every `Vert_*` arc
   lands on a junction with `drain_uppb = +∞` and `drain_cost = 0`,
   so spill is a free, unbounded sink — but the **3.6 $/(m³/s)/h
   penalty already sits on the `Vert_*` Waterway itself**, so the
   ocean drain isn't the problem. The problem is symmetric: the LP
   only spills when forced (Vert has no `fmin`).

4. **Penstock `fmax` cap is per-unit-peak, not per-block.**
   `gtopt_writer.py:940-942` sets `penstock.fmax =
   pmax_peak/production_factor` with `pmax_peak = max(pmax_profile)`.
   A unit with `pmax_profile = [0, …, 113.4, …, 0]` gets penstock
   `fmax = 113.4/pf` at *every* block — even hours when the unit's
   pmax is 0 (a slack on `gen` would still be enforced, but the
   penstock-flow column is unbounded by pmax). This was tightened
   in commit message at line 884-894 but only across the horizon,
   not per-block. Result: in hours where PLEXOS has the unit
   off-line, gtopt's LP can still route up to peak-rated flow
   through the penstock for free (the turbine_conversion row
   couples gen to flow, but if gen lower-bound is 0 and the
   penstock has fmax > 0 with zero cost, the LP can carry water
   past the turbine without producing MWh — *unless* the gen col
   itself is skipped at pmax=0 blocks, in which case the
   penstock is unbounded). See note in `parsers.py:3775-3787`
   on the `pmax=0` whole-horizon case; the **per-block zero hours
   are NOT filtered**.

5. **`_synthesise_pinned_flow_rights` is dead code.** `parsers.py:1671`
   accepts `forced_targets_out` but `extract_waterways` no longer
   appends to it (the FORCED_FR_PREFIXES branch was removed in the
   uncommitted working tree). `parsers.py:3699-3701` passes an empty
   list to `_synthesise_pinned_flow_rights` (line 3932-3935), which
   then emits zero `FlowRight`s. Harmless but misleading.

## Recommended changes — ordered by expected impact

### 1. Force Vert spillage via PLEXOS `Min Flow` on the Waterway

**Files**: `scripts/plexos2gtopt/parsers.py:1795-1850`.

PLEXOS publishes `Waterway.Min Flow` on each `Vert_*` arc that does
spill — we already read it on line 1795 into `fmin`, but then immediately
overwrite it (line 1846) with `forced_target` from `Hydro_WaterFlows.csv`.
For `Vert_*` we should *not* overwrite: when the static `Min Flow > 0`,
keep it as the hard floor; when `Hydro_*Spill*.csv` (or the per-storage
`Hydro_Storage_*Spill*.csv` if present) is available, use the per-hour
series as `fmin`. Then the LP must carry that flow on Vert. Expected
~7,500 m³/s·h spillage realised.

*Rationale*: matches the empirical PLEXOS spillway split (Vert_B_C_Isla
2,858; Vert_LAJA_I 2,316; Vert_RUCUE 1,059; Vert_B_M_Isla 910).

### 2. Pin `B_*` bypass `fmax` to PLEXOS `Max Flow` (or to the published
profile peak when constant)

**Files**: `scripts/plexos2gtopt/parsers.py:1844-1850`,
`scripts/plexos2gtopt/entities.py:306`.

Currently `is_bypass` sets `fmax = 0.0`, which the writer at
`gtopt_writer.py:826-828` translates to "no fmax key → +∞". The
PLEXOS Waterway object's `Max Flow` static property (already read at
line 1796, then clobbered) should win as `fmax` for `B_*` arcs. Pair
with the Vert fix above — otherwise capping B_\* without forcing Vert
gives infeasibility on weeks where head reservoir surplus exceeds
B_\* + penstock. Combine the two patches.

*Rationale*: matches PLEXOS total flow through Vert + B (26,868 vs
gtopt 19,197 — gap is 7,671 m³/s·h being free-routed through penstocks).

### 3. Use `Junction.drain_capacity` / `drain_cost` on `_ocean` sinks
populated from PLEXOS `Spill Penalty` and `Max Spill`

**Files**: `scripts/plexos2gtopt/entities.py:320` (extend `JunctionSpec`
with `drain_capacity`, `drain_cost`), `scripts/plexos2gtopt/parsers.py:1735-1786`
(propagate the source reservoir's `Spill Penalty` and `Max Spill` to the
synthesised `<source>_ocean` JunctionSpec), `scripts/plexos2gtopt/gtopt_writer.py:776-778`
(emit the new fields when present).

CEN PCP ships `Spill Penalty = 0` and `Max Spill = unset`, so this is
forward-compat noise on the current bundle. However once #1 forces
flow through Vert, the *destination* drain becomes a load-bearing
constraint — if a bundle ever ships a finite `Max Spill`, gtopt would
silently exceed it. Doing this now closes that gap and makes the
`Junction.drain_capacity` feature (commit 3d977d57a) actually
reachable from plexos2gtopt.

*Rationale*: makes the existing `Junction.drain_capacity` feature
useful, defends against future bundles, and removes the silent
"+∞ drain" footgun.

### 4. Tighten `penstock.fmax` to a per-block profile derived from
`pmax_profile / production_factor`

**Files**: `scripts/plexos2gtopt/gtopt_writer.py:925-943` (penstock
synthesis), `scripts/plexos2gtopt/entities.py:269` (extend
`WaterwaySpec` to carry an optional fmax matrix), the existing
`forced_flow_profile` path already handles per-block matrix emission
(`gtopt_writer.py:812-825`).

When `pmax_profile` is non-constant, emit `penstock.fmax` as a
per-block matrix `[[pmax_b / pf for b in block_layout]]` instead of
`pmax_peak / pf`. Blocks with `pmax_b = 0` get `fmax = 0` so the
penstock is forcibly closed exactly when the unit is off-line.
Closes the silent free-conduit leak at zero-pmax blocks.

*Rationale*: per-block tightening is the dual to the per-block
`pmax_profile` we already emit on the generator; without it the
free-energy arbitrage path through penstocks at zero-pmax hours
remains open. Matches the PLEXOS convention (Max Flow on a turbine
respects unit availability).

### 5. Delete the dead `_synthesise_pinned_flow_rights` path

**Files**: `scripts/plexos2gtopt/parsers.py:1671` (drop
`forced_targets_out` parameter), `parsers.py:2670-2730` (remove
helper), `parsers.py:3699-3935` (drop call site).

`forced_waterway_targets` is never populated by the current
`extract_waterways`; the helper emits zero entries on every run.
Removing it shrinks parsers.py by ~70 lines and eliminates the
"FlowRight is forced-flow" ambiguity in the codebase. Pair with a
`extract_flow_rights` reduction so only the *real* per-junction
FlowRight source path remains.

*Rationale*: code hygiene; reduces the surface area future debugging
has to consider.

## Risks

- **#1 (Vert fmin)** could re-introduce infeasibility when the head
  reservoir's `eini` is below the volume needed to sustain the
  PLEXOS-published spill rate for the whole horizon. The new
  `solution_efin` pin (`parsers.py:1568-1569`) makes the trajectory
  match PLEXOS exactly, so the volume IS available — but verify on a
  bundle where `solution_efin` is unavailable (older PCP runs without
  the `.accdb`). Mitigation: route Vert through a *soft* slack
  (FlowRight with `fcost = hydro_spill_cost`) when the hard pin
  proves infeasible.
- **#2 (B_\* fmax)** in combination with **#1** must be exact: capping
  B_\* without forcing Vert leaves the LP nowhere to put the surplus
  and either causes infeasibility or pushes mass back through the
  reservoir efin slack. Both fixes must land in the same patch.
- **#3 (drain caps)** is forward-compat only on CEN PCP; verify the
  reservoir extractor still reports `spill_penalty_per_mwh = 0` for
  every storage (`parsers.py:1588-1590`) before relying on the new
  fields to be no-ops on this bundle.
- **#4 (per-block penstock)** is the only change that could regress
  the existing perfect Riego/Caudal_Eco/Filt match: those waterways
  are *not* synthetic penstocks, so they're untouched. Verify by
  diffing the post-patch JSON's `waterway_array` entries for
  `Riego_*` / `Caudal_Eco_*` / `Filt_*` — they should be byte-for-byte
  unchanged.
- **#5 (dead-code removal)** is mechanical; no LP behaviour change.

## Out of scope (verified non-issues)

- `expand_reservoir_constraints` (`source/system.cpp:321-389`) only
  hoists nested seepage/dlim/pfac into top-level arrays; it does NOT
  synthesise duplicate drain paths. Reviewed end-to-end, no double-
  count vector.
- Pure-pondage demotion (`parsers.py:3833-3856`) requires `emin ==
  emax == efin == 0` AND not a turbine `main_reservoir`. With the
  new `solution_efin` pin, real reservoirs get `efin > 0` (PLEXOS End
  Volume) and stay as Reservoirs. The demotion list on CEN PCP is
  exactly the pass-through bocatomas (B_C_Isla, B_M_Isla, B_Maule,
  Post_Antuco/Isla/Machicura/Pangue/Quilleco) — all genuinely
  storage-free and correctly demoted.
