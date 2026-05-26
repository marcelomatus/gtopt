# plexos2gtopt — Design Doc (v0)

Convert a **CEN PCP daily PLEXOS bundle** into a gtopt JSON planning,
mirroring the role `plp2gtopt` plays for PLP and `sddp2gtopt` plays
for PSR-SDDP. Same CLI ergonomics, same JSON + Parquet output, same
`gtopt_writer` integration; only the input dialect changes.

## 1. Scope & non-goals

### MVP target (P1)

- **In:** one CEN PCP daily PLEXOS bundle (`PLEXOS{YYYYMMDD}.zip` or
  its two inner zips `DATOS{date}.zip[.xz]` + `RES{date}.zip[.xz]`)
  vendored at `support/plexos_pcp_2026-04-22/`.
- **Out:** one gtopt planning JSON containing
  - 1 `scenario`, 1 `scene`, 1 `phase`, 1 `stage` (the operational day)
  - 24 chronological `block` entries of `duration: 1.0` h
  - `bus_array` from `t_class[Node]`
  - `generator_array` from `t_class[Generator]` + `Gen_*.csv` schedules
  - `line_array` from `t_class[Line]` + `Lin_*.csv`
  - `demand_array` from `Nod_Load.csv` (wide CSV, one column per bus)
  - basic `fuel_array` + `reservoir_array` + `battery_array` stubs
- This exactly mirrors what `tools/ucjl2gtopt.py` produces for UC.jl
  benchmarks: single scene / single phase / chronological-block run.

### Non-goals (v0)

