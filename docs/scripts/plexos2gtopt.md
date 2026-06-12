# plexos2gtopt — PLEXOS to gtopt Converter

Converts a **CEN PCP daily PLEXOS bundle**
(`PLEXOS{YYYYMMDD}.zip` / `DATOS{YYYYMMDD}.zip[.xz]`) into a gtopt
planning JSON + Parquet schedule tree, so a PLEXOS-modelled power
system can be solved with the gtopt LP/MIP backend (CPLEX, HiGHS,
CBC, CLP) and compared end-to-end against the PLEXOS reference
solution.

It is the PLEXOS sibling of [`plp2gtopt`](plp2gtopt.md) (PLP Fortran
dialect) and [`sddp2gtopt`](sddp2gtopt.md) (PSR-SDDP dialect), and
reuses gtopt's native LP element catalogue + AMPL-style
`UserConstraint` framework rather than emitting a parallel modelling
surface.

> **Audience.** This document is the critical-review reference for
> `plexos2gtopt`: it specifies how it works, what files it depends
> on, every gtopt element it emits, every constraint family, the
> validation passes that confirm the conversion, and the operational
> picture against PLEXOS on the canonical CEN PCP 2026-04-22 weekly
> bundle (both MIP and LP-relax). See `scripts/plexos2gtopt/DESIGN.md`
> for the original v0 design proposal (most of which is implemented).

---

## 1. What it does, in one paragraph

PLEXOS keeps the model schema in a single MasterDataSet XML
(`DBSEN_PRGDIARIO.xml`, ~36 MB) and ships time-series data in
~50 sibling CSVs (one per object class). `plexos2gtopt` opens the
bundle (zip / zip.xz / extracted directory), walks the XML object
graph (objects, classes, memberships, properties), reads every
per-class CSV at the chosen horizon, builds an in-memory
`PlexosCase` of frozen typed dataclasses (one per converted entity
class), then runs the gtopt writer which emits:

* a self-contained planning JSON (`<bundle>.json`),
* a per-class Parquet schedule tree (`Generator/pmax.parquet`,
  `Reservoir/inflow.parquet`, …, one column per object),
* per-family modular AMPL files (`uc_<family>.pampl`) for the
  PLEXOS-translated `UserConstraint` rows,
* a `boundary_cuts.csv` carrying the PLEXOS Future-Cost-Function
  hyperplane (FCF terminal value, when present),
* a curated `solvers/cplex.prm` (`MIP Gomory = 2`,
  `MIPGap = 0.01`),
* a sidecar `<bundle>.provenance.json` documenting per-element
  PLEXOS source / units / transforms.

A post-conversion validation pass (`run_post_check`) immediately
diffs the converted JSON against the PLEXOS bundle to confirm every
class, constraint family and global indicator landed where expected
— this is the converter's primary correctness gate.

---

## 2. Quick start

### Install / locate

The script lives at `scripts/plexos2gtopt/`. It is registered as an
entry point in `scripts/pyproject.toml`, so a `pip install -e .` (or
the in-tree alias) exposes `plexos2gtopt` on `PATH`. The same module
can always be run as `python -m plexos2gtopt`.

### Minimum run

```bash
# Sanity check + summary (no conversion):
plexos2gtopt --info support/plexos/pcp_2026-04-22/PLEXOS20260422.zip

# Schema-light validation (no conversion):
plexos2gtopt --validate support/plexos/pcp_2026-04-22

# Default conversion (writes to gtopt_PLEXOS20260422/ next to the input).
# Auto-discovers RES<DATE>.zip[.xz] sibling for the PLEXOS block layout.
plexos2gtopt support/plexos/pcp_2026-04-22/PLEXOS20260422.zip \
  -o gtopt_PLEXOS20260422

# Standalone re-comparison: parse the bundle vs an existing JSON.
plexos2gtopt --compare gtopt_PLEXOS20260422/PLEXOS20260422.json \
  support/plexos/pcp_2026-04-22/PLEXOS20260422.zip

# Solve the resulting case (MIP by default; --solver picks the backend):
gtopt gtopt_PLEXOS20260422/PLEXOS20260422.json --solver cplex
```

### Accepted input shapes

`plexos_loader.locate_bundle` accepts any of:

* A directory already containing `DBSEN_PRGDIARIO.xml` + the per-class
  CSVs (extracted by hand).
* A `DATOS{YYYYMMDD}.zip` archive (the input dataset).
* A `DATOS{YYYYMMDD}.zip.xz` (the on-disk form vendored under
  `support/plexos/pcp_2026-04-22/`).
* A `PLEXOS{YYYYMMDD}.zip` outer wrapper (auto-unwrapped to its
  DATOS inner zip).
* A directory containing a single `DATOS*.zip[.xz]` inside (auto-
  located).

The optional `RES{YYYYMMDD}.zip[.xz]` solution archive is **not
required** for the conversion, but `plexos2gtopt` will auto-discover
it next to the input when present and pull two things from it:

1. the exact `t_phase_3` block layout PLEXOS solved over
   (111 variable-duration blocks per CEN PCP week — under
   `--horizon-mode plexos`, the default);
2. a cached zstd CSV mirror of every solution table
   (`<output_dir>/plexos_cache/*.csv.zst`) for downstream
   `compare_with_plexos` (Step-3 cost / energy / SRMC / LMP diffs).

---

## 3. Input file inventory and what they feed

### 3.1 Master schema (XML)

| File | Role |
|---|---|
| `DBSEN_PRGDIARIO.xml` | The PLEXOS MasterDataSet object graph: `t_class` (96 object classes), `t_object` (~4 500 instances), `t_attribute`, `t_property`, `t_collection`, `t_membership`, `t_data` (per-property numeric values), `t_band`, `t_text`, `t_memo_data`, `t_action`. XML namespace `http://tempuri.org/MasterDataSet.xsd`. |
| `PLEXOS_Param.xml` | Run-parameters wrapper (`Day Beginning`, `Step Count`, `Step Type`, `Horizon`). Used to infer the run horizon length (`*_7d` → 7 days) when no solution layout is available. |

### 3.2 Per-class CSV time-series

Every CSV is in one of two layouts:

* **Long** — `NAME, YEAR, MONTH, DAY, PERIOD, BAND, VALUE`. `PERIOD`
  is 1-based hour-of-day; `BAND` carries piecewise / multi-segment
  records. Read by `plexos_csv.read_long`.
