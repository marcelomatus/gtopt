# Critical review: plp2gtopt Junction/FlowRight features for plexos2gtopt

Goal: identify which recently-added LP features should be adopted by
`scripts/plexos2gtopt/` to close the +76 % hydro over-dispatch on the CEN
PCP weekly bundle.  All file references absolute; read-only review.

## 1. Feature summary

* **`Junction.drain_capacity` / `drain_cost`** (commit 3d977d57a) —
  optional fields on `Junction` (declared in
  `include/gtopt/junction.hpp:70-81`).  When `drain = true`, the per-block
  drain column is created with `uppb = drain_capacity.value_or(DblMax)`
  and `cost = drain_cost.value_or(0.0)` in
  `source/junction_lp.cpp:54-55,69-81`.  Collapses the legacy synthetic
  `<central>_ocean` Junction + `_ver` Waterway pair into a single
  drain-bearing junction with the PLP `VertMax` cap and `CVert` penalty
  in line.

* **`FlowRight.bypass_junction` / `bypass_cost`** (commit 35d3bdb8a) —
  optional pass-through path declared in
  `include/gtopt/flow_right.hpp:204-212`.  LP emission lives in
  `source/flow_right_lp.cpp:664-707`: one extra `bypass` column per
  block, contributing `-1` to the source junction's balance row and
  `+1` to the bypass junction's balance row, priced at
  `CostHelper::block_ecost(scenario, stage, block, bypass_cost)`.  Mass
  is conserved across the two junctions.

* **Three `FlowMode` (per_block / stage_average / stage_uniform)**
  (commit 95dc3113b) — resolved in `source/flow_right_lp.cpp:53-85`.
  `per_block` is the default (one `flow_b`, per-block kink);
  `stage_average` keeps per-block `flow_b` (hard bounds only) plus a
  stage-scope `qeh` column with the kink at stage scope linked by
  `qavg`; `stage_uniform` emits only `qeh` and pastes it into every
  block's junction balance.  Legacy `use_average: true` aliases to
  `stage_average`.

* **One-sided kink substitution** (commit 4ec10d940 + 95dc3113b refactor)
  — `attach_flow` in `source/flow_right_lp.cpp:191-304` folds the
  fail / excess slack column + kink row into the primary flow column
  whenever exactly one of `fcost`, `uvalue` is non-zero (Demand-style
  substitution).  Saves 1 column + 1 row per block at no LP cost.

* **`gtopt_expand` bypass-target resolution** (commits 51a0a3327,
  e61eb20ef) — `_ensure_bypass_target_junction` in
  `scripts/gtopt_expand/pmin_flowright_expand.py:281-346` resolves the
  bypass target via: (1) reuse `<junction>_ocean` if present; (2) reuse
  legacy `<junction>_spill`; (3) return `None` and skip the bypass.
  Crucially the third fallback was removed (e61eb20ef) — synthesising a
  free-drain at a non-terminal junction was an LMAULE-class LP exploit.

## 2. LP semantics

### Junction drain column

In `source/junction_lp.cpp:69-81`, when `Junction.drain == true`, the
per-block balance row gains one column:

```
balance_b : ... − drain_b = 0
drain_b ∈ [0, drain_capacity],  cost(drain_b) = drain_cost · cf
```

`drain_cost` is per-(m³/s)·h (same units as Waterway.fcost), folded
into the objective via `CostHelper::block_ecost`-style coefficient
`cf` baked into the column cost.  When `drain_capacity` and
`drain_cost` are unset the column is `[0, +∞]` with zero cost (free
disposal) — that is plexos2gtopt's current state on the 22 `_ocean`
junctions.

### FlowRight bypass column

In `source/flow_right_lp.cpp:672-707`, when `bypass_junction` is set
(and mode != `stage_uniform`):

```
For each block b:
  bypass_b ∈ [0, +∞],  cost(bypass_b) = bypass_cost · cf
  source.balance_b[bypass_b]      = -1   (water leaves source junction)
  bypass_junction.balance_b[bypass_b] = +1 (water arrives downstream)
```

The bypass column is **independent** of the consumption flow column —
both run side-by-side in the same balance.  No interaction with the
kink substitution (the kink lives on the consumption side only).

## 3. Where plexos2gtopt diverges

Verified against current `master`:

1. `scripts/plexos2gtopt/entities.py:309-320` — `JunctionSpec` has only
   `name` + `drain: bool`.  **No `drain_capacity` / `drain_cost`
   fields**.  Writer `gtopt_writer.py:769-779` emits only `drain: true`,
   never the cap or cost.  All 22 `_ocean` drains in the current bundle
   are unbounded + free.

2. `scripts/plexos2gtopt/entities.py:466-503` — `FlowRightSpec` *does*
   carry `bypass_junction` / `bypass_cost` (added in 35d3bdb8a) and the
   writer `gtopt_writer.py:1233-1242` auto-resolves a `Vert_*`
   downstream when not set on the spec.  This part is already adopted.

3. `_synthesise_pinned_flow_rights` (`parsers.py:2670-2730`) is **not
   dead code** — it is invoked at `parsers.py:3932-3934` for every
   forced-flow waterway harvested by `extract_waterways`.  It emits
   FlowRights with `fcost = 1000 $/(m³/s)·h`, `target = csv_value`,
   `fmax = 10 × target` — but it does NOT set `bypass_junction` on the
   spec, relying on the writer auto-resolve.

4. plexos2gtopt does **not** call any `gtopt_expand` step — the
   pipeline stops at JSON emission.  The plp2gtopt path runs
   `pmin_flowright_expand.ensure_bypass_for_flowrights` AFTER the JSON
   is written; plexos2gtopt has no equivalent post-pass.