- Reading the `.accdb` solution back (that is `gtopt_compare`'s job).
- Multi-day rolling horizons. The CEN PCP bundle is one operating day
  by design; multi-day cases require user-driven stitching.
- Round-trip (gtopt → PLEXOS).
- Wheeling / contract / charge / tariff PLEXOS classes — they have no
  current gtopt equivalent and are skipped with a debug log.

## 2. Input survey

### PLEXOS XML object database

`DBSEN_PRGDIARIO.xml` (~36 MB, MasterDataSet schema) is the **typed
schema**. The CEN PCP daily bundle exposes 96 enabled `t_class`
entries and ~4 500 `t_object` instances across:

| table | role |
|---|---|
| `t_class` | object classes (Generator, Fuel, Line, Node, …) — 96 |
| `t_object` | concrete instances (4 500+) |
| `t_attribute` | scalar metadata on a class (e.g. Latitude) |
| `t_property` | dynamic properties (Max Capacity, Heat Rate, …) |
| `t_collection` | (parent_class, child_class) relation definitions |
| `t_membership` | concrete (parent_object, child_object) edges |
| `t_data` | numeric property values, by membership × property |
| `t_band` | multi-band tariff / piecewise property segments |
| `t_text` | textual property values |
| `t_memo_data` | per-time-slice / per-hour overrides |
| `t_action` | operational events (outages, derates) |
| `t_message`, `t_unit` | units (MW, m³/s, $, …) |

XML namespace: `http://tempuri.org/MasterDataSet.xsd`.

Class IDs verified against the 2026-04-22 PCP bundle:

| class_id | name |
|---|---|
| 1 | System |
| 2 | Generator |
| 4 | Fuel |
| 7 | Battery |
| 8 | Storage (reservoir / pond) |
| 9 | Waterway |
| 10 | Emission |
| 22 | Node |
| (auto-discover) | Line, Reserve, Region, Zone, Constraint, … |

Class IDs higher than ~12 vary between CEN PCP releases, so the
parser auto-discovers Line / Reserve / Constraint by walking
`t_class.name` rather than hard-coding IDs.

### Per-class CSV inventory (2026-04-22 bundle)

| CSV | role | drives gtopt field |
|---|---|---|
| `Gen_Rating.csv` | per-(unit, hour) Pmax (MW) | `Generator.pmax` (per-block) |
| `Gen_MinStableLevel.csv` | per-unit Pmin (MW) | `Generator.pmin` |
| `Gen_HeatRate.csv` | per-unit heat rate (kJ/kWh or kcal/kWh) | `Generator.heat_rate` |
| `Gen_VOMCharge.csv` | per-unit variable O&M ($/MWh) | `Generator.gcost` |
| `Gen_StartCost.csv` | per-unit startup cost ($) | `Commitment.startup_cost` |
| `Gen_ShutDownCost.csv` | per-unit shutdown cost ($) | `Commitment.shutdown_cost` |
| `Gen_FixedLoad.csv` | per-unit must-run | `Generator.pmin_factor` |
| `Gen_FuelTransportCharge.csv` | per-unit fuel transport adder | folded into `gcost` |
| `Gen_UnitsOut.csv` | forced-outage schedule | `Generator.pmax_factor` |
| `Gen_IniGeneration.csv` | initial dispatch | `Generator.pini` |
| `Gen_IniHoursUp/Down.csv` | initial commitment status | `Commitment.initial_hours` / `initial_status` |
| `Gen_IniUnits.csv` | initial number of units online | `Generator.uini` |
| `Gen_Commit.csv` | commitment override | `Commitment.must_run` |
| `Gen_AuxUse.csv` | auxiliary consumption | folded into capacity |
| `Fuel_Price.csv` | monthly fuel prices ($/MMBtu) | `Fuel.price` |
| `Fuel_MaxOfftakeWeek.csv` | take-or-pay limits | `Fuel.max_offtake` |
| `Hydro_WaterFlows.csv` | natural inflow per reservoir (m³/s) | `Reservoir.inflow` |
| `Hydro_MaxVolume.csv` | reservoir max volume (hm³) | `Reservoir.emax` |
| `Hydro_MinVolume.csv` | reservoir min volume (hm³) | `Reservoir.emin` |
| `Hydro_InitialVolume.csv` | reservoir initial volume (hm³) | `Reservoir.eini` |
| `Hydro_StoWaterValues.csv` | water value ($/hm³) | `Reservoir.water_value` |
| `Hydro_MaxRampDay.csv` | daily ramp limits | (skipped in v0) |
| `Hydro_AntucoBounds.csv` | irrigation envelope (Laja) | `FlowRight` overlay (P4) |
| `Hydro_EfficiencyIncr.csv` | piecewise productibility | `Turbine.efficiency` (P3) |
| `Lin_MaxRating.csv` | line forward rating (MW) | `Line.tmax_ab` |
| `Lin_MinRating.csv` | line reverse rating (MW) | `Line.tmin_ab` |
| `Lin_Units.csv` | parallel-line count | `Line.capacity` factor |
| `Nod_Load.csv` | per-bus per-hour load (wide CSV) | `Demand.lmax` |
| `Res_Requirement.csv` | reserve requirements (MW) | `ReserveZone.urreq` |
| `Res_Timeslice.csv` | reserve time-slice mapping | `ReserveZone.timeslice` |
| `SSCC_Activation_BESS.csv` | BESS in ancillary services | `Battery.reserve_provision` |
| `ReserveUsageTxCompensation.csv` | reserve cross-zone allocation | (skipped in v0) |
| `BESS_IniValue.csv` | battery initial SOC | `Battery.eini` |
| `CFdata/CPF/*.csv` | per-unit ramp curves (MRU/MRD) | `Commitment.ramp_up`/`ramp_down` |

### CSV column layout

Two recurring layouts:

1. **Long (`NAME, YEAR, MONTH, DAY, PERIOD, BAND, VALUE`)** — used by
   `Gen_*.csv` and `Hydro_*.csv` per-unit time-series. `BAND` carries
   piecewise segments for multi-band heat rates / costs.
2. **Wide (`YEAR, MONTH, DAY, PERIOD, <bus_name>, <bus_name>, …`)** —
   used by `Nod_Load.csv`. One column per object, one row per hour.

`PERIOD` is 1-based hour-of-day (1..24). The CEN PCP bundle always
has 24 periods for a single calendar day; longer horizons are out of
scope for v0.

## 3. PLEXOS → gtopt entity mapping

| gtopt class | PLEXOS class / collection | CSV source | Naming-dialect rows | Notes |
|---|---|---|---|---|
| `Bus` | `t_class[Node]` (class 22) | — | (none — bus uses name only) | One bus per Node object |
| `Line` | `t_class[Line]` + `Node-from/to` memberships | `Lin_MaxRating.csv`, `Lin_MinRating.csv`, `Lin_Units.csv` | `line.tmax_ab=Max Flow`, `line.tcost=Wheeling Charge`, `line.overload_penalty=Overload Penalty` | Capacity = MaxRating × Units |
| `Generator` (thermal) | `t_class[Generator]` ∩ Fuel-membership ≠ ∅ | `Gen_Rating.csv`, `Gen_MinStableLevel.csv`, `Gen_HeatRate.csv`, `Gen_VOMCharge.csv`, `Fuel_Price.csv` | `generator.pmin=Min Stable Level`, `generator.pmax=Max Capacity`, `generator.heat_rate=Heat Rate`, `generator.fuel=Fuels`, `generator.emission_rate=Emission Coefficient` | `gcost = heat_rate × fuel_price + VOM + transport` |
| `Generator` (renewable) | `t_class[Generator]` ∩ Fuel-membership = ∅ | `Gen_Rating.csv` (becomes pmax_factor × pmax) | same as above | Use `GeneratorProfile` for per-hour availability |
| `Demand` | (synthetic, one per Node with non-zero load) | `Nod_Load.csv` (wide) | `demand.lmax=Load`, `demand.lmin=Firm Load` | One demand per bus column |
| `Battery` | `t_class[Battery]` (class 7) | `BESS_IniValue.csv`, `SSCC_Activation_BESS.csv` | `battery.emin=Min SOC`, `battery.emax=Max SOC`, `battery.eini=Initial SOC`, `battery.efin=Min End SOC` | Direct map; gtopt auto-instantiates discharge gen / charge demand |
| `Fuel` | `t_class[Fuel]` (class 4) | `Fuel_Price.csv`, `Fuel_MaxOfftakeWeek.csv` | `fuel.price=Price`, `fuel.heat_content=Heat Content` | Monthly Price collapsed to a single scalar (day-of-bundle month) |
| `Reservoir` | `t_class[Storage]` (class 8) | `Hydro_MaxVolume.csv`, `Hydro_MinVolume.csv`, `Hydro_InitialVolume.csv`, `Hydro_StoWaterValues.csv` | `reservoir.emax=Max Volume`, `reservoir.emin=Min Volume`, `reservoir.eini=Initial Volume`, `reservoir.efin=Min End Volume`, `reservoir.spillway_cost=Spill Penalty` | Volumes in hm³, water value in $/hm³ |
| `Junction` | (synthetic) | `Hydro_WaterFlows.csv` topology | — | Generated per reservoir confluence point |
| `Waterway` | `t_class[Waterway]` (class 9) | (topology in XML memberships) | `waterway.fmin=Min Flow` | Spillway + bypass channels |
| `Turbine` | `t_class[Generator]` ∩ Storage-membership ≠ ∅ | `Hydro_EfficiencyIncr.csv` | — | Hydro generator linked to reservoir |
| `FlowRight` | `Hydro_AntucoBounds.csv` + irrigation tables | (same) | — | Laja irrigation overlay; P4 deliverable |
| `ReserveZone` | `t_class[Reserve]` | `Res_Requirement.csv`, `Res_Timeslice.csv` | `model_options.reserve_shortage_cost=VoRS` | One zone per Reserve object |
| `ReserveProvision` | Reserve→Generator memberships | (per-unit eligibility from XML) | — | Eligibility flags + provision factor |
| `EmissionZone` | `t_class[Emission]` (class 10) | (currently disabled in CEN PCP) | `emission_zone.cap=Emission Cap`, `emission_zone.cap_cost=Emission Penalty`, `emission_zone.price=Carbon Price` | Optional; skipped if class disabled |
| `EmissionSource` | Emission→Fuel memberships | — | `emission_source.rate=Production Rate` | Per-fuel CO₂ coefficient |
| `Emission` | (per-Generator emission rate) | — | (canonical = `Generator.emission_rate`) | Optional; v0 derives from fuel-CO₂ when present |
| `Commitment` | `t_class[Generator]` thermal subset | `Gen_StartCost.csv`, `Gen_ShutDownCost.csv`, `Gen_IniHoursUp/Down.csv` | (UC-specific; no plexos dialect rows in 35-line subset) | Generated only for units with non-zero startup cost |
| `InertiaZone` / `InertiaProvision` | (not in CEN PCP today) | — | — | Skipped |
| `UserConstraint` (PAMPL) | `t_class[Constraint]` | — | — | Network / reserve coupling constraints (P5) |

### Concepts that do not map cleanly

- **PLEXOS `Region` / `Zone` / `Area`** — gtopt models reserve and
  emission zones explicitly but has no generic "region" concept; we
  fold these into `bus.area` tags (informational only).
- **PLEXOS `Horizon` / `Chronology`** — gtopt's stage/block layout is
  emitted statically by the writer (1 stage, 24 blocks for PCP).
- **PLEXOS `Production` / `Power2X`** — disabled in CEN PCP, ignored.
- **PLEXOS `Variable` / `Definition`** — symbolic scenarios; gtopt
  uses scenario_array directly. PCP single-scenario so no conversion.
- **Multi-band tariff (`t_band`)** — collapsed to first-band scalar
  in v0; piecewise-cost emission is a P3 item parallel to PLP's
  `cenpmax_parser` segment expansion.

## 4. Architecture

We adopt option **(c) — the `sddp2gtopt` hybrid layout** because:

1. PLEXOS input is structurally identical to PSR-SDDP: XML object
   database + per-class CSV time-series. Same parse-then-translate
   pattern fits naturally.
2. The entity layer (frozen dataclasses) cleanly separates "what
   PLEXOS exposes" from "what gtopt consumes", which we will need
   when CEN ships PLEXOS schema upgrades (the 96 enabled classes are
   a moving target — `class_id` re-use across releases is documented).
3. Multi-file packages make per-class unit testing trivial, matching
   the proactive-tests rule.
4. Single-file `tools/plexos2gtopt.py` was rejected because the
   PLEXOS XML reader alone (just topology + properties + memberships)
   is already ~400 lines; bolting the writers onto it produces an
   unmaintainable monolith.

```
scripts/plexos2gtopt/
├── __init__.py
├── __main__.py             # python -m plexos2gtopt
├── DESIGN.md               # this file
├── main.py                 # argparse + dispatch
├── plexos2gtopt.py         # convert_plexos_bundle / validate_plexos_bundle
├── plexos_loader.py        # bundle unpacker (handles .zip, .zip.xz, dir)
├── plexos_xml.py           # MasterDataSet XML reader (objects, classes, memberships, t_data)
├── plexos_csv.py           # CEN per-class CSV reader (long-format + wide-format)
├── entities.py             # frozen dataclasses for parsed entities
├── parsers.py              # per-class extractors (Node, Generator, Line, ...)
├── gtopt_writer.py         # gtopt JSON / Parquet emitter
├── info_display.py         # --info renderer
└── tests/
    ├── __init__.py
    ├── conftest.py
    ├── test_smoke.py       # CLI --help + --version
    ├── test_plexos_xml.py  # XML reader unit tests
    └── data/               # vendored mini-fixtures (synthetic)
```

Most writers can **not** be reused unchanged from `plp2gtopt` (unlike
sddp2gtopt) because PLEXOS uses native English field names that
already match gtopt's canonical schema via the `dialect: plexos`
rows in `share/gtopt/naming_dialects.json` — bypassing plp2gtopt's
Spanish-PLP renaming layer entirely.

## 5. Time model

CEN PCP is **24 hourly chronological blocks × 1 stage × 1 scenario**.
The writer emits exactly the same shape as `ucjl2gtopt`:

```json
{
  "simulation": {
    "scenario_array": [{"uid": 1, "probability_factor": 1}],
    "scene_array":    [{"uid": 1, "active": 1}],
    "phase_array":    [{"uid": 1, "first_stage": 0, "count_stage": 1}],
    "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 24, "active": 1}],
    "block_array":    [{"uid": h, "duration": 1.0} for h in 1..24]
  }
}
```

`PLEXOS_Param.xml` carries the run horizon (`Day Beginning`, `Period
Type`, etc.). We honour `Period Type = Hour` and `Step Count = 24`
unconditionally; any other horizon shape (sub-hourly, multi-day)
rejects with a clear error in v0.

## 6. Unit handling

The PLEXOS column-name aliases in `share/gtopt/naming_dialects.json`
already encode the canonical conversions; the writer applies them via
the dialect resolver at output time. Specific decisions:

| Quantity | PLEXOS column | gtopt field | gtopt unit |
|---|---|---|---|
| Generator dispatch | `Max Capacity`, `Min Stable Level` | `pmax`, `pmin` | MW |
| Generation cost | (synthesised from `Heat Rate × Fuel Price + VO&M`) | `gcost` | $/MWh |
| Generator emission rate | `Emission Coefficient` | `emission_rate` | tCO₂/MWh |
| EmissionSource rate | `Production Rate` | `rate` | t/MWh |
| Reservoir storage | `Max Volume`, `Min Volume`, `Initial Volume` | `emax`, `emin`, `eini` | hm³ |
| Reservoir water value | `Hydro_StoWaterValues.csv` value | `water_value` | $/hm³ |
| Spillway penalty | `Spill Penalty` (PLEXOS reports $/MWh) | `spillway_cost` | **$/(m³/s)/h** — converted via productibility (`fp_med` from `Hydro_EfficiencyIncr.csv`) |
| Demand | `Nod_Load.csv` | `lmax` | MW |
| Line capacity | `Lin_MaxRating.csv` × `Lin_Units.csv` | `tmax_ab` | MW |
| Reserve | `Res_Requirement.csv` | `urreq` | MW |
| Battery SOC | `Max SOC`, `Min SOC`, `Initial SOC` | `emax`, `emin`, `eini` | MWh |

**Spill Penalty caveat**: PLEXOS expresses spill penalty in
energy-equivalent ($/MWh) on the Storage object; gtopt expects flow-
equivalent ($/(m³/s)/h). Conversion uses the average productibility
`fp_med` (MW per m³/s) from `Hydro_EfficiencyIncr.csv`:

```
spillway_cost [$/(m³/s)/h]  =  Spill Penalty [$/MWh]  ×  fp_med [MW/(m³/s)]
```

When `fp_med` is missing, the writer falls back to the global
default 10 $/(m³/s)/h (matches Laja/Maule decision in
`feedback_irrigation_design`).

## 7. CLI

Mirror `sddp2gtopt` exactly:

```
plexos2gtopt INPUT [-o OUTPUT_DIR] [-l LOG_LEVEL]
             [--info] [--validate]
             [--use-single-bus]
             [--input-bundle BUNDLE_PATH]
```

`INPUT` accepts any of:

- A directory containing extracted `DBSEN_PRGDIARIO.xml` + `*.csv`.
- A `DATOS{date}.zip` or `DATOS{date}.zip.xz` archive.
- A `PLEXOS{date}.zip` outer wrapper (inner zips auto-unwrapped).
- A `RES{date}.zip[.xz]` solution archive (rejected for v0 — convert
  uses inputs only; comparing solutions is `gtopt_compare`'s job).

Output dir inference: `support/plexos_pcp_2026-04-22/PLEXOS20260422.zip` →
`gtopt_PLEXOS20260422/`.

## 8. Test plan

### Smoke (gate P0)

- `python -m plexos2gtopt --help` exits 0 with the description string.
- `python -m plexos2gtopt --version` reports `0.1.0`.
- `--validate` on a directory without the bundle fails with a usable
  error message.

### Unit (P1)

- `plexos_xml.load(path)` returns the expected `t_class` count for
  the 2026-04-22 reference bundle (96).
- `plexos_xml.objects_of_class(root, name="Generator")` returns the
  expected ~1700 entries.
- `plexos_csv.read_long("Gen_Rating.csv")` returns a `dict[name →
  list[24 floats]]`.
- `plexos_csv.read_wide("Nod_Load.csv")` returns a `dict[bus → list[24
  floats]]`.

### Integration (P2)

- Golden-fixture pattern (mirrors `tools/test_ucjl2gtopt.py`):
  - vendor a stripped 4-bus 2-generator synthetic `DBSEN_PRGDIARIO.xml`
    + matching CSVs under `tests/data/mini/`,
  - assert `convert_plexos_bundle(mini) == json.load(mini_golden.json)`,
  - regenerate golden via `--update-golden` (manual review gate).
- `gtopt --lp-only` accepts the regenerated planning end-to-end.

### Coverage gate

- `pytest -n auto plexos2gtopt/tests/` ≥ 83 % line coverage by the
  end of P2 (matches scripts/ root coverage gate).

## 9. Open questions / risks

- **Battery direction**: PLEXOS `Battery` exposes `Max Power` as a
  single rate; gtopt `Battery` splits into `pmax_charge` and
  `pmax_discharge`. Does CEN PCP always assume symmetric power, or
  do we need to read `Max Power Charge` / `Max Power Discharge` as
  separate properties? Need to verify on the 2026-04-22 bundle.
- **Hydro topology**: PLEXOS uses `Waterway` objects with
  `Storage From / Storage To` memberships to encode the cascade.
  gtopt requires explicit `Junction` nodes; we will need a
  synthetic-junction generator analogous to `plp2gtopt.junction_writer`.
- **Reserve direction**: PCP `Res_Requirement.csv` mixes up- and
  down-reserve in one file (column `BAND` carries the side). The
  parser must split BAND=1 (up) vs BAND=2 (down) before writing.
- **Fuel monthly→daily collapse**: `Fuel_Price.csv` has one row per
  month. The PCP run is one day, so the writer picks the matching
  month and emits a scalar. Days that straddle a month boundary
  (rare) are out of scope for v0.
- **`t_band` for piecewise gen cost**: PCP appears to ship single-band
  Heat Rate (verified for 2026-04-22). Multi-band support — needed
  for some thermals with breakpoints — is P3.
- **PLEXOS_Param.xml schema**: a thin wrapper; we only need
  `Day Beginning`, `Step Count`, `Step Type`. Full schema validation
  is deferred.
- **CFdata/CPF ramps**: 1000+ small CSVs (one per unit × direction)
  with sub-hourly ramp envelopes. v0 collapses to scalar
  `ramp_up`/`ramp_down` per unit; full envelope is P4.