* **Wide** — `YEAR, MONTH, DAY, PERIOD, <obj1>, <obj2>, …`. One
  column per object, one row per hour. Read by `plexos_csv.read_wide`.

Each file feeds at least one gtopt field; the converter's input ledger
(displayed in the post-check report under "PLEXOS Input Files →
Indicators") makes this auditable in every run:

| CSV | Carries | Drives gtopt field |
|---|---|---|
| `Gen_Rating.csv` | per-(unit, hour) pmax (MW) + DLR profile | `Generator.pmax` (scalar + per-block) |
| `Gen_UnitsOut.csv` | forced-outage schedule | `Generator.pmax_factor` (derate by units-out) |
| `Gen_FixedLoad.csv` | per-unit must-run / forced output | `Generator.pmin`/`pmax` pinning + soft `pmin_fcost` |
| `Gen_MinStableLevel.csv` | per-unit / per-period Pmin (MW) | `Commitment.pmin` / `Commitment.pmin_profile` |
| `Gen_HeatRate.csv` | heat rate (multi-band optional) | `Generator.heat_rate` + `heat_rate_segments` |
| `Gen_VOMCharge.csv` | VOM ($/MWh) | folded into `Generator.gcost` |
| `Gen_AuxUse.csv` | auxiliary station-service fraction | derates net capacity (folded into gcost) |
| `Gen_FuelTransportCharge.csv` | per-unit fuel transport adder | folded into `Generator.gcost` |
| `Gen_IniGeneration.csv` | dispatch at `t=0` | `Commitment.initial_power` |
| `Gen_IniUnits.csv` | units initially online | `Commitment.initial_status` |
| `Gen_IniHoursUp.csv` / `Gen_IniHoursDown.csv` | hours up/down at `t=0` | `Commitment.initial_hours` |
| `Gen_Commit.csv` | commit override (`-1` free / `0` off / `+1` on) | `Commitment.must_run` / `fixed_status` per block |
| `Gen_StartCost.csv` | startup cost ($) | `Commitment.startup_cost` |
| `Gen_ShutDownCost.csv` | shutdown cost ($) | `Commitment.shutdown_cost` |
| `CFdata/CPF/*.csv` (`<gen>_MRU/MRD`) | per-unit ramp curves | `Commitment.ramp_up`/`ramp_down` (+ profile) |
| `Lin_MaxRating.csv` | line forward rating (steady-state) | `Line.tmax_normal_ab` + per-block DLR |
| `Lin_MinRating.csv` | line reverse rating | `Line.tmin_ab` |
| `Lin_Units.csv` | parallel circuits + in-service schedule | `Line.capacity` + `Line.in_service` profile |
| `Nod_Load.csv` | per-bus per-hour load | `Demand.lmax` |
| `BESS_IniValue.csv` | battery initial SOC | `Battery.eini` |
| `Hydro_MaxVolume.csv` | reservoir max volume (hm³) | `Reservoir.emax` (+ optional per-block) |
| `Hydro_MinVolume.csv` | reservoir min volume (hm³) | `Reservoir.emin` (+ optional per-block) |
| `Hydro_InitialVolume.csv` | reservoir initial volume | `Reservoir.eini` / `efin` |
| `Hydro_WaterFlows.csv` | natural inflow + forced flow profile | `Flow.discharge` + `Waterway.forced_flow_profile` |
| `Hydro_EfficiencyIncr.csv` | turbine productibility | `Turbine.production_factor` (`fp_med`) |
| `Hydro_StoWaterValues.csv` | water values + FCF intercept | `BoundaryCutSpec` + `alpha_fcf` / `boundary_cuts.csv` |
| `Hydro_AntucoBounds.csv` | Laja irrigation envelopes | `FlowRight` overlays |
| `Hydro_MaxRampDay.csv` | hydro daily-ramp limits | per-day RHS overlay on `*ramp*` UCs |
| `Res_Requirement.csv` | reserve requirement (MW) | `ReserveZone.urreq` / `drreq` |
| `Res_Timeslice.csv` | reserve day-type / hour mapping | per-day day-type selector for `Res_Requirement` |
| `SSCC_Activation_BESS.csv` | BESS ancillary-services activation % | `ReserveProvision.ur_provision_factor` / `dr_provision_factor` |
| `Fuel_Price.csv` | monthly fuel prices ($/MMBtu) | `Fuel.price` |
| `Fuel_MaxOfftakeWeek.csv` | weekly fuel offtake limits | `Fuel.max_offtake` (soft) |

### 3.3 Solution database (optional — read only when needed)

| File | Used by |
|---|---|
| `<Model> Solution.accdb` (inside `RES<DATE>.zip`) | Block layout (`t_phase_3`, 111 blocks for CEN PCP weekly); plus the cached PLEXOS solution tables for `--use-plexos-commit` / `--use-plexos-gen-cap` / `--use-plexos-efin` curve-fit modes and for the downstream `compare_with_plexos` Step-3 tables. |

The default conversion is strictly **inputs-only**: the solution is
consulted **only** for the block grouping. Setting any of the
`--use-plexos-*` curve-fit flags activates a heavier mdb-export + zstd
pass that mirrors the relevant `.accdb` tables to
`<output_dir>/plexos_cache/`.

---

## 4. Architecture and module map

```
scripts/plexos2gtopt/
├── __main__.py             # python -m plexos2gtopt
├── main.py                 # argparse / CLI dispatch (~900 lines)
├── DESIGN.md               # original v0 design (specification)
├── plexos2gtopt.py         # convert/validate/compare entry points (~550 lines)
├── plexos_loader.py        # bundle locator (zip / zip.xz / outer wrapper)
├── plexos_xml.py           # MasterDataSet XML reader (objects, properties, memberships)
├── plexos_csv.py           # long/wide CSV readers (multi-day, fill-forward)
├── plexos_block_layout.py  # .accdb t_phase_3 reader + cache_plexos_tables
├── entities.py             # 24 frozen dataclasses (one per emitted entity)
├── parsers.py              # ~30 extract_*(...) functions, ~9 300 lines
├── gtopt_writer.py         # build_planning + per-class build_*_array (~3 500 lines)
├── uc_families.py          # the 8-family UC taxonomy (single source of truth)
├── _comparison.py          # post-conversion PLEXOS↔gtopt comparison (~1 500 lines)
├── compare_with_plexos.py  # standalone Step-3 PLEXOS solution diff (cost / MWh / SRMC / LMP)
├── auto_lift_lines.py      # post-conversion radial-cap detection (pandapower / gtopt DC OPF)
├── info_display.py         # --info renderer
├── solvers/                # bundled cplex.prm shipped into every output dir
└── tests/                  # 25 unit/integration test files (≥83% coverage)
```

### High-level pipeline

```
                 ┌──────────────────────────────────────────────────┐
                 │           PLEXOS bundle on disk                  │
                 │  PLEXOS{date}.zip │ DATOS{date}.zip[.xz] │ dir   │
                 └──────────────────────────────────────────────────┘
                                       │
                                       ▼
                       plexos_loader.locate_bundle
                       (auto-unwrap outer / xz / zip;
                        return PlexosBundle root dir)
                                       │
            ┌──────────────────────────┼────────────────────────────┐
            ▼                          ▼                            ▼
  plexos_xml.load_xml         plexos_csv.read_long           plexos_block_layout
  (PlexosDb: t_class /        plexos_csv.read_wide           .load_block_layout_from_accdb
   t_object / t_data /        (per-class CSV → dict[name →   (RES*.zip → .accdb →
   t_membership / t_band)     list[float]])                   111-block t_phase_3 grid)
            │                          │                            │
            └──────────────────────────┴────────────────────────────┘
                                       │
                                       ▼
                          parsers.extract_case(bundle)
                          (~30 extract_*(...) per-class
                           extractors → typed entities
                           in entities.py)
                                       │
                                       ▼
                          gtopt_writer.build_planning(case)
                          (per-class build_*_array → planning dict)
                                       │
                  ┌────────────────────┴──────────────────┐
                  ▼                                       ▼
       write_planning(planning, *.json)       write_user_constraint_pampl
       + Parquet schedule tree                (per-family uc_<fam>.pampl)
       + boundary_cuts.csv                    + write_boundary_cut_csv
       + solvers/cplex.prm                    + write_provenance(*.provenance.json)
                                       │
                                       ▼
                     _comparison.run_post_check(planning, case)
                     (element counts / UC families / global indicators /
                      drop funnel + input-file ledger + JSON validation)
                                       │
                                       ▼
                                Exit code = #CRITICAL findings
```

### Why a multi-file layout (not a single `tools/plexos2gtopt.py`)

PLEXOS input is structurally identical to PSR-SDDP — XML object
database + per-class CSVs — so the `sddp2gtopt` hybrid layout
(entity dataclasses + per-class extractors + per-class writers)
maps naturally. The XML reader alone (objects + properties +
memberships + bands) is ~700 lines; the writer is ~3 500 lines;
the comparison is ~1 500. A single-file converter was rejected in
the design doc as unmaintainable.

The clean split also means PLEXOS schema upgrades (the 96 enabled
classes are a moving target, and `class_id` re-use across releases
is documented) only churn `plexos_xml.py` + the affected
`extract_*`.

---

## 5. Time model

CEN PCP runs in two horizon modes; both are emitted as a single
deterministic stage with a chronological block grid:

| Mode | Selector | Block grid | Used when |
|---|---|---|---|
| `plexos` (default) | `--horizon-mode plexos` | The exact `t_phase_3` variable-duration grouping PLEXOS solved over (111 blocks across 7 days for CEN PCP weekly, `[24, 20, 13, 14, 12, 15, 13]` per day) | A sibling `RES<DATE>.zip[.xz]` (or explicit `--plexos-solution-accdb`) provides the layout |
| `plexos` fallback / `hourly` | `--horizon-mode hourly`, or `plexos` with no solution available | Uniform 1-hour blocks (`24 × n_days`, e.g. 168 for a week) | No solution available — runs the same wall-clock horizon at finer block granularity. `--block-layout` can override either with an inline `{uid:dur,…}` / `d1,d2,…` spec or a CSV `duration` column. |

The simulation block emits one `scenario`, one `scene` (implicit),
one `phase` (implicit), one `stage` with `chronological: True`, and
N blocks of the chosen layout. `chronological: True` is **required**
— `commitment_lp.cpp` early-returns for non-chronological stages,
leaving `status`/`startup`/`shutdown` columns unbuilt and breaking
every UserConstraint that references them.

---

## 6. The gtopt element catalogue: every class plexos2gtopt emits

`plexos2gtopt` does **not** introduce a parallel modelling surface —
every PLEXOS object lands on a gtopt-native LP class. The full set
(observed on the CEN PCP 2026-04-22 weekly bundle):

| gtopt class | Count on PCP weekly | PLEXOS source | LP role |
|---|---|---|---|
| `Bus` | 247 | `t_class[Node]` (class_id 22) | Power-balance node |
| `Generator` | 1 732 | `t_class[Generator]` (class_id 2) | Thermal (with `fuel`), renewable (Gen_Rating-driven), and hydro turbine (with `Storage` membership) |
| `Fuel` | 221 | `t_class[Fuel]` (class_id 4) + virtual unit-price fuel | `Generator.fuel` reference, drives `gcost = HR · price + VOM + transport` |
| `Emission` | (when present) | `Fuel.CO2 Production Rate` or `Emission→Fuel` Production Rate | Per-fuel CO₂ factor |
| `Battery` | 36 | `t_class[Battery]` (class_id 7) | Energy storage; gtopt auto-instantiates `<bat>_gen` discharge + `<bat>_load` charge |
| `Line` | 317 | `t_class[Line]` + Node-from/to memberships | Transmission with PWL losses; soft/hard cap pair (`tmax_normal_ab` / `tmax_ab`) drives the overload band |
| `Demand` | 127 | synthesised one per non-zero column of `Nod_Load.csv` | Hourly load curtailable at `Region.VoLL` ($/MWh) |
| `Reservoir` | 12 | `t_class[Storage]` (class_id 8), after pondage demotion | Hydraulic storage state; carries `emin`/`emax`/`eini`/`efin`/`spillway_cost` |
| `Junction` | 32 | synthesised one per Storage / sink / `<src>_ocean` drain | Hydraulic balance node; carries `drain`/`drain_capacity`/`drain_cost` for terminal cascades |
| `Waterway` | 16 | `t_class[Waterway]` (class_id 9), minus turbine arcs (built into Turbine) and collapsed Vert arcs (onto Junction.drain) | Physical spillage / scheduled bypass / forced-flow arcs |
| `Turbine` | 35 | `Generator` ∩ `Storage` membership | Hydro generator with built-in waterway flow arc (`junction_a`/`junction_b`) + power conversion; tail storage → `junction_b` |
| `Flow` | 16 | natural-inflow rows in `Hydro_WaterFlows.csv` | Per-block discharge injected into a Junction |
| `ReserveZone` | 12 | `t_class[Reserve]` | Up/down reserve product (RS = raise, LW = lower), with `urreq`/`drreq` per block |
| `ReserveProvision` | 393 | Reserve → Generator/Battery memberships | Per-generator eligibility + per-block `ur`/`dn` provision column; SSCC BESS rows are emitted per (battery, `*_BESS` zone) |
| `Commitment` | 1 578 | `Generator` thermal subset + auto-promoted hydro min/max UCs | Unit-commitment binaries (`status`/`startup`/`shutdown`) with `pmin`/`pmax`/`ramp`/`min_up_time`/`max_starts` |
| `DecisionVariable` | 44 | `t_class[Decision Variable]` (class_id 72) + synthesized `alpha_fcf` | Free continuous LP column with optional bounds + cost; referenced via `decision_variable("X").value` |
| `Plant` | (when present) | synthesised from multi-fuel-band generator families | Native plant-cap row `Σ generation ≤ pmax` per (stage, block) — replaces the legacy `PlantCap_*` UCs |
| `FlowRight` | (when present) | `Hydro_AntucoBounds.csv` (Laja) | Soft irrigation / environmental flow envelope on a Junction |
| `UserConstraint` | 3 595 (across 8 families, see §7) | `t_class[Constraint]` + synthesised hydro / plant / FCF rows | Generic AMPL-style LP row binding any combination of LP variables |
| `BoundaryCut` | 1 | `Hydro_StoWaterValues.csv` | End-of-horizon FCF terminal-value cut on reservoir end volumes |

### Concepts that do not have a direct gtopt match

* PLEXOS **`Region` / `Zone` / `Area`** — folded into informational
  `bus.area` tags; the per-region `Region.VoLL` is collapsed to
  `model_options.demand_fail_cost` when consistent across regions
  (CEN PCP ships 467.19 $/MWh).
* PLEXOS **`Horizon` / `Chronology`** — collapsed to the static block
  layout described in §5.
* PLEXOS **`Variable` / `Definition`** symbolic scenarios — replaced
  by gtopt's explicit `scenario_array` (single scenario in CEN PCP).
* PLEXOS **wheeling / contract / charge / tariff** classes — skipped
  with a debug log (no current gtopt equivalent).

---

## 7. UserConstraint family taxonomy and PAMPL emission

Every PLEXOS `Constraint` object lands on the gtopt **AMPL-style
`UserConstraint`** primitive, which `gtopt`'s
`include/gtopt/constraint_parser.hpp` parses into a single LP row of
the form `Σ_i c_i · element(name).accessor [op] rhs` per
(stage, block). The accessor catalogue
(`source/{generator,line,battery,reserve_provision,waterway,reservoir,decision_variable,commitment}_lp.cpp`)
exposes:

| Element | Accessors registered by `add_ampl_variable` |
|---|---|
| `generator(N)` | `.generation` |
| `line(N)` | `.flow` (= `flowp − flown`) |
| `battery(N)` | `.charge`, `.discharge`, `.energy` |
| `reserve_provision(N)` | `.up`, `.dn` |
| `waterway(N)` | `.flow` |
| `reservoir(N)` | `.efin` |
| `decision_variable(N)` | `.value` |
| `commitment(N)` | `.status`, `.startup`, `.shutdown` |

`parsers._DIRECT_COEFFS` is the routing table from a PLEXOS property
kind (`Generation Coefficient`, `Flow Coefficient`, …) to the
(`element_class`, `accessor`, `name_template`) tuple gtopt resolves
at LP-build time. `Fuel.Offtake Coefficient` and
`Reserve.Provision Coefficient` use *expansion* rules — the LHS
expands to a sum over every Generator the Fuel feeds (or the Reserve
covers).

### Family routing (single source of truth: `uc_families.py`)

Every constraint name is routed to exactly one family (first match
wins, default = `operational`):

| Family | Match rule | Typical content |
|---|---|---|
| `config_exclusivity` | name ends in `_Uniq` or contains `ConfTG` | Combined-cycle turbine mutexes (at most one config committed; HARD) |
| `gas_offtake` | starts with `Gas_MaxOp` | Daily gas/GNL fuel-operation caps (soft, day-scoped) |
| `commitment` | ends in `_starting` or `== NorthSecurity` | Unit-commitment scheduling rows |
| `reserve` | matches `CTF`/`CSF`/`CPF`/`RegRange`/`Provision`/`MinUnits`/`MAXCSF`/`SSCC` or starts with `Special` | Ancillary-services / reserve products |
| `security` | starts with `SD_`, contains `Security`, or matches `^\d` | N-1 / contingency security rows (mostly inactive) |
| `comparison` | contains `Comparison` | PLEXOS↔gtopt validation rows (diagnostic, inactive) |
| `terminal_value` | starts with `FCF` or `alpha` | End-of-horizon future-cost (FCF) rows |
| `operational` | default | Genuine generation/flow floors, ramps, caps |

`gtopt_writer.write_user_constraint_pampl` emits one
`<output_dir>/uc_<family>.pampl` per non-empty family and wires them
into the planning JSON via `system.user_constraint_files = [...]`.
Constraints carrying a per-block `rhs` profile (e.g. PLEXOS
`RHS Day` overrides) stay inline in `system.user_constraint_array`
because gtopt only consumes per-block RHS via the JSON schedule
field.

### Family counts on CEN PCP weekly 2026-04-22 (after MIP-mode CLI defaults)

The post-conversion comparison classifies the converted UCs by
family (see §10), and is shown here for the canonical PCP weekly:

| Family | gtopt JSON | PLEXOS source | Δ |
|---|---:|---:|---:|
| `config_exclusivity` | 129 | 129 | 0 |
| `gas_offtake` | 520 | 520 | 0 |
| `commitment` | 17 | 17 | 0 |
| `reserve` | 420 | 420 | 0 |
| `security` | 1 372 | 1 372 | 0 (mostly inactive) |
| `comparison` | 104 | 104 | 0 (inactive) |
| `terminal_value` | 9 | n/a (converter-synthesised) | — |
| `operational` | 1 025 | 1 025 | 0 |

The `terminal_value` family carries the FCF cut + `alpha_fcf`
encoding (see §8). The `--pampl-uc-mode` CLI flag toggles between
`hard` (PLEXOS-stamped penalty per row, default), `soft` (force a
slack penalty on EVERY row at `--default-uc-penalty`), and `off`
(drop UCs entirely — diagnostic). `--pampl-uc-only` /
`--pampl-uc-off` provide per-family leave-one-out / keep-one-only
controls for isolating which family over-constrains the MIP.

### Soft-penalty policy

Two centrally-computed penalties drive every soft surface:

* **`soft_penalty_cost`** (used by `Generator.pmin_fcost` forced
  floors, EL=1 line soft caps, and the soft line-overload band):

  ```
  soft_penalty = min(max(gcost) + 1, min(demand.fcost) - 1)
  ```

  `max(gcost) + 1` makes running a unit STRICTLY cheaper than
  leaving its forced floor unserved, the `min(VoLL) - 1` cap keeps
  the penalty below the cheapest unserved-demand cost so load-
  serving always outranks it. CLI override: `--soft-penalty-cost`.

* **`--water-fail-cost`** (default 10 $/(m³/s·h)): uniform shortfall
  penalty on every soft hydro obligation — soft `FlowRight`s
  (Filt_Laja, Caudal_Eco_*, Riego_*, Ext_*), soft `discharge_*min`
  UC floors, and natural-inflow slack columns. Matches plp2gtopt's
  default so PLEXOS- and PLP-derived JSONs use identical water-
  obligation pricing.

---

## 8. Hydro topology, terminal value, and the FCF cut

`extract_reservoirs` + `extract_waterways` + `extract_junctions` +
`extract_turbines` together reconstruct PLEXOS's `Storage` /
`Waterway` cascade into gtopt's three-class topology
(`Junction` + `Reservoir` + `Waterway`/`Turbine`). The key
transformations:

* **Pondage demotion.** PLEXOS encodes every cascade balance point
  as a `Storage` object (B_C_Isla, B_Maule, Post_Antuco, …) even
  when it has no real storage. Reservoirs with every volume / cost /
  penalty field zero and no turbine `main_reservoir` reference are
  demoted to **Junction-only**: 52 → 12 on the CEN PCP weekly bundle.
* **Vert spillway collapse.** Every `Vert_<src>` waterway feeding an
  `<src>_ocean` sink is collapsed onto `Junction.drain` (with the
  source storage's `Max Flow` + `Max Flow Penalty` → `drain_capacity`
  + `drain_cost`). This drops 35+ synthetic waterways and 30+ ocean
  sink junctions on CEN PCP weekly. `--vert-routing cascade` keeps
  the legacy "Vert feeds next cascade junction" routing; default
  `ocean` drains.
* **Built-in turbine waterway.** Each `Turbine` carries its own
  flow arc (`junction_a` = reservoir, `junction_b` = tail storage)
  + power conversion (`production_factor = fp_med`), eliminating
  synthetic penstock waterways (+35) and terminal-ocean junctions
  (+4).
* **RoR generators** (hydro `Generator` with NO `Storage` membership,
  ~99 on PCP) are **left as plain `Generator` with per-block `pmax`
  profile** rather than re-built into a water topology — the
  per-block pmax IS the water-driven dispatch envelope.

### End-of-horizon future-cost cut

`Hydro_StoWaterValues.csv` is parsed into a `BoundaryCutSpec`:

* `FCF` row → the cut intercept ($);
* every other row → a reservoir water value ($/GWh).

The converter materialises **two parallel encodings** of the same
cut so both SDDP and the monolithic LP value terminal storage:

1. `boundary_cuts.csv` — gtopt's native SDDP cut file
   (`scene,rhs,<reservoirs>` rows, coefficients = `-water_value`),
   wired via `simulation.boundary_cuts_file` and
   `options.monolithic_options.boundary_cuts_file`.
2. `alpha_fcf` — a single `DecisionVariable` at the last block
   (`block` scope, `cost_type = energy`, cost = 1) + a
   `FCF_future_cost` UserConstraint
   `alpha_fcf + Σ_r water_value_r · reservoir(r).efin >= FCF` in
   the `terminal_value` family. The monolithic LP only binds this
   `alpha_fcf` row; the boundary-cuts CSV path is inert under
   monolithic.

The `alpha_fcf` column is α-rebased by subtracting the cut value
evaluated at each reservoir's terminal target (`efin` when set,
midpoint(emin, emax), or `eini`) so the cost-to-go centres on ~0
at the optimum and the LP objective stays well-conditioned. The
column scale `--fcf-scale-alpha` (default 1000, matched to
`scale_objective`) keeps the alpha LP cost in the same magnitude
range as every other objective term.

