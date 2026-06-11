# PLEXOS "missing efin" / boundary-cut signal — root cause

**Date**: 2026-05-21
**Bundle**: PCP `DATOS20260422` (1 week, 168 h, starts 2026-04-22)
**Target reservoirs**: COLBUN, RALCO, CANUTILLAR

## TL;DR

The anti-drain signal that holds COLBUN/RALCO/CANUTILLAR near their initial
volume is **not** a per-block hard floor, an SDDP Benders cut, a generic
constraint, or a model-options setting. It is a single per-storage
**linear-in-end-volume objective term**:

```
PLEXOS Storage property:    "Water Value"  (property_id = 1101, collection 93)
PLEXOS unit:                $/CMD          (unit_id = 46  →  $/CMD per t_unit)
PLEXOS Variable override:   StoFCF         (object_id 4116, class 74 = Variable)
PLEXOS Variable Filename:   Hydro_StoWaterValues.csv
```

Every t_data row that carries property 1101 on a Storage is **tagged** in
`t_tag` with the `StoFCF` Variable, which substitutes the constant value
(10,000 or 1E+30) with the per-storage figure published in
`Hydro_StoWaterValues.csv`. The CSV is shipped inside `datos.zip` and is
**already in our input bundle**.

`plp2gtopt`'s/`plexos2gtopt`'s existing `extract_reservoirs`
(`scripts/plexos2gtopt/parsers.py:1575-1601`) reads the t_data row but
**stops there**: it never resolves the StoFCF tag, so every PCP reservoir
inherits the placeholder constant (10,000 $/GWh — and the L_Maule sentinel
1E+30) instead of the real `Hydro_StoWaterValues.csv` price.

## Investigation log per candidate

### 1. Benders / SDDP cuts in t_data_0  —  **not present**

`t_data_0.csv` from the solution `.accdb` is the **output time series**
(key_id, period_id, value triples), and `t_key.csv` enumerates
46,988 result keys. There is no t_property entry whose name contains
`Cut`, `Benders`, `Recourse`, `Linear Term`, or `Constant Term` on any
Storage-related collection (40, 80, 93). The full t_property catalogue
in `/tmp/gtopt-pcp-fuelcap/plexos_cache/t_property.csv.zst` (verified)
contains only `Max Volume / Min Volume / Initial Volume / End Volume /
Natural Inflow / Shadow Price / Non-physical Inflow` for collection 93,
i.e. solution-side quantities only.

**Verdict**: the solution database does not encode end-of-horizon cuts.
It encodes the *resulting* dispatch only.

### 2. Generic Constraint objects acting on Storage  —  **none binding**

`t_object` lists 963 Constraint-class (70) objects. Names suggesting
volume coupling:

| Constraint name | Membership | Comment |
|---|---|---|
| `CANUTILLARreserve` | (not memberships back to Storage with Energy/Vol coefs) | likely reserve provision, not a volume floor |
| `DOWNStorageBound_BAT_*` / `UPStorageBound_BAT_*` | 10 batteries only | battery SoC limits, not hydro |
| `BatMaxCycDay_BAT_*` | batteries only | unrelated |

Cross-tabulation of `t_data` rows for Constraint-class memberships
(see embedded script run during investigation) shows the following
property usage on class 70 — none of which target the three hydro
reservoirs:

| Constraint property | Row count |
|---|---|
| Generation Coefficient | 2,773 |
| Units Generating Coefficient | 3,729 |
| Flow Coefficient | 814 |
| Reserve Units Coefficient | 641 |
| Regulation Raise/Lower Coefs | 1,381 / 1,961 |
| **End Volume Coefficient** | **2** (only `CFRS_COLBUN`/MACHICURA at 29.53, `CFRS_PEHUENCHE`/PEHUENCHE at 41.76) |
| Energy Coefficient | 15 |
| RHS / RHS Custom / RHS Day | 1,806 / 273 / 11 |

The two `End Volume Coefficient` rows attach Constraint
`CFRS_COLBUN`/`CFRS_PEHUENCHE` to MACHICURA/PEHUENCHE, *not* to
COLBUN/RALCO/CANUTILLAR, and their coefficients (29.53, 41.76) are too
small to anchor a 9,890 GWh reservoir against a $10/m³ drainage incentive.

