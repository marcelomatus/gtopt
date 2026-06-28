# sddp2gtopt — PSR SDDP to gtopt Converter

Converts a **PSR SDDP** (*Stochastic Dual Dynamic Programming*, by PSR
Inc.) case directory into a gtopt JSON planning, mirroring the role
[`plp2gtopt`](plp2gtopt.md) plays for PLP cases.

---

## Overview

PSR SDDP is the commercial hydrothermal-dispatch software developed by
PSR Inc. (Rio de Janeiro), used across Latin America (Brazil, Chile,
Peru, Uruguay, Colombia, …) for medium- and long-term operation
studies.  An SDDP study is stored as:

- A flat collection of fixed-format `.dat` files (`sistem.dat`,
  `chidros1.dat`, `ctermis1.dat`, `hinflw.dat`, `htopol.dat`,
  `deme01s1.dat`, …).
- A **typed JSON snapshot** `psrclasses.json` written by the SDDP GUI
  alongside the `.dat` files when a study is saved.

`sddp2gtopt` has **two interchangeable front-ends** (auto-detected from
the input directory), both feeding the same entity IR + writer:

1. **`psrclasses.json`** — the typed JSON snapshot the SDDP GUI saves.
2. **Raw PSR `.dat`** — the flat file collection real PSR SDDP / NCP
   deployments ship (e.g. the Guatemalan AMM weekly/daily cases), which
   have **no** `psrclasses.json`.  Parsed by the modular per-file readers
   in [`dat_parsers.py`](../../scripts/sddp2gtopt/dat_parsers.py)
   (each subclassing the shared
   `gtopt_shared.base_parser.BaseTextParser`) and orchestrated by
   [`dat_loader.py`](../../scripts/sddp2gtopt/dat_loader.py).

