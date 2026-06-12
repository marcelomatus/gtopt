# plp2gtopt modernization audit

> Generated 2026-06-01 — critical review of `scripts/plp2gtopt/` vs
> `scripts/plexos2gtopt/`, identifying modernization gaps + new
> gtopt features plp2gtopt could adopt.

## TL;DR

The biggest gap is the **hydro topology generator**: plp2gtopt still emits
synthetic `<central>_ocean` / `<central>_sink` drain junctions and uses the
**legacy `Turbine.waterway` connection mode**, even though gtopt has shipped
the **built-in waterway** (`Turbine.junction_a`/`junction_b`) for months and
plexos2gtopt's converter already uses it. Memory item
`project_turbine_builtin_waterway` confirms +35 synthetic penstock waterways
and +4 ocean junctions disappear on the plexos side after migration; the
equivalent saving in juan/IPLP is even larger because PLP centrals dominate.
`junction_writer.py` is 2 784 lines, more than half of it ocean/sink
plumbing that would simply be deleted.

Recommended next 3 items, ordered by leverage:

1. **Adopt `Turbine.junction_a/junction_b` built-in waterway** in
   `junction_writer.py` (delete `_OCEAN_UID_OFFSET`, `_ocean_junction_counter`,
   spillway/ocean-fallback code, vrebemb-as-sink synthesis) — would remove
   1 000+ lines and tens of synthetic objects per case.
2. **Expose the new line-loss family of CLI flags** (`--loss-cost-eps`,
   `--loss-envelope`, `--loss-segments`, `--lift-line-caps`,
   `line_losses_mode=tangent_signed_flow`) — plexos2gtopt has them all,
   plp2gtopt has none. Memory `project_loss_model_midpoint_envelope` shows
   this is what closed the PLEXOS-vs-gtopt cost gap to ±0.5 %.
3. **Add the Conversion Drop Funnel + per-family `.pampl` modular UC
   emission** (with `_pampl_ident` sanitization + `_PENALTY_TIER_NAMES`)
   — currently the .pampl path is irrigation-only and inherits no
   identifier hardening.

## P0 — Wrong/stale code (blocks production)

### P0.1 — Synthetic `_ocean` / `_sink` junction synthesis still lives

* **Files**: `scripts/plp2gtopt/junction_writer.py:44, 477, 1021-1088,
  1140-1211`; `scripts/plp2gtopt/gtopt_writer.py:161-243, 1450-1456`;
  `scripts/plp2gtopt/_writer_hydro.py:687-699`.
* **Diagnosis**: `_OCEAN_UID_OFFSET`, `_ocean_junction_counter`, the
  `_vrebemb_as_sink` synthesis path (line 1042 onward), and the "Spillway
  ocean fallback" branch all manufacture extra Junction + Waterway pairs
  per terminal hydro plant. gtopt's new Turbine carries its own flow arc:
  setting `junction_a` to the intake and leaving `junction_b` unset
  models a terminal drain natively (see `include/gtopt/turbine.hpp:74-86`).
  plexos2gtopt does this at `gtopt_writer.py:1971-1995`.
* **Fix**: in `_create_turbine_entry()` (currently around
  `junction_writer.py:1310`) drop `"waterway"`, set
  `entry["junction_a"] = central_name` (or reservoir intake name), and
  set `entry["junction_b"]` only when `ser_ver > 0`. Then delete the
  ocean-fallback and `_sink_collapser` branches in `gtopt_writer.py` —
  they become dead code. Keep the seepage `filt_*` waterway path for
  filtration losses (still a multi-arc topology).

### P0.2 — Pasada "flow-turbine" mode bypasses gtopt's terminal-drain shorthand