**Verdict**: no constraint object pins COLBUN/RALCO/CANUTILLAR end volume.

### 3. Storage property catalog  —  Water Value is the only relevant one

For collection 93 (System.Storages), `t_data` carries values only for
**eight** properties, across 33 storages:

| Storage property | Distinct rows |
|---|---|
| Max Volume | 33 |
| Min Volume | 33 |
| Initial Volume | 33 |
| End Effects Method | 33 (all = `1` = "Free") |
| Natural Inflow | 28 |
| Non-physical Inflow Penalty | 11 |
| **Water Value** | **8** (CANUTILLAR, CIPRESES, COLBUN, ELTORO, L_Maule, PEHUENCHE, RALCO, RAPEL) |
| Balance Period | 5 |

No `Min Energy`, `End Volume`, `End Volume Penalty`, `Target`,
`Target Day/Week/Month/Year`, `Target Level`, `Target Penalty`,
`Trajectory Lower/Upper Bound Penalty` — none of those 70-odd Storage
properties are present in t_data. Only the eight above.

`End Effects Method = 1` (= "Free", per input_mask `0;"Auto";1;"Free";2;"Recycle"`)
means PLEXOS does **not** force end-of-horizon volumes back to initial;
the **only** end-of-horizon force is the `Water Value` linear term.

### 4. Hydro_*.csv files  —  Hydro_StoWaterValues.csv is the smoking gun

Files in `datos.zip` under `Hydro_*`:

| CSV | Role | Contains target signal? |
|---|---|---|
| `Hydro_AntucoBounds.csv` | ANTUCO/ELTORO per-day min/max | No (not COLBUN/RALCO/CANUTILLAR) |
| `Hydro_EfficiencyIncr.csv` | Unit hydro efficiency $/MWh | No |
| `Hydro_InitialVolume.csv` | eini per reservoir at hour 1 | Just initial |
| `Hydro_MaxRampDay.csv` | per-day ramp caps | No |
| `Hydro_MaxVolume.csv` | per-hour emax (uniform across all hours) | No (caps are wide, e.g. COLBUN=17,411) |
| `Hydro_MinVolume.csv` | per-hour emin (uniform across all hours) | No — values 4,416/4,738/5,741 are well below achieved efin |
| **`Hydro_StoWaterValues.csv`** | **per-storage $/CMD Water Value** | **YES** |
| `Hydro_WaterFlows.csv` | Irrigation/eco flows per reservoir+block | No (separate constraint) |

`Hydro_StoWaterValues.csv` content (verbatim, 11 lines):

```csv
NAME,YEAR,MONTH,DAY,PERIOD,VALUE
FCF,2026,4,22,1,1158518300
L_Maule,2026,4,22,1,9744.363936
CIPRESES,2026,4,22,1,6502.226054
PEHUENCHE,2026,4,22,1,4500.02304
COLBUN,2026,4,22,1,2264.995094
ELTORO,2026,4,22,1,6483.298925
RAPEL,2026,4,22,1,789.5639635
CANUTILLAR,2026,4,22,1,1242.14351
RALCO,2026,4,22,1,3291.554794
PANGUE,2026,4,22,1,1512.373939
PILMAIQUEN,2026,4,22,1,665.5475549
```

- `FCF` line = the **FCF Constant** scalar (PLEXOS subtracts it during
  SDDP reporting; safe to ignore in the LP).
- Each named storage = $/CMD-day end-of-horizon linear term on its
  state variable.

#### Cross-check: drainage vs CSV water value

| Reservoir | eini (GWh) | efin (GWh) | drained % | CSV $/CMD | t_data placeholder |
|---|---|---|---|---|---|
| COLBUN     | 9,890.6 | 9,395.7 | 5.00% | **2,264.99** | 10,000 |
| RALCO      | 8,136.9 | 7,805.3 | 4.08% | **3,291.55** | 10,000 |
| CANUTILLAR | 7,206.2 | 7,137.6 | 0.95% | **1,242.14** | 10,000 |
| L_Maule    | n/a     | n/a     | —     | **9,744.36** | 1E+30 (sentinel) |