Both produce a single-bus gtopt planning that `gtopt --lp-only` ingests
directly.  See [the `.dat` front-end](#the-dat-front-end-psr-sddp--ncp)
below.

> ⚠️  **Two flavours of "SDDP"**: this tool targets the **PSR Inc.**
> commercial format only, *not* the academic Julia package
> [SDDP.jl](https://sddp.dev) (Oscar Dowson) which uses
> StochOptFormat (`.sof.json`) and a different schema.

### Who needs this tool?

- Engineers migrating PSR SDDP archives to gtopt.
- Teams running PLP and SDDP in parallel (e.g. CEN-Chile) who want a
  single solver to cross-validate both dialects.
- Automation pipelines that already produce SDDP cases.

---

## Installation

Installs as part of the gtopt scripts package:

```bash
pip install ./scripts        # registers sddp2gtopt on PATH
sddp2gtopt --version
sddp2gtopt --help
```

No extra dependencies beyond what `plp2gtopt` already requires
(`numpy`, `pandas`, `pyarrow`).

---

## Quick start

```bash
# Show what's in a case (no conversion)
sddp2gtopt --info  /path/to/sddp_case

# Schema sanity check (exit 0 if OK)
sddp2gtopt --validate /path/to/sddp_case

# Convert (default: writes ./gtopt_<case>/<case>.json)
sddp2gtopt /path/to/sddp_case

# Explicit output dir
sddp2gtopt -i /path/to/sddp_case -o /path/to/out

# End-to-end smoke test
sddp2gtopt sddp_demo -o gtopt_demo
gtopt --lp-only -s gtopt_demo/gtopt_demo.json
```

---

## CLI options

| Flag                          | Default | Description                                       |
|-------------------------------|---------|---------------------------------------------------|
| `INPUT_DIR` (positional)      | —       | SDDP case dir (alternative to `-i`)               |
| `-i, --input-dir DIR`         | —       | Same as positional, takes precedence              |
| `-o, --output-dir DIR`        | inferred| Output dir (`sddp_X` → `gtopt_X`; else `gtopt_<X>`)|
| `--hydro-cost $/MWh`          | `0`     | Uniform hydro water-value stand-in (.dat path)     |
| `--import-limit MW`           | —       | Cap aggregate interconnection import (GUA↔MEX ≈ 200)|
| `--info`                      | off     | Print case summary and exit                       |
| `--validate`                  | off     | Run schema sanity checks and exit                 |
| `-l, --log-level LEVEL`       | `INFO`  | `DEBUG` / `INFO` / `WARNING` / `ERROR` / `CRITICAL`|
| `-V, --version`               | —       | Print version and exit                            |

---

## PSR SDDP input file format

A vanilla SDDP case directory (e.g. the vendored
[`tests/data/case0/`](../../scripts/sddp2gtopt/tests/data/case0/)) contains
~15 files.  v0 reads only `psrclasses.json`; the rest are listed for
context — they will be parsed by v1+ when present without a JSON
snapshot.

| File              | Format    | Used in v0 | Holds                                                              |
|-------------------|-----------|:----------:|--------------------------------------------------------------------|
| `psrclasses.json` | JSON      |  ✅        | Typed snapshot of every PSR collection (the v0 source of truth)    |
| `sddp.dat`        | text      |    —      | Study control: stages, blocks, deficit cost, discount rate         |
| `sistem.dat`      | text      |    —      | System list (one per area)                                         |
| `chidros1.dat`    | text      |    —      | Hydro plant params (Vmin/Vmax, Qmin/Qmax, FP, head/tail curves)    |
| `ctermis1.dat`    | text      |    —      | Thermal plant params (G/CEsp segments, Comb/fuel index)            |
| `ccombus1.dat`    | text      |    —      | Fuel definitions per system (Custo, EmiCO2, PC)                    |
| `htopol.dat`      | text      |    —      | Hydro topology (turbine → spill → downstream)                      |
| `hparam.dat`      | binary    |    —      | AR-P inflow model coefficients                                     |
| `hinflw.dat`      | text      |    —      | Historical inflow series (year × month × station)                  |
| `deme01s1.dat`    | text      |    —      | Demand profile (block × stage)                                     |
| `mhdadds1.dat`    | text      |    —      | Hydro additional/maintenance data                                  |
| `coral.dat`       | text      |    —      | Solver settings                                                    |
| `estima.dat`      | text      |    —      | Inflow-estimation parameters (AR-P order, year range)              |
| `unimon.dat`      | text      |    —      | Currency / unit conversions                                        |
| `index.dat`       | text      |    —      | Catalog of 325 SDDP output variables (used as parity reference)    |

### What's inside `psrclasses.json`

A flat top-level dictionary `{collection_name: [entity, ...]}`.  Every
entity carries:

- `reference_id` (int) — the canonical cross-reference key.
- `code` (int) — 1-based PSR uid (often shown in SDDP reports).
- `name` (str) — human label.
- `classType` (int) — internal type tag (mostly ignorable).

References between entities (e.g. `PSRThermalPlant.fuels`,
`PSRDemand.system`) are arrays/scalars of `reference_id`.
:class:`sddp2gtopt.psrclasses_loader.PsrClassesLoader` indexes them so
resolution is O(1).

### Collections seen in `case0`

```
PSRStudy                       1
PSRSystem                      1
PSRDemand                      1
PSRDemandSegment               1
PSRFuel                        2
PSRThermalPlant                3
PSRHydroPlant                  1
PSRGaugingStation              2
PSRNetwork                     1   (container — ignored)
PSRNetworkDC                   1   (container — ignored)
PSRElectrificationNetwork      1   (container — ignored)
PSRHydrologicalNetwork         1   (container — ignored)
PSRHydrologicalPlantNetwork    1   (container — ignored)
PSRInterconnectionNetwork      1   (container — ignored)
PSRGasNetwork                  1   (container — ignored)
```

Container collections (the `*Network` family) wrap the actual entities
and currently carry no data we need; v0 ignores them.

---

## Mapping PSR SDDP elements → gtopt

The conversion is a per-collection transformation.  Each PSR entity
type is parsed by a function in
[`sddp2gtopt/parsers.py`](../../scripts/sddp2gtopt/parsers.py) into a
typed dataclass from
[`sddp2gtopt/entities.py`](../../scripts/sddp2gtopt/entities.py); the
writer in
[`sddp2gtopt/gtopt_writer.py`](../../scripts/sddp2gtopt/gtopt_writer.py)
then assembles the gtopt JSON.

### High-level table

| PSR collection / field           | gtopt target                       | v0 status         |
|----------------------------------|------------------------------------|-------------------|
| `PSRStudy`                       | top-level `options` + `simulation` | ✅ full            |
| `PSRSystem`                      | `system.bus_array` (synthesised)   | ✅ single-system   |
| `PSRDemand` + `PSRDemandSegment` | `demand_array`                     | ✅ inline `lmax`   |
| `PSRFuel`                        | absorbed into thermal `gcost`      | ✅                 |
| `PSRThermalPlant`                | `generator_array` (thermal)        | ✅ collapsed bid   |
| `PSRHydroPlant`                  | `generator_array` (hydro)          | ⚠️ flattened       |
| `PSRGaugingStation`              | inflow time-series                 | ⏳ deferred (v2)   |
| `htopol.dat` topology            | reservoir/turbine cascade          | ⏳ deferred (v2)   |
| `ccombus1.dat` / multi-bus       | extra `bus_array` + `line_array`   | ⏳ deferred (v3)   |
| `PSRInterconnection*`            | `line_array`                       | ⏳ deferred (v3)   |
| Multi-system cases               | multiple `system` blocks           | ⏳ deferred (v4)   |

### `PSRStudy` → `options` + `simulation`

| PSR field             | gtopt target / formula                        |
|-----------------------|-----------------------------------------------|
| `Ano_inicial`         | reference year (logged, not in JSON)          |
| `Etapa_inicial`       | 1-based stage index (logged)                  |
| `Tipo_Etapa`          | sets per-stage hour budget (see below)        |
| `NumeroEtapas`        | length of `simulation.stage_array`            |
| `NumeroBlocosDemanda` | blocks per stage                              |
| `DeficitCost[0]`      | `options.demand_fail_cost`                    |
| `TaxaDesconto`        | `options.annual_discount_rate`                |
| `CurrencyReference`   | preserved on `SystemSpec.currency`            |

**Stage hour budget** (used to compute block durations and convert
demand from GWh → MW):

| `Tipo_Etapa` | Cadence    | Hours / stage |
|:-:           |------------|--------------:|
| 1            | weekly     | 168           |
| 2            | monthly    | 730           |
| 3            | trimester  | 2190          |
| other        | (fallback) | 730           |

These are nominal averages — calendar-aware durations are deferred to
v2 once `mhdadds*.dat` parsing lands.  Block duration is
`stage_hours / num_blocks`.

### `PSRSystem` → bus

v0 collapses each PSR system to a single synthesised bus
named `sys_<code>_bus`.  The bus carries every generator and demand
in that system.  Multi-bus topology (from `ccombus*.dat`) is the v3
deliverable.

| PSR field         | gtopt target                                                  |
|-------------------|---------------------------------------------------------------|
| `code`            | derives the synthetic bus name (`sys_{code}_bus`)             |
| `name`            | logged, not in JSON                                           |
| `UnM`             | currency tag (logged, not in JSON)                            |

> Multi-system cases are **rejected** with a clear `ValueError`
> rather than silently collapsing all systems to one bus.

### `PSRFuel` → thermal cost component

Fuels are not standalone gtopt entities; they only serve to compute
each thermal plant's `gcost`.

| PSR field      | Used as                                          |
|----------------|--------------------------------------------------|
| `Custo[0]`     | $/fuel-unit price (the `cost` factor)            |
| `UE`           | Fuel unit (informational, e.g. `MWh`, `m³`)      |
| `EmiCO2`       | logged; emission cost not yet wired in v0        |
| `reference_id` | resolved from `PSRThermalPlant.fuels[]`          |

### `PSRThermalPlant` → `generator_array`

Each thermal plant becomes one entry in `system.generator_array`.

| PSR field       | gtopt field      | Notes                                                                |
|-----------------|------------------|----------------------------------------------------------------------|
| `name`          | `name`           | Falls back to `thermal_<code>` when blank                            |
| `code`          | `uid` (offset)   | gtopt uids are sequential within the array, not direct copies        |
| `GerMin[0]`     | `pmin`           |                                                                      |
| `GerMax[0]`     | `pmax`           |                                                                      |
| `PotInst[0]`    | `capacity`       | Same as `pmax` for v0                                                |
| `G(i)`          | segment cap      | Up to 3 piecewise segments (i = 1..3); zero-cap segments dropped     |
| `CEsp(i,1)`     | × fuel cost      | Specific consumption (fuel-unit / MWh)                               |
| `fuels[0]`      | fuel reference   | `PSRFuel.reference_id` to look up the price                          |
| `CTransp[0]`    | (logged)         | Transport cost; not yet added to `gcost`                             |
| `system`        | bus picker       | Picks `sys_<system.code>_bus`                                        |

**Cost computation (single-segment v0)**:

```
gcost = min(over segments) [ CEsp(i,1) × fuels[0].Custo[0] ]
```

Worked example from `case0`:

| Plant     | `G(1)` | `CEsp(1,1)` | Fuel ref → Cost   | gcost ($/MWh) |
|-----------|-------:|------------:|-------------------|--------------:|
| Thermal 1 | 100    | 10.0        | Fuel 1 → 0.8      | **8.0**       |
| Thermal 2 | 100    | 15.0        | Fuel 1 → 0.8      | **12.0**      |
| Thermal 3 | 100    | 12.5        | Fuel 2 → 1.2      | **15.0**      |

> v0 collapses the up-to-3-segment bid curve to its **cheapest**
> segment, which preserves the merit order but loses the convex bid
> shape.  Multi-segment piecewise gcost is a v2 enhancement (writer
> already emits the `g_segments` list — only the writer needs an
> upgrade).

### `PSRHydroPlant` → `generator_array` (flattened)

Hydro plants are modelled as **zero-cost generators** in v0 — full
reservoir + turbine + inflow chains arrive in v2 once we parse
`htopol.dat` and the gauging-station series.

| PSR field    | v0 mapping                | v2+ target                          |
|--------------|---------------------------|--------------------------------------|
| `PotInst[0]` | generator `pmax` & `cap`  | `Turbine.pmax`                       |
| `Vmin[0]`    | (ignored)                 | `Reservoir.vmin`                     |
| `Vmax[0]`    | (ignored)                 | `Reservoir.vmax`                     |
| `Vinic`      | (ignored)                 | `Reservoir.vinic`                    |
| `Qmin[0]`    | (ignored)                 | `Turbine.qmin` / waterway lower band |
| `Qmax[0]`    | (ignored)                 | `Turbine.qmax`                       |
| `FPMed[0]`   | (ignored)                 | `Turbine.conversion_rate`            |
| `station`    | (ignored)                 | inflow series source                 |
| `htopol.dat` | (not parsed)              | `Junction` + `Waterway` cascade      |

The v0 limitation means hydro is "infinite-water" — every block can
dispatch up to `PotInst` MW for free, regardless of physical
availability.  This is acceptable for an LP-build smoke test on
`case0` (1 MWh/block hydro, no reservoir tightness) but unsafe for
real studies.

### `PSRGaugingStation` → inflow series (deferred)

v0 parses the station name + raw `Vazao` series for diagnostics
(`--info` shows the series length), but the inflows are **not wired
into the planning** until the hydro topology lands in v2.

### `PSRDemand` + `PSRDemandSegment` → `demand_array`

Each `PSRDemand` is one entry in `system.demand_array`; the time-series
profile comes from the matching `PSRDemandSegment` (linked via
`demand` reference_id).  v0 emits the profile inline as
`lmax[stage][block]`, no Parquet file.

| PSR source                          | gtopt field   | Conversion                    |
|-------------------------------------|---------------|-------------------------------|
| `PSRDemand.name`                    | `name`        |                               |
| `PSRDemand.system`                  | `bus`         | bus picked from `PSRSystem`   |
| `PSRDemandSegment.Demanda(1)[s]`    | `lmax[s][b]`  | see formula below             |

**Unit conversion (GWh / stage  →  MW / block)**:

```
mw_block_b = (gwh_stage_s × 1000) / block_hours
```

where `block_hours = stage_hours(Tipo_Etapa) / num_blocks`.  When the
PSR series is **shorter** than `num_stages`, the tail is zero-padded;
**longer** series are truncated.  The same value is replicated across
every block within a stage (per-block load shape from `Duracao(i)`
weights is a v2 enhancement).

Worked example from `case0` (Tipo_Etapa = 2 → 730 h, 1 block):

| Stage | `Demanda(1)` (GWh) | `lmax` (MW)        |
|------:|-------------------:|-------------------:|
| 1     | 8.928              | 8.928 × 1000 / 730 ≈ **12.23** |
| 2     | 8.064              | 8.064 × 1000 / 730 ≈ **11.05** |

---

## Stages, blocks, scenarios

| gtopt concept    | PSR source                              |
|------------------|-----------------------------------------|
| `stage_array`    | one entry per `NumeroEtapas`            |
| `block_array`    | `NumeroBlocosDemanda` blocks per stage  |
| `scenario_array` | always **1** in v0 (probability = 1)    |

Multi-scenario stochasticity (forward/backward Monte-Carlo from
`Series_Forward` / `Series_Backward` and the AR-P inflow model in
`hparam.dat`) is the v4 deliverable.  Until then the converted
planning is **deterministic** — single-scenario, expected-value
demand, and (in v2+) deterministic inflows.

---

## Output schema

The converter writes a single JSON file shaped like the small
reference cases under [`cases/c0`](../../cases/c0/system_c0.json) and
[`cases/s1b`](../../cases/s1b/s1b.json):

```json
{
  "options":   { "annual_discount_rate": …, "use_single_bus": true,  … },
  "simulation":{ "block_array": [...], "stage_array": [...], "scenario_array": [...] },
  "system":    { "name": "…", "bus_array": [...], "generator_array": [...], "demand_array": [...] }
}
```

No Parquet sidecar files in v0 (every series is short enough to live
inline).  Parquet output and a `<output_dir>/` subtree (matching the
plp2gtopt layout) are part of the v2 hydro deliverable.

---

## Test fixture library

`scripts/sddp2gtopt/tests/data/` ships six standalone cases, each
small enough to read by hand:

| Fixture                | Source       | What it covers                                                     |
|------------------------|--------------|--------------------------------------------------------------------|
| `case0/`               | upstream PSR | Real PSR sample (1 system, 3 thermal, 1 hydro, 2 stations)         |
| `case_min/`            | hand-crafted | Smallest valid case (1 thermal, 1 demand, 1 stage, 1 block)        |
| `case_thermal_only/`   | hand-crafted | Thermals only, multi-fuel, 3 stages × 2 blocks                     |
| `case_two_systems/`    | hand-crafted | Two `PSRSystem` — must trigger a clear conversion error            |
| `case_bad_no_study/`   | hand-crafted | Missing `PSRStudy` — must fail `--validate`                        |
| `case_bad_truncated/`  | hand-crafted | Truncated JSON — must surface a parse error                        |

`case0/` is vendored from
[`psrenergy/PSRClassesInterface.jl`](https://github.com/psrenergy/PSRClassesInterface.jl)
under the Mozilla Public License 2.0 (see
`tests/data/LICENSE-PSRClassesInterface`).  The hand-crafted cases use
the same JSON schema PSR SDDP writes natively, so adding a new fixture
only requires authoring a `psrclasses.json` — no PSR install needed.

### Adding a fixture

1. Create `scripts/sddp2gtopt/tests/data/case_<name>/psrclasses.json`
   with at least `PSRStudy` and `PSRSystem`.
2. Add a session-scoped fixture to
   [`tests/conftest.py`](../../scripts/sddp2gtopt/tests/conftest.py)
   that returns the directory.
3. Reference it from any `test_*.py`; the fixture is injected via
   pytest argument names.

---

## The `.dat` front-end (PSR SDDP / NCP)

When the input directory has **no `psrclasses.json`** but does contain a
`sddp.dat`, `sddp2gtopt` switches to the raw-`.dat` front-end.  This is
the format real PSR SDDP / NCP deployments ship — including the
Guatemalan AMM weekly (`NCP`) and daily (`PDD_DIARIO`) cases.

### Files read

| file            | parser class    | → entity / use                                   |
|-----------------|-----------------|--------------------------------------------------|
| `sddp.dat`      | `ControlParser` | `StudySpec` (cadence, stages, deficit, discount) |
| `sistem.dat`    | `SystemParser`  | `SystemSpec` list                                |
| `ccombu*.dat`   | `FuelParser`    | fuel `Custo` ($/MWh) → thermal cost              |
| `ctermi*.dat`   | `ThermalParser` | `ThermalSpec` (`gcost = CEsp × Custo + CVaria`)  |
| `chidro*.dat`   | `HydroParser`   | `HydroSpec` (installed `Pot`)                     |
| `cpde*.dat`     | `DemandParser`  | hourly demand → per-block `lmax`                 |

Each parser subclasses `gtopt_shared.base_parser.BaseTextParser` — the
same shared base `plp2gtopt`'s parsers use.

### Cost model (the key win over the PSS/E network export)

PSR `.dat` carries the economics a PSS/E `.raw` lacks.  Each thermal
unit's gtopt `gcost` is computed per segment as
`CEsp(i) × fuel.Custo + CVaria`, where `Comb` selects the fuel and
`G(i)` are the segment capacity percentages — so the converted DC
dispatch follows the real PSR merit order.

### Multi-bus network (`dbus.dat` + `dcirc.dat`)

When both `dbus.dat` and `dcirc.dat` are present, the converter emits a
**multi-bus DC OPF** (`use_kirchhoff = true`):

- **buses** from `dbus.dat` (one gtopt `Bus` per node);
- **generator → bus** routing from the `dbus.dat` plant columns (matched
  by name — 100 % of the Guatemalan thermal+hydro plants map);
- **circuits** from the fixed-width `dcirc.dat` (lines + transformers),
  with ohm reactances converted to per-unit on a 100 MVA base using the
  bus base voltages from **`Volt.dat`** (name-suffix + neighbour
  inference fill the generator-terminal buses absent from `Volt.dat`);
  near-radial extremes are floored/capped to keep the LP well-conditioned;
- **per-bus demand** from `cpdexbus.dat` (one `Demand` per load node).

Falls back to a single bus when `dbus`/`dcirc` are absent.

### Hydro water-value pricing

A single-stage snapshot cannot *derive* water values (those come from the
multi-stage SDDP cost-to-go) — so, exactly as PSR's NCP does, the
converter **reads the water values and applies them as hydro costs**.
When `watervcp.csv` is present it is auto-loaded: each plant's value
(k$/hm³) is converted to $/MWh via its production factor —
`gcost = WV · 3.6 / FPMed`, capped at the deficit cost — and set as that
hydro's `gcost`.  Storage reservoirs price high (they hold water back);
run-of-river plants (water value ≈ 0) stay free.  `--hydro-cost <$/MWh>`
is a uniform fallback for plants without a water value.

### The LMP cross-check (vs PDD `cmgbuscp.csv`)

Cross-checking gtopt's bus duals against PSR's solved `cmgbuscp.csv`
shows the multi-bus **topology + economics are reproduced**: the network
builds and congests, storage-hydro water values are applied (83/102 GUA
plants priced), run-of-river is inflow-limited (`inflow.csv × FPMed`),
and the **interconnection import cap** (`--import-limit 200`, the EOR
GUA↔MEX tie limit) stops the $0 Mexico import fuels (~1.8 GW) from
flooding the dispatch.

The import cap is the decisive lever — it lifts gtopt's LMPs from ~$3 to
the right order of magnitude:

| metric (PDD 22-Jun) | PSR `cmgbuscp` | gtopt (import-capped) | AMM 2025 spot |
|---------------------|---------------:|----------------------:|--------------:|
| time-mean LMP $/MWh | 141            | **58**                | ~85 (Jun ~80) |
| peak-block LMP $/MWh| 165            | **88**                | —             |

The residual ~2× gap is the **single-stage limitation**: PSR never prices
below its **inter-temporal water-value floor** (~$93 even off-peak,
because water saved now serves the peak later), whereas a single-stage
snapshot lets free run-of-river hydro price to ~$0 off-peak.  Closing
that requires the **multi-stage reservoir coupling** (volume state +
boundary/FCF cuts) — the same mechanism PSR's SDDP uses to set the water
value — which is the documented next step, not a single-stage fix.

### Time model (v0)

The NCP demand file is **hourly over the dispatch horizon**, so the
converted planning is **single-stage with one block per demand hour**
(e.g. a 7-day week → 1 stage × 168 hourly blocks).  Multi-stage SDDP
horizons and reservoir hydro (`htopol`/`hinflw`) are follow-ons below.

### Example

```bash
# Guatemalan AMM weekly NCP case (extracted .dat directory)
sddp2gtopt --info  ./ncp_week
sddp2gtopt -i ./ncp_week -o gtopt_ncp_week      # auto multi-bus if dbus+dcirc
gtopt -s gtopt_ncp_week/gtopt_ncp_week.json -d out      # DC OPF, solves optimally

# price hydro at the water value to lift LMPs toward the PSR band
sddp2gtopt -i ./ncp_week -o gtopt_ncp_week --hydro-cost 92
```

---

## Roadmap and deferred mappings

The following PSR features are **not yet** mapped.  See
[`scripts/sddp2gtopt/DESIGN.md`](../../scripts/sddp2gtopt/DESIGN.md) for the
full phased plan.

| Phase | Deliverable                                                                       |
|------:|-----------------------------------------------------------------------------------|
| v1    | `.dat` fallback for cases without `psrclasses.json`                              |
| v2    | Hydro reservoir + turbine + waterway from `PSRHydroPlant` + `htopol.dat`         |
| v2    | Inflow time-series from `PSRGaugingStation` + AR-P stationary draw               |
| v2    | Multi-segment thermal bid curves (already collected, only the writer changes)    |
| v2    | Per-block demand shape from `Duracao(i)` weights                                 |
| ~~v3~~ ✅ | Multi-bus DC OPF from `dbus.dat` + `dcirc.dat` + `Volt.dat` + `cpdexbus.dat` (done) |
| v3    | Transmission lines from `PSRCircuito` / `PSRTransformador`                       |
| v4    | Multi-system cases with `PSRInterconnection*`                                    |
| v4    | Multi-scenario stochasticity (forward/backward AR-P samples)                     |
| v5    | Round-trip parity vs `index.dat`'s 325-variable output catalog                   |

---

## Cross-references

- [DESIGN.md](../../scripts/sddp2gtopt/DESIGN.md) — phased roadmap.
- [plp2gtopt.md](plp2gtopt.md) — sister tool for the PLP dialect.
- [PSRClassesInterface.jl](https://github.com/psrenergy/PSRClassesInterface.jl)
  — upstream Julia library that sources `case0`.
- [PSR SDDP user manual (v17.2 PDF)](https://www.psr-inc.com/wp-content/uploads/softwares/SddpUsrEng.pdf)
  — authoritative format reference.