---

## 9. Loss model — adaptive piecewise-linear

`Line.loss_pwl_layout` and `loss_envelope` are emitted per-line by
`gtopt_writer._apply_dynamic_loss_layout`, driven by a single
budget knob `--loss-error-pct` (default `0.01` = 1%):

* The **cube-root rule** `K_i ∝ L_max,i^(1/3)` (with
  `L_max,i = R · fmax²`) allocates per-line segment count so the
  worst-case system-wide signed mean error stays within the budget.
* Per-line `K_i ∈ [2, --nseg-losses]` (ceiling defaults to 6 in
  adaptive mode, 4 in uniform mode).
* `--loss-pwl-layout dynamic` (default) gives heavy lines `midpoint`
  (negative bias secant chords, accuracy-first) and the rest
  `uniform` (positive-bias secant chords, presolve-eliminable for
  ~2× faster solve). The two bias directions cancel system-wide.
* `--loss-extend-overload` (default OFF) extends each soft-cap
  line's PWL envelope from `[0, tmax_normal]` to
  `[0, headroom × tmax_normal]`. The trade-off (4× worse PWL
  fidelity in the normal band vs. accuracy on a band the LP rarely
  reaches) is documented inline in `main.py` and remains OFF for
  routine runs.

On CEN PCP weekly (measured against an all-midpoint baseline): the
dynamic layout cuts total LP loss segments by 30 % (1 200 → 839),
LP solve time by 74 % (688 s → 182 s), kappa from 2.8 × 10⁸ to
1.1 × 10⁸, and the LP-internal-vs-analytic loss gap from 5.1 % to
2.1 %; operational cost vs PLEXOS goes from −3.32 % to −2.84 %.