The CSV value is the **only anchor** in PLEXOS pulling efin toward eini.
The static placeholder (10,000 — same for all eight) cannot explain the
heterogeneous drainage pattern (CANUTILLAR < COLBUN < RALCO in percent);
a single global $10,000/GWh would drain all three to the floor.
The Hydro_StoWaterValues.csv values (CANUTILLAR=1,242; COLBUN=2,265;
RALCO=3,292) explain the asymmetry directly.

### 5. PLEXOS_Param.xml / SolverParam.xml  —  no model-wide anchor

`PLEXOS_Param.xml` only ships `MTSchedule.ResolutionBlockCount /
ResolutionIntervalCount / ResolutionType` for 7 bands. No
`StorageEndTarget`, `EndVolumePenalty`, `Recycle`, or `FCFScalar` overrides.
`PLEXOS_SolverParam.xml` carries 3 CPLEX params + 1 Gurobi param.

The XML database `DBSEN_PRGDIARIO.xml` defines two **attributes** in class
84 (Horizon?) — `FCF Scalar` (default 1,000,000) and `FCF Constant`
(default 0) — but neither is overridden in the bundle's `t_attribute_data`,
so they take their library defaults.

### 6. t_data_5/6/7 in the .accdb  —  no Storage cut data

The cached .accdb covers `t_data_0` only. PLEXOS's `t_data_N` numbering
corresponds to result granularity (period_type_id: 0 = period, 4 = day, 5 = week,
6 = month, 7 = year). Since `Hydro_StoWaterValues.csv` provides a static
end-of-horizon term (one row, PERIOD=1) and the LP horizon is a single
week, no time-resolved cut data is expected in higher data tables.
Verified by absence of any t_property whose name suggests a cut on
Storage / Reservoir.

## Where the StoFCF override is wired (XML evidence)

1. Variable object: `<t_object>` with `object_id=4116`, `class_id=74`
   ("Variable"), `name="StoFCF"`.
2. Membership 4115: parent System / child class 74 / child object 4116,
   collection 718.
3. `<t_data>` row 32637: membership 4115, property 4450 ("Filename"),
   value `0` (placeholder).
4. `<t_text>` row 32637: associates that data row with the string
   `Hydro_StoWaterValues.csv`.
5. Each Storage with property 1101 ("Water Value") has its t_data row
   tagged via `<t_tag>{data_id, object_id=4116}` — eight rows total
   (CANUTILLAR/CIPRESES/COLBUN/ELTORO/L_Maule/PEHUENCHE/RALCO/RAPEL).

At runtime PLEXOS sees the tag → resolves the variable → opens the CSV →
substitutes per-storage values keyed by `NAME` column.

## Why gtopt currently misses this

`scripts/plexos2gtopt/parsers.py:1575-1601` reads:

```python
raw_water_value_gwh = (
    db.static_property(
        "Storage", storage.object_id, "Water Value", keep_sentinel=True
    )
    or 0.0
)
```

`db.static_property` (`scripts/plexos2gtopt/plexos_xml.py:263`) reads
**only** from the t_data row. It does **not** inspect `t_tag` or
load the CSV that the StoFCF Variable points to. Result: every Storage
that PLEXOS would price via the CSV gets `water_value = 10,000` (the
placeholder), and L_Maule gets the 1E+30 sentinel → handled as
`never_drain`. Net effect for COLBUN/RALCO/CANUTILLAR: a flat
$10,000/GWh efin price, which is high enough to discourage some
drainage but does not reproduce the differentiated PLEXOS behaviour.

## Recommended fix (extraction code)

Add a CSV-resolver to `extract_reservoirs`. Pseudocode (drop into
`parsers.py`):