* **File**: `scripts/plp2gtopt/_writer_generation.py:62-90`.
* **Diagnosis**: under `pasada_mode=flow-turbine` we emit a `Turbine` with
  `"flow": central_name` (the `Flow` schedule connection, header lines
  63-72). That's fine for true run-of-river, but every "pasada-with-bus"
  could *also* use `junction_a` and omit `junction_b` if we ever wanted
  the per-block water balance to participate in upstream / seepage
  routing (currently impossible because there's no junction at all).
  Worth re-evaluating once P0.1 lands.

### P0.3 — `_writer_boundary.py` reaches into `low_memory`/SDDP options unconditionally

* **File**: `scripts/plp2gtopt/_writer_boundary.py:107-148`.
* **Diagnosis**: writes the bare path `"boundary_cuts.csv"` and sets
  `boundary_cuts_mode = "noload"` even when SDDP is off. plexos2gtopt's
  `plexos2gtopt.py:395-414` only wires `monolithic_options.boundary_cuts_file`
  when the case actually has cuts (`case.boundary_cut is not None`). The
  plp side is benign today but pre-broken for any future `--monolithic`
  run that has no `plpplaem1.dat`. Make the wiring conditional on
  `len(planos.cuts) > 0`.

## P1 — Missing features that plexos2gtopt has

### P1.1 — Conversion Drop Funnel

* **Evidence**: plexos2gtopt at `_comparison.py:1250-1299` emits a
  "Conversion Drop Funnel" table (raw PLEXOS counts → gtopt emitted +
  drop reason). plp2gtopt's `_comparison.py` has the "PLP Input Files →
  Indicators" ledger (line 732) but no funnel — so a vrebemb-as-sink drop,
  a dead-zero-waterway drop, a `ser_hid=0` drop, etc., is silent.
* **Fix**: capture raw counts in `_parsers.py` (the central / waterway /
  ess / battery parsers all know their pre-filter sizes already) and
  surface them next to the comparison table.

### P1.2 — Per-family modular `.pampl` UC emission with sanitization

* **Evidence**: plexos2gtopt at `gtopt_writer.py:3493-3521` defines
  `_pampl_ident()` + `_PENALTY_TIER_NAMES` (`soft_floor_penalty = 10`).
  Per-family split at `gtopt_writer.py:3550-3737`
  (`uc_config_exclusivity.pampl` etc.) with `--pampl-uc-mode`,
  `--pampl-uc-only`, `--pampl-uc-off` (see `main.py:210-265`).
* **plp2gtopt** has only the **irrigation** `.pampl` path
  (`gtopt_expand/templates/laja.tampl`, `maule.tampl`) — every other UC
  (manem volume bounds, mance flow bounds, custom user constraints from
  PLP `.dat` rows) is emitted as inline `user_constraint_array` JSON in
  `planning.json`. There is no `_pampl_ident()` helper, no
  `_PENALTY_TIER_NAMES`, no `slack_name = "slack_<ident>"` convention.
* **Fix**: lift `_pampl_ident()` into a shared helper (e.g.
  `scripts/_shared/pampl_ident.py`); the function is identical between
  the two converters and `uc_audit.py` already mirrors it. Add per-family
  splitting once non-irrigation UC volume grows.

### P1.3 — Line-loss CLI surface (gtopt features added in task #102)

* **Evidence**: plexos2gtopt main.py exposes `--loss-cost-eps`
  (line 338), `--loss-envelope`/`--loss-envelope-override` (line 456),
  per-line `--lift-line-caps` (line 678, 591). The writer emits
  `loss_cost_eps` (`gtopt_writer.py:104, 178-183`) and per-line
  `loss_envelope` (`gtopt_writer.py:719-734`).
* **plp2gtopt** has only `--line-losses-mode` (`_parsers.py:939-954`).
  `loss_segments` is computed inside `gtopt_writer.py:857`/`964` (no
  user override). `loss_cost_eps`, `loss_envelope`, per-line
  `--lift-line-caps` are missing. Memory item
  `project_loss_model_midpoint_envelope` shows the envelope decoupling +
  midpoint de-bias was the single biggest cost-accuracy win against
  PLEXOS — and PLP cases are even more loss-sensitive.

### P1.4 — `line_losses_mode = tangent_signed_flow`

* **Evidence**: gtopt added the Coffrin-style outer-approximation mode
  (`include/gtopt/line_enums.hpp:218` and `line_lp.hpp:37-51`). Neither
  converter currently selects it by default, but plexos2gtopt at least
  accepts it through `--line-losses-mode`. plp2gtopt's choices in
  `_parsers.py:940-954` should be widened (or simply pass through) so the
  new mode is reachable without `--set`-poking.

### P1.5 — `aperture_chunk_size` CLI passthrough

* **Evidence**: gtopt's `aperture_chunk_size` defaults to auto/1 per
  CLAUDE.md "Key options" table. plp2gtopt mentions it only in a
  docstring (`aperture_writer.py:338`). There is **no CLI flag** in
  `_parsers.py`. Cases that hit memory pressure cannot currently force a
  larger chunk size from `plp2gtopt`. plexos2gtopt also lacks this — but
  for juan/IPLP-scale workloads (plp2gtopt's primary user) it matters
  more.
* **Fix**: add `--aperture-chunk-size` to `_parsers.py`, plumb into
  `sddp_options.aperture_chunk_size` in `main.py` near line 453.

### P1.6 — Native-primitive promotion (`Fuel.max_offtake`, `Battery.max_cycles_day`)

* **Evidence**: plexos2gtopt entities at `entities.py:107-140, 345`
  promote PLEXOS' `MaxOfftakeDay/Week`, `MaxCyclesDay` to native gtopt
  primitives. The writer emits `Fuel.max_offtake`, `Fuel.max_offtake_cost`,
  `Battery.max_cycles_day` directly (`gtopt_writer.py:1530`).
* **plp2gtopt** has no equivalent promotion. `manem`/`maness` reservoir
  bounds are still emitted as inline `user_constraint_array` rows
  (verified via `_writer_hydro.py`). Battery cycle limits — see
  `Battery.max_cycles_day` in `include/gtopt/battery.hpp:302` — are not
  set on the plp side (no `cycle` field in `battery_writer.py:69-94`
  `BatteryEntry`).

### P1.7 — `FlowRight.bypass_junction` inline-bypass primitive

* **Evidence**: plexos2gtopt at `gtopt_writer.py:2305-2328` exploits
  `FlowRight.bypass_junction` (priced pass-through column on the
  FlowRight LP class) to skip a synthetic Waterway + Junction per
  bypass. plp2gtopt's PILMAIQUEN / ABANICO / ANTUCO / PALMUCHO
  FlowRight conversion (`_parsers.py:998-1056`,
  `pmin_flowright_writer.py:480-560`) still creates the constraint via a
  resolved `junction_b` only — could be tightened to use
  `bypass_junction` for the entries that are exactly upstream-bypass
  obligations.

## P2 — New gtopt features not yet adopted

### P2.1 — Turbine built-in waterway (covered in P0.1)

(See P0.1 above — listed twice because the migration is both a "stale
code" cleanup *and* a feature adoption.)

### P2.2 — Named penalty tiers + `slack_name` discipline

* `include/gtopt/user_constraint.hpp` exposes `slack_name` (per-row
  named slack column for visible duals).
* plexos2gtopt sets `slack_name = "slack_<_pampl_ident(name)>"`
  (`gtopt_writer.py:2300`) for every soft-floor UC and declares
  `soft_floor_penalty = 10` once per file at the top
  (`gtopt_writer.py:3511`).
* plp2gtopt **never** sets `slack_name`, so its soft UCs ship anonymous
  slack columns whose dual cannot be reported by `gtopt_check_output`
  with stable names.
* Fix: walk every plp2gtopt UC builder
  (`_writer_hydro.py`, `aperture_writer.py`, `_writer_boundary.py`,
  irrigation templates) and set `slack_name` everywhere a `fail_cost` or
  `slack_cost` is set.

### P2.3 — `boundary_cuts.csv` is already wired (good)

* plp2gtopt `_writer_boundary.py:127-148` and
  `planos_writer.py:91+` emit `boundary_cuts.csv` and wire
  `simulation.sddp_boundary_cuts_file`. plexos2gtopt's parity is at
  `plexos2gtopt.py:379-414` — same feature, present on both sides. **No
  gap.** (Only the unconditional-write subtlety from P0.3 remains.)

### P2.4 — `loss_cost_eps`, `loss_envelope`, midpoint PWL (covered in P1.3)

### P2.5 — Per-line `loss_segments` override

* gtopt accepts a per-`Line.loss_segments` integer (see
  `gtopt_writer.py:847-856` on the plexos side, which honours
  `LineSpec.loss_segments` first). plp2gtopt's `line_writer.py:27, 98-100`
  has a `loss_segments` *field* but no CLI / spec mechanism to override
  per line — only the implicit count from the PLP `genpdlin.f` section
  count. Add a `--loss-segments-override "L1:K,L2:K,..."` mirror.

### P2.6 — `--plexos-legacy`-style "=" semantics for `--plp-legacy`

* plexos2gtopt's `--plexos-legacy` is a 1-line opt-in; plp2gtopt's
  `--plp-legacy` (main.py:259-334) is well-documented (the table is good)
  but **silently forces** `pasada_mode=flow-turbine` even on cases where
  tech detection would have been better. The "applied: ..." log line
  exists but the override happens before the user can see it. Consider
  refactoring so the bundle is dumped at `--info` regardless.

## P3 — Test coverage gaps

### Headline numbers

| Converter      | Test files | Test funcs | Module lines | Tests / 1k LoC |
|----------------|-----------:|-----------:|-------------:|---------------:|
| plp2gtopt      | 86         | 956        | ~28 600       | ~33            |
| plexos2gtopt   | 30         | 490        | ~26 800       | ~18            |

plp2gtopt is **denser** in tests (per-parser/per-writer + multiple
integration files) — the count surprised me. The user prompt's "705+"
plexos number is now closer to 490 funcs (490 in 30 files); plp2gtopt
genuinely has more tests. What it **lacks** is test coverage for the
modern features identified above, not raw test volume.

### Highest-priority test cases to add

1. `test_turbine_builtin_waterway.py` — assert every emitted Turbine
   either sets `flow` OR `junction_a` (no `waterway` field) once P0.1
   lands; assert zero `*_ocean` / `*_sink` junctions in output.
2. `test_drop_funnel.py` — feed a case with 1 dead-zero waterway,
   1 vrebemb-as-sink drop, 1 `ser_hid=0` drop, verify the new funnel row
   counts match.
3. `test_pampl_ident.py` — shared helper test: name with hyphen, with
   leading digit, with dot/space, idempotent.
4. `test_slack_name_emission.py` — every soft UC in juan/IPLP-small
   output JSON carries a `slack_name` matching `_pampl_ident`.
5. `test_loss_cost_eps_cli.py` — `--loss-cost-eps 1e-4` → JSON
   `options.model_options.loss_cost_eps == 1e-4`.
6. `test_loss_envelope_cli.py` — `--lift-line-caps "L1:2.0"` →
   `Line.loss_envelope` widened on L1 only.
7. `test_tangent_signed_flow.py` — `--line-losses-mode tangent_signed_flow`
   accepted; round-trip through `from_json<Planning>`.
8. `test_aperture_chunk_size_cli.py` — `--aperture-chunk-size 4` →
   `options.sddp_options.aperture_chunk_size == 4`.
9. `test_battery_max_cycles_day.py` — promote `plpmanbat.dat`-equivalent
   cycle limit, assert `Battery.max_cycles_day` emitted (currently silent).
10. `test_fuel_max_offtake.py` — if a PLP fuel offtake cap source is added,
    assert promotion to `Fuel.max_offtake` instead of inline UC.

## Refactor sketch

Post-modernization layout (target, NOT current):

```
scripts/plp2gtopt/
  parsers/                  # was _parsers.py + *_parser.py  (slim, no logic)
  writers/
    junctions.py            # ~600 lines (was 2 784), built-in waterway model
    turbines.py             # NEW: junction_a/junction_b emission
    lines.py                # adds --loss-* + --lift-line-caps
    user_constraints.py     # split-by-family, calls _shared.pampl_ident
    boundary_cuts.py        # conditional wiring (P0.3 fix)
  _shared/                  # NEW: shared with plexos2gtopt
    pampl_ident.py
    penalty_tiers.py        # {10.0: "soft_floor_penalty"}
    drop_funnel.py
  _comparison.py            # adds Conversion Drop Funnel + raw counts
  main.py                   # +--loss-cost-eps, --loss-envelope, --loss-
                            #  segments-override, --lift-line-caps,
                            #  --aperture-chunk-size, --pampl-uc-mode
```

Net effect: ~1 200 lines deleted (ocean/sink synthesis), ~400 lines
added (CLI flags + drop funnel + shared helpers), shared
`pampl_ident`/`penalty_tiers` helpers move out of both converters into
`scripts/_shared/`, and the gtopt side learns one new fact: that PLP
spillway / vrebemb cases are now expressed natively as `junction_b`-less
turbines (no synthetic objects). Risk is low — the JSON shape change is
exactly what plexos2gtopt already emits and what
`include/gtopt/turbine.hpp:74-86` already documents as the recommended
mode.

## Methodology

### Files read in full

* `include/gtopt/turbine.hpp` (current Turbine schema, junction_a/b
  semantics).

### Files skimmed (targeted greps + spot reads)

* `scripts/plp2gtopt/junction_writer.py` (turbine emission, ocean
  synthesis, vrebemb-as-sink).
* `scripts/plp2gtopt/_parsers.py` (CLI flag inventory).
* `scripts/plp2gtopt/main.py` (--plp-legacy bundle, line_losses_mode wiring).
* `scripts/plp2gtopt/_writer_hydro.py` (irrigation .pampl pipeline,
  `user_constraint_files` plural aggregation).
* `scripts/plp2gtopt/_writer_boundary.py` (boundary cuts wiring).
* `scripts/plp2gtopt/_comparison.py` (input-file ledger present, drop
  funnel absent).
* `scripts/plp2gtopt/line_writer.py` (loss_segments path).
* `scripts/plexos2gtopt/gtopt_writer.py` (turbine built-in waterway,
  `_pampl_ident`, `_PENALTY_TIER_NAMES`, per-family UC split,
  `loss_envelope` extension).
* `scripts/plexos2gtopt/_comparison.py` (drop funnel implementation).
* `scripts/plexos2gtopt/main.py` (`--loss-*`, `--lift-line-caps`,
  `--pampl-uc-mode`).
* `scripts/plexos2gtopt/entities.py` (native-primitive promotion).
* `scripts/gtopt_expand/templates/laja.tampl`, `maule.tampl`
  (PAMPL state of the art on the plp side; uses `flow_right(name).flow`,
  not `state(var)` — confirming memory item that `state(var)` is a typing
  assertion only and not in regular use).

### Patterns checked

* `junction_a` / `junction_b` adoption on Turbine — **not adopted in
  plp2gtopt**, fully adopted in plexos2gtopt.
* Synthetic `_ocean` / `_sink` junctions — **still present** in plp2gtopt
  (`_OCEAN_UID_OFFSET`, `_ocean_junction_counter`).
* `.pampl` modular emission — **irrigation-only** in plp2gtopt; full
  per-family split in plexos2gtopt.
* `_pampl_ident()` sanitization — **absent** in plp2gtopt; defined twice
  in plexos2gtopt (`gtopt_writer.py` + `uc_audit.py`).
* `loss_cost_eps`, `loss_envelope`, `loss_segments` per-line,
  `tangent_signed_flow`, `--lift-line-caps` — **absent** in plp2gtopt;
  all present in plexos2gtopt.
* `boundary_cuts.csv` emission — **present in both**; plp2gtopt has a
  minor "unconditional wiring" smell.
* Drop funnel — **absent** in plp2gtopt; present in plexos2gtopt.
* Input-files-to-indicators ledger — **present in both** (good
  symmetry).
* `slack_name` on soft UCs — **absent** in plp2gtopt; present in
  plexos2gtopt.
* Native-primitive promotion (`Fuel.max_offtake`,
  `Battery.max_cycles_day`) — **absent** in plp2gtopt; present in
  plexos2gtopt.
* `aperture_chunk_size` CLI passthrough — **absent in both**, but
  matters more for plp2gtopt scale.

### Patterns NOT checked

* The PLP-specific `manem`/`maness`/`manli` writers' RHS profile shape
  vs gtopt's accepted UC.rhs schema (would require running a juan case).
* Whether `gtopt_expand/templates/*.tampl` Jinja flow handles all
  agreement edge cases (Convenio del Laja vs Maule cascade) — surveyed
  the file headers only.
* SDDP cut-sharing flag passthrough (memory says only `none` is safe;
  worth verifying neither converter exposes the unsafe modes).
* `RightBoundRule` multi-axis — referenced in
  `docs/analysis/irrigation_agreements/` but not searched in the writer
  code paths.