A complementary line-cap policy ships out of the box:

* **EL=2 ("Always enforce")** → plain hard cap at the rating.
* **EL=1 ("Voltage enforce")** → automatic soft-cap pair
  `tmax_normal_* ← rated`, `tmax_* ← 3× rated`,
  `overload_penalty = soft_penalty_cost` ($/MWh). This is the
  `augment_el1_with_soft_caps` step at the end of `build_planning`;
  it's the LP's priced escape valve when PLEXOS itself dispatches a
  line above its stated rating.
* **EL=0 ("Never enforce")** → `--el0-lines extended` (default):
  free up to the rating, penalised between rating and a headroom
  multiple, hard-capped at the headroom multiple. `--el0-lines
  strict` treats it like EL=2.
* **`--lift-line-caps <names>`** (default
  `Capricornio110->LaNegra110`): demote a curated set of radial
  step-down corridors to EL=0 with a wider free band (4× rating)
  and 10× hard cap.
* **`--auto-lift-lines`** runs a DC OPF (pandapower or
  `gtopt --no-mip`) on the first (scenario, block) and patches
  `enforce_level = 0` onto every line whose unlifted-OPF flow
  exceeds rating by ≥ THRESHOLD. Useful when the lift list isn't
  curated for a new bundle.

---

## 10. Validation — the post-conversion comparison