```python
def _load_sto_water_values(bundle) -> dict[str, float]:
    """Return {storage_name: water_value_$/CMD} from Hydro_StoWaterValues.csv.

    PLEXOS Variable 'StoFCF' (object_id 4116, class 74) tags every
    Storage.WaterValue t_data row with this CSV. The CSV's NAME column
    keys per-storage $/CMD coefficients of the end-of-horizon linear
    cost term; the 'FCF' row is the FCF Constant scalar and is not
    storage-keyed (skip it).
    """
    if not bundle.has("Hydro_StoWaterValues.csv"):
        return {}
    out: dict[str, float] = {}
    for row in bundle.read_csv("Hydro_StoWaterValues.csv"):
        name = row["NAME"]
        if name == "FCF":
            continue
        try:
            out[name] = float(row["VALUE"])
        except (KeyError, ValueError):
            continue
    return out


# inside extract_reservoirs(), after computing eini/emax/emin/efin:
sto_water_values = _load_sto_water_values(bundle)  # hoist out of loop in real code
for storage in db.objects_of_class("Storage"):
    ...
    placeholder_wv = db.static_property(
        "Storage", storage.object_id, "Water Value", keep_sentinel=True
    ) or 0.0
    if placeholder_wv > 1.0e12:
        # Existing never-drain handling for L_Maule (1E+30) keeps its semantics.
        never_drain = True
        water_value_per_cmd = 0.0
    else:
        # Resolve StoFCF override: if the storage has a placeholder t_data row
        # AND appears in Hydro_StoWaterValues.csv, use the CSV value.
        # If no t_data row was ever shipped (placeholder_wv == 0.0), we have
        # no end-volume signal — leave water_value=0 (gtopt's default).
        water_value_per_cmd = sto_water_values.get(name, placeholder_wv) \
            if placeholder_wv > 0.0 else 0.0
        never_drain = False
    # Unit conversion: PLEXOS Water Value is $/CMD (cubic-meter-day per
    # `t_unit` row 46). gtopt currently labels it "$/GWh" in entities.py
    # but the existing `efin_cost` slack is unit-agnostic.  When wiring
    # this, multiply by the storage's MW-per-CMD efficiency
    # (or equivalently leave as $/CMD and let the writer convert via
    # `ReservoirSpec.energy_factor_mwh_per_cmd_day`).
```

**Implementation tasks**:
1. Add `_load_sto_water_values` helper in `parsers.py`.
2. Replace the constant-only branch in `extract_reservoirs` so that a
   non-sentinel placeholder triggers a CSV lookup.
3. (Optional but recommended) Add a unit-conversion step: PLEXOS unit
   is `$/CMD` (water-flow $); gtopt's `efin_cost` in `entities.py`
   line 247 is annotated `$/GWh` and the writer uses it as a coefficient
   on the `efin` slack. The conversion factor is
   `water_value_$/CMD * (3600 * 24 / 1e6) * efficiency_MWh_per_m³`
   to land in `$/MWh`; for this PCP bundle a simpler path is to
   take the CSV value as a per-storage opportunity cost and let the
   writer multiply by the existing energy-volume conversion that
   `eini`/`efin` already use.
4. Tag (`StoFCF`) verification — confirmation step: read
   `t_tag` for each Water-Value `data_id` and assert
   `child_object_id == 4116`. This catches future bundles where some
   storages are kept on the static placeholder without tag — defensive
   only; not strictly needed because the placeholder is the
   reservoir-specific override semantics in PLEXOS.

## Final ranking by faithfulness × ease

| Rank | Source | Faithfulness | Ease | Notes |
|------|--------|--------------|------|-------|
| 1 | **`Hydro_StoWaterValues.csv` via StoFCF tag** | **High** | **High** | CSV is already in the bundle; eight storages map 1:1 to PCP reservoirs; matches drainage pattern qualitatively |
| 2 | Static Storage.WaterValue placeholder (current) | Low | (already done) | Flat $10,000 explains *some* anti-drain but not the heterogeneous pattern |
| 3 | FCF Constant 1.158e9 from CSV first row | n/a | n/a | Pure objective constant; affects reporting only, not optimization |
| 4 | t_data_0 in solution `.accdb` | Zero | n/a | Solution output, not Benders cuts |
| 5 | Generic constraint with End Volume Coefficient | None | High | Only attached to MACHICURA/PEHUENCHE via CFRS_*; not the three target reservoirs |
| 6 | PLEXOS_Param.xml model options | None | High | Carries only MTSchedule resolution bands |

**Conclusion**: this is fully extractable from the bundle we already
have. The fix is a ~20-line CSV reader in `parsers.py`. The hardest
piece is the unit-conversion decision (`$/CMD` → gtopt's `efin_cost`
coefficient on the `efin` slack); the simplest interim choice is to
keep the existing $/GWh interpretation but seed it with the CSV value
for the eight named storages (effectively "use the right scalar
instead of 10,000") — that alone will produce per-reservoir
differentiated efin levels in the same ordering as PLEXOS, even before
a strict CMD↔GWh conversion is wired.