5. `Vert_*` waterways are kept (`parsers.py:1782-1786`) and redirected
   to `<source>_ocean` drains.  These are unbounded free drains today.

## 4. Recommended adoption order (impact-ranked)

1. **Put `drain_cost` on every `<reservoir>_ocean` drain.**  The 22
   ocean drains are currently free, so the LP cheerfully spills via
   `Vert_*` → free ocean drain whenever turbine arbitrage is negative —
   but with $40/MWh × 1.44 pf opportunity cost, spilling is NEVER
   chosen (Section 5G: turbine saves $58/cumec-h ≫ $0 drain).  Adding
   `drain_cost = CVert` (~$3.6) does NOT solve the over-dispatch by
   itself, but it makes the spill path priced consistently with
   `Vert_*.fcost`.  Files: `entities.py:JunctionSpec` (+2 fields),
   `gtopt_writer.py:769-779` (emit fields).

2. **Pin `drain_capacity = Storage.Max Spill` on each `_ocean`.**  This
   is the highest-impact change.  Today the ocean is an
   unbounded relief valve — anywhere water is "excess", the LP sends
   it through `Vert_*` (capped at `Waterway.Max Flow`) to the
   unbounded `_ocean`.  Capping the drain at PLEXOS's per-storage
   `Max Spill` forces excess water back through penstocks OR up to the
   `Vert_*` cost.  Files: read `Storage.Max Spill` in
   `extract_waterways` / `extract_junctions`, pass through
   `JunctionSpec.drain_capacity`.

3. **Switch `B_<reservoir>` bypass waterways to FlowRight with
   `bypass_junction = <reservoir>_ocean`.**  Currently
   `parsers.py:1844-1862` emits `B_*` as Waterways with `fmin = csv,
   fmax = +∞` (pin_fmax_from_profile=False).  But the downstream `B_*`
   target is a non-drain junction — the LP can only push surplus
   through the B_* arc when its downstream junction can absorb it
   (turbines, more spillways), and on COLBUN/PANGUE the downstream
   accumulates rather than drains.  Replacing each `B_<X>` with a
   FlowRight `target = csv, fmax = +∞, bypass_junction = <X>_ocean,
   bypass_cost = small ε` gives the LP an explicit pressure-release
   path priced at the spill cost.  Files: new helper in
   `parsers.py` analogous to `_synthesise_pinned_flow_rights` but for
   B_ waterways; drop the `B_*` from `out` in `extract_waterways`.

4. **Fold `_synthesise_pinned_flow_rights` to emit
   `bypass_junction = <source>_ocean` explicitly.**  Currently the
   writer auto-resolves via `spill_downstream` (the Vert_* target),
   which IS the `<source>_ocean`.  Explicit on the spec is more
   robust and lets us drop the per-Vert_* synthetic Waterway in step
   5.  Files: `parsers.py:2705-2721` add
   `bypass_junction = f"{source_junction}_ocean"`.

5. **(Optional, follow-up) Drop the 26 `Vert_*` Waterways.**  Once
   steps 1–4 are in place, the only role of `Vert_*` is to carry
   priced flow from reservoir to ocean.  But the FlowRight bypass
   column does exactly that: `-1` at source, `+1` at ocean, at
   `bypass_cost = Vert_*.fcost`.  Dropping the Waterways saves 26
   columns × n_blocks per stage and removes a parallel path the LP
   currently game-theoretically chooses between.  Verify with a
   feature-flag run first.

## 5. Open questions

* **Does `Storage.Max Spill` exist as a per-storage property in the
  CEN PCP PLEXOS DB?**  The plp2gtopt counterpart uses
  `VertMax`; the PLEXOS analogue may be `Storage.Max Volume` (wrong
  units), `Waterway.Max Flow` on `Vert_*` (already used as
  Waterway.fmax), or absent.  If absent, fall back to `Vert_*.Max
  Flow` for `drain_capacity`.

* **`bypass_cost` value for B_* and `_synthesise_pinned_flow_rights`?**
  plp2gtopt uses `CVert` ($3.6) on the junction drain and lets the
  FlowRight bypass run at $0.  CEN PCP `Vert_*.Max Flow Penalty` is
  $3.6 / 7200 / 360 in different cells.  Recommendation: mirror the
  `Vert_*` fcost on the FlowRight bypass_cost, then drop Vert_*
  entirely.

* **Should non-terminal junctions ever be drains in plexos2gtopt?**
  The e61eb20ef commit removed the synthesised-spill fallback because
  free drains at non-terminal cascade nodes let the LP shed upstream
  water.  plexos2gtopt's current 22 `_ocean` drains are all attached
  to TERMINAL cascade sinks (`<reservoir>_ocean` for Vert_* targets).
  Confirm no `_ocean` is reachable from a non-terminal reservoir
  before adding `drain_capacity`.

* **Section 5G's spill-incentive question:** plp2gtopt does NOT rely
  on `drain_cost` to "make the LP spill" — it relies on the
  combination of (a) hard `Reservoir.efin = PLP final volume` floor,
  (b) `vmax` capping per-stage volume, and (c) per-block penstock
  `fmax` caps coming from PLP's `Cmax`.  Water has nowhere to go but
  the drain when those binds.  plexos2gtopt's analogues exist
  (`Reservoir.efin` from solution `End Volume`, generator `pmax`
  caps), so adopting `drain_capacity` should be sufficient WITHOUT
  needing `drain_cost > $58/cumec-h`.  Concretely: cap the ocean
  drain at the PLEXOS spill capacity, and the LP will be forced to
  honour the penstock cap rather than route through ocean.