`_comparison.run_post_check(planning, case)` runs on every default
conversion (opt out with `--no-check`) and is the converter's
primary correctness gate. It prints four side-by-side tables and
returns the CRITICAL finding count as the exit code so CI can gate
on it.

### Table 1 — Conversion stats (per-class base stats)

Plp2gtopt-style baseline counters: buses, lines, generators (hydro
/ thermal / renewable / total), demands, batteries, fuels,
reservoirs, junctions, waterways, turbines, flows, reserve
zones/provisions, commitments, decision variables, plants, plus
the per-family UC counts.

### Table 2 — Element Comparison (PLEXOS vs gtopt vs Δ)

For every emitted entity class, the parsed PLEXOS count, the
gtopt-JSON count (read back by
`gtopt_check_json._info.compute_counts` — the single authority — so
gtopt-side numbers stay honest), and the difference. The contract
is **Δ = 0** for every class except where the converter
intentionally drops or synthesises:

* Boundary state variables: PLEXOS water-value rows for non-bundle
  reservoirs (e.g. PILMAIQUEN) are excluded with a "discarded" note;
* Turbines: tied to per-`Storage` hydro generators that pass the
  pmax-positive filter;
* Junctions: pondage-demoted reservoirs + sink/ocean nodes.

### Table 3 — User Constraint families

Per-family table (PLEXOS-side count vs gtopt-emitted vs Δ, plus
inactive count) using the `uc_families.py` taxonomy. Converter-
synthesised families (`terminal_value`) are excluded from the Δ
total because they have no PLEXOS source. The conversion funnel
(`UserConstraintStats`) is also reported:

```
raw → empty_lhs_dropped → base_emitted → hydro_synthesized → plant_cap_synthesized
```

### Table 4 — Global Indicators

PLEXOS vs gtopt aggregates: installed capacity (hydro / thermal /
renewable), peak/average/total demand, total energy, total water
(hm³), reservoir storage (Σ), initial volume, FCF intercept,
reserve requirement (up/dn), gen-cost statistics (avg/min/max).

### Table 5 — Drop funnel + Input-file ledger

For every raw PLEXOS class the converter drops or demotes
(`Storage` → Reservoir vs Junction, `Generator` with pmax=0,
`Line` with EL=0/1/2 split, …), the funnel shows how many
instances landed in each bucket so the comparison can attribute
every gap to a reason. The input-file ledger lists every CSV the
converter is expected to read with the indicator it drives — a
zero in the linked indicator flags an unread file.

### Table 6 — JSON structural validation

`gtopt_check_json._checks.run_all_checks` runs the gtopt JSON
validator over the produced planning: schema sanity, dangling-
reference detection, every emitted UC term resolvable through
`add_ampl_variable`, every Junction/Waterway endpoint present,
etc. Findings are split into CRITICAL / WARNING / NOTE; the
process exit code is the CRITICAL count.

### Strict UserConstraint resolution

By default any UC term that references an element name not in the
emitted-name set raises `UnresolvedConstraintReferenceError` with
the **full list** of offending references + closest-match hints —
the converter refuses to ship a JSON with dangling refs (matches
gtopt's strict JSON parser). `--lax-uc-refs` downgrades this to a
warning + silent per-term drop for diagnostic / iterative parser
work.

---

## 11. End-to-end example: CEN PCP 2026-04-22 weekly

This is the canonical reference bundle. The complete pipeline:

```bash
# 1. Convert (default == PLEXOS-native 111-block layout, MIP commitments,
#              auto soft EL=1 caps, default --loss-pwl-layout dynamic):
plexos2gtopt \
  -i support/plexos/pcp_2026-04-22/PLEXOS20260422.zip \
  -o gtopt_PLEXOS20260422

# 2. Solve with CPLEX MIP, write full Parquet output stream:
gtopt gtopt_PLEXOS20260422/PLEXOS20260422.json \
  --solver cplex --output-dir gtopt_PLEXOS20260422/output

# 3. Compare against the PLEXOS reference solution
#    (auto-discovers RES{date}.zip[.xz] sibling):
plexos2gtopt --compare gtopt_PLEXOS20260422/PLEXOS20260422.json \
  support/plexos/pcp_2026-04-22/PLEXOS20260422.zip
```

### What the converter produces (observed)

The conversion of CEN PCP weekly 2026-04-22 emits (from the post-
conversion comparison and the resulting JSON):

| gtopt array | Count |
|---|---:|
| `bus_array` | 247 |
| `generator_array` | 1 732 |
| `line_array` | 317 |
| `demand_array` | 127 |
| `battery_array` | 36 |
| `fuel_array` | 221 (incl. virtual unit-price fuel when needed) |
| `reservoir_array` | 12 (52 → 12 after pondage demotion) |
| `junction_array` | 32 |
| `waterway_array` | 16 (51 → 16 after Vert collapse + turbine built-in) |
| `turbine_array` | 35 |
| `flow_array` | 16 |
| `reserve_zone_array` | 12 |
| `reserve_provision_array` | 393 |
| `commitment_array` | 1 578 (thermal + auto-promoted hydro min/max) |
| `decision_variable_array` | 44 (including `alpha_fcf`) |
| `user_constraint_array` | 6 (per-block-RHS only) |
| `user_constraint_files` | 8 (`uc_<family>.pampl`) |
| `boundary_cuts_file` | `boundary_cuts.csv` (1 cut, 8 reservoir slopes) |

### Simulation block

```
block_array : 168 blocks (under --horizon-mode hourly)
              or 111 blocks (default --horizon-mode plexos, t_phase_3 layout)
stage_array : 1 (chronological)
scenario_array : 1
```

### Solve statistics (CEN PCP weekly, observed)

| Mode | Variables | Rows | Solver | Wall time | Status | Obj (unscaled) |
|---|---:|---:|---|---:|---|---:|
| Default LP (Ruiz scaling, default loss layout) | 695 711 | 497 230 | CPLEX 22.1.1 dual simplex | 174 s | optimal | 23.618 M (scaled obj × `scale_objective` 1000) |
| LP-relax (`--lp-relax`, equilibration=ruiz, K=8 loss, full UC set) | 790 616 | 497 230 | CPLEX dual simplex | 170 s | optimal | 22.754 M |
| MIP (default since 2026-05-23, no `--lp-relax`) | ~790 k | ~497 k | CPLEX MIP | ~180 s | optimal | within ~1% MIPGap |

The MIP-vs-LP-relax gap is intentionally small because the same
soft slacks make the LP-relax already feasible; what MIP recovers
is the cycling / startup cost that the LP-relaxed commitment
misses. Empirically (see feedback memory): dropping the LP-relax
closed ~7 pp of the PLEXOS-vs-gtopt dispatch-cost gap on CEN PCP,
moving `NUEVA_RENCA-TG+TV_GN_A` from 19 GWh/week (LP) to 33
GWh/week (MIP, vs PLEXOS 40 GWh/week).

### Step-3 (`compare_with_plexos`) comparison surface

`scripts/plexos2gtopt/compare_with_plexos.py` is the heavier
side-by-side comparator (it reads back both the PLEXOS `.accdb`
solution and the gtopt Parquet output). Its rendered tables
include:

* **Solution totals** — block count, hours covered, consumer
  demand [MWh], BESS charge / discharge / round-trip loss,
  line losses (analytic vs LP-internal vs bus-residual),
  generation [MWh], unserved [MWh], operational $ broken into
  dispatch + start/shutdown cost.
* **Per-unit SRMC** vs PLEXOS pid-137 (per-unit short-run
  marginal cost weighted by dispatch).
* **Per-bus LMP** vs PLEXOS price.
* **Per-line** flow / loss / utilisation diffs.
* **Per-technology generation** (`_classify_tech` + PLEXOS category map).
* **Reservoir volumes** start / end / per-block.
* **Battery operation** (charge / discharge / round trip).
* **Commitment** comparison (units started / shutdown / on, per unit).
* **UserConstraint drilldown** — per-family penalty totals + per-
  block breakdowns for diagnosing which UCs the LP paid to violate.

A typical PCP weekly run prints all of these as Rich tables; the
single-line summary "PLEXOS objective $X vs gtopt $Y; Δ $Z (Δ%)"
is the at-a-glance gate.

---

## 12. CLI reference (canonical knobs)

The complete list lives in `scripts/plexos2gtopt/main.py`; the
following groups together the operationally important knobs.

### Conversion mode

```
plexos2gtopt INPUT [-o OUT] [-l LOG_LEVEL]
plexos2gtopt --info  INPUT
plexos2gtopt --validate INPUT
plexos2gtopt --compare GTOPT_JSON INPUT
```

### Horizon

| Flag | Default | Effect |
|---|---|---|
| `--horizon-mode {plexos,hourly}` | `plexos` | `plexos` reproduces the PLEXOS-solved `t_phase_3` 111-block layout (needs `RES<date>.zip`); `hourly` emits `--horizon-days × 24` uniform blocks. |
| `--horizon-days N` (1..7) | inferred from `Horizon` name (`*_7d` → 7) | Number of consecutive days. |
| `--plexos-solution-accdb PATH` | auto-discovered | Override of the .accdb path (block layout + optional table cache). |
| `--block-layout CSV-OR-SPEC` | unset | User-defined layout for `plexos` mode when no .accdb is available. CSV (`duration` column) or inline `{uid:dur,…}` / `d1,d2,…`. |

### User constraints (PAMPL emission)

| Flag | Default | Effect |
|---|---|---|
| `--uc-emit {pampl,inline}` | `pampl` | Modular `uc_<family>.pampl` files (default) vs inline `user_constraint_array`. |
| `--pampl-uc-mode {hard,soft,off}` | `hard` | Keep PLEXOS penalties (`hard`), force a slack on every row (`soft`), drop UCs entirely (`off`). |
| `--default-uc-penalty F` | unset | Fallback penalty for PLEXOS rows without `Penalty Price`. |
| `--pampl-uc-only FAM1,FAM2,…` | unset | Emit ONLY these families. |
| `--pampl-uc-off FAM1,FAM2,…` | unset | Drop these families (leave-one-out diagnostic). |
| `--lax-uc-refs` | OFF | Downgrade unresolved-ref errors to warnings + silent drop. |

### Soft-penalty policy

| Flag | Default | Effect |
|---|---|---|
| `--soft-penalty-cost F` | computed `min(max(gcost)+1, min(VoLL)−1)` | Override the shared penalty for forced-floor `pmin_fcost` + soft line overload + EL=1 soft cap. |
| `--water-fail-cost F` | 10 | Per-flow penalty $/(m³/s·h) on soft FlowRights / `discharge_*min` floors / Flow slack columns. |
| `--spill-fcost F` | PLEXOS `Max Flow Penalty` | Override per-flow spill cost on every `Vert_*` waterway. |
| `--spill-fcost-scale F` | 1.0 | Multiplicative scale on the `Vert_*` `Max Flow Penalty`. |

### Hydro topology + FCF

| Flag | Default | Effect |
|---|---|---|
| `--vert-routing {ocean,cascade}` | `ocean` | Vert_* spillway destinations. |
| `--reservoir-spillway {basic,strict}` | unset | `basic`: set `Reservoir.spillway_cost = 0`. `strict`: also disable every duplicate spillway path. |
| `--soft-efin-reservoirs A,B,…` | `L_Maule` | Emit efin as a soft slack at the reservoir's water value for the listed reservoirs; HARD `vol_end >= efin` for all others. |
| `--emin-eod-day1` / `--no-emin-eod-day1` | OFF | Hard hour-24 emin clamp (operational floor from `Hydro_MinVolume.csv`). Default OFF — disabling brings PLEXOS-vs-gtopt water-value duals in line. |
| `--battery-efin-pin` / `--no-battery-efin-pin` | ON | Pin every battery's end-of-horizon SoC to its initial SoC. |
| `--fcf-scale-alpha F` | 1000 (= `scale_objective`) | Column scale for `alpha_fcf` LP variable. |

### Loss model

| Flag | Default | Effect |
|---|---|---|
| `--loss-pwl-layout {dynamic,uniform,midpoint,tangent,equal_error}` | `dynamic` | Per-line PWL layout (see §9). |
| `--loss-error-pct F` | 0.01 (1 %) | Target absolute system-loss-error budget as a fraction of Σ Lmax,i. |
| `--nseg-losses N` | adaptive ceil 6 / uniform 4 | Segment-count ceiling (adaptive) or count (uniform). |
| `--loss-extend-overload` | OFF | Extend each soft-cap line's PWL envelope across the soft-headroom band. |
| `--loss-tangent-lines NAME,…` | unset | Per-line escape hatch to force `tangent` layout. |
| `--nseg-tangent` / `--nseg-uniform` | 6 / 4 | Per-layout segment counts. |

### Line cap policy

| Flag | Default | Effect |
|---|---|---|
| `--el0-lines {extended,strict}` | `extended` | EL=0 line treatment (soft headroom band vs hard cap at rating). |
| `--lift-line-caps NAMES` | `Capricornio110->LaNegra110` | Demote to EL=0 with wider free band (4×) + 10× hard cap. Empty string activates a global soft-EL=1 mode. |
| `--auto-lift-lines [THR]` | unset | After write, run DC OPF and patch `enforce_level = 0` on lines whose unlifted flow exceeds THR × rating. |
| `--auto-lift-engine {pandapower,gtopt}` | `pandapower` | OPF engine for the auto-lift detector. |

### Commitment mode

| Flag | Default | Effect |
|---|---|---|
| `--lp-relax` | OFF (since 2026-05-23) | Emit `Commitment.relax = true` on every commitment, LP-relaxing the binaries. Default is MIP. |

### Curve-fit / debug

| Flag | Effect |
|---|---|
| `--use-plexos-commit` | Per-period pmax overlay = PLEXOS `Units Generating`. |
| `--use-plexos-gen-cap` | Per-period pmax hard-cap = PLEXOS `Generation` (hydro turbines only). |
| `--use-plexos-efin` | Reservoir.efin = PLEXOS End Volume. |
| `--no-check` | Skip the post-conversion comparison + JSON validation. |

---

## 13. Output layout

```
gtopt_PLEXOS20260422/
├── PLEXOS20260422.json              # planning JSON (options + simulation + system)
├── PLEXOS20260422.provenance.json   # per-element source / units / transforms
├── boundary_cuts.csv                # FCF terminal cut (scene, rhs, <reservoirs>)
├── uc_commitment.pampl              # one per non-empty UC family
├── uc_comparison.pampl
├── uc_config_exclusivity.pampl
├── uc_gas_offtake.pampl
├── uc_operational.pampl
├── uc_reserve.pampl
├── uc_security.pampl
├── uc_terminal_value.pampl
├── solvers/
│   └── cplex.prm                    # curated MIPGap=0.01, Gomory=2, …
├── plexos_cache/                    # mdb-exported .accdb tables (only when --use-plexos-* is set)
│   └── *.csv.zst
├── Generator/                       # Parquet schedule tree (one column per object)
│   ├── pmax.parquet
│   ├── pmin.parquet
│   ├── gcost.parquet
│   └── …
├── Demand/lmax.parquet
├── Line/{tmax_ab,tmin_ab,in_service,…}.parquet
├── Reservoir/{inflow,emin,emax,…}.parquet
├── Reserve*/, Battery/, Waterway/, Junction/, Turbine/, Flow/, …
```

The whole tree is gtopt-native: `gtopt <case>.json` (with the
solver of your choice) runs directly on it. The `provenance.json`
sidecar documents the conversion mapping for every emitted entity
class, so a reader can answer "where does `Generator.gcost` for unit
X come from?" without re-reading the converter source.

---

## 14. Limitations + known divergences

* **Single scenario, single stage.** CEN PCP is by design one
  operating week; multi-scenario / multi-stage stitching is left to
  upstream tooling (the gtopt SDDP / cascade solvers stitch the
  per-week planning files themselves).
* **Round-trip is one-way.** gtopt → PLEXOS export is not supported
  (out of scope).
* **Wheeling / contract / charge / tariff** classes are skipped.
* **Multi-band cost.** PLEXOS's `t_band` piecewise costs are
  collapsed to the first band for the common case; quadratic /
  multi-segment forms (e.g. 118-Bus benchmark) are honoured via
  `pmax_segments` + `heat_rate_segments` + virtual unit-price fuel.
* **Region.Load vs Node.Load.** The PLEXOS comparison uses `Node.Load`
  (the LP's actual demand at each bus) — using `Region.Load` would
  inflate the comparison by 7–8 % because the latter folds in
  battery charging and losses (see
  `feedback_plexos_demand_scope` memory).
* **FCF obj_constant.** CPLEX adds the ~$931 M FCF intercept
  post-solve as a constant — PLEXOS never sees it, so it must not
  be used to "explain away" PLEXOS-vs-gtopt cost gaps; that
  rationale belongs only to CPLEX's rebased bound / incumbent
  (`feedback_fcf_obj_constant_not_a_gap_explanation`).

---

## 15. Related references

* `scripts/plexos2gtopt/DESIGN.md` — the original v0 design (CSV
  inventory, entity mapping, time model). Most P0/P1/P2/P3 items
  are implemented; the doc here is the operational truth.
* `docs/scripts/plp2gtopt.md` — sibling PLP converter; shares the
  proactive-tests + per-class writer architecture + `_comparison`
  contract.
* `docs/scripts/sddp2gtopt.md` — sibling PSR-SDDP converter; the
  PLEXOS layout was modelled after it (XML + per-class CSV).
* `docs/formulation/mathematical-formulation.md` — gtopt LP/MIP
  formulation; the canonical reference for what every emitted
  element does at LP-build time.
* `include/gtopt/constraint_parser.hpp` — the AMPL-style
  expression parser that consumes `UserConstraint.expression`.
* `include/gtopt/{generator,line,battery,reservoir,turbine,waterway,reserve_provision,decision_variable,commitment}.hpp`
  + matching `_lp.cpp` — the gtopt LP class catalogue (types,
  accessors registered via `add_ampl_variable`, schedule fields).
* `support/plexos/pcp_2026-04-22/` — the canonical reference
  bundle the converter is exercised against in every release.
