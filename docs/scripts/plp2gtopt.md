# plp2gtopt — PLP to gtopt Converter

Converts a **PLP** (*Programación de Largo Plazo*) case directory into the gtopt JSON + Parquet
format, enabling legacy PLP hydro-thermal cases to be solved with the gtopt
solver.

---

## Overview

PLP (*Programación de Largo Plazo*) is a widely used hydro-thermal scheduling
tool in Latin America.  Its input data is stored as a collection of
fixed-format `.dat` text files describing the power system topology, generator
parameters, demand curves, hydrology, and maintenance schedules.

`plp2gtopt` reads all standard PLP data files from a case directory and
produces:

- A **gtopt JSON file** (`<output-dir>.json`) containing the complete system
  definition, simulation parameters, and solver options.
- **Parquet (or CSV) time-series files** organised in subdirectories under
  `<output-dir>/` (e.g. `Demand/lmax.parquet`, `Generator/pmin.parquet`,
  `Afluent/afluent.parquet`).

The output is ready to be solved directly with the `gtopt` binary, uploaded to
**gtopt\_guisrv** (browser GUI), or submitted to **gtopt\_websrv** (REST API).

### Who needs this tool?

- Engineers migrating existing PLP study cases to gtopt.
- Teams that maintain PLP-format archives and want to benchmark or cross-validate
  results with gtopt's LP/MIP solver.
- Automation pipelines that generate PLP `.dat` files and need a gtopt-compatible
  output.

---

## Installation

Install as part of the gtopt scripts package from the repository root:

```bash
# Standard install (registers plp2gtopt on PATH)
pip install ./scripts

# Editable install with development dependencies
pip install -e "./scripts[dev]"
```

After installation the command is available on `$PATH`:

```bash
plp2gtopt --version
plp2gtopt --help
```

### Dependencies

| Package    | Purpose                        |
|------------|--------------------------------|
| `numpy`    | Numerical array processing     |
| `pandas`   | DataFrame I/O                  |
| `pyarrow`  | Parquet read/write             |

---

## Quick Start

```bash
# Default: reads ./input/, writes ./output.json + ./output/
plp2gtopt

# Explicit input and output directories
plp2gtopt -i plp_case_dir -o gtopt_case_dir

# Limit conversion to the first 5 stages
plp2gtopt -i input/ -s 5

# Two hydrology scenarios with 60/40 probability split
plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

# Apply a 10% annual discount rate
plp2gtopt -i input/ -d 0.10

# Create a ZIP archive ready for gtopt_guisrv / gtopt_websrv
plp2gtopt -z -i plp_case_2y -o gtopt_case_2y

# Verbose debug output
plp2gtopt -i input/ -l DEBUG
```

---

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `-i, --input-dir DIR` | `input` | PLP input directory containing `.dat` files |
| `-o, --output-dir DIR` | `output` | Output directory for Parquet/CSV data files |
| `-f, --output-file FILE` | `<output-dir>.json` | JSON output file path |
| `-n, --name NAME` | basename of output JSON | System name in the output JSON |
| `--sys-version STR` | `""` | Version string for the system in the output JSON |
| `-s, --last-stage N` | all | Stop conversion after stage N |
| `-d, --discount-rate RATE` | `0.0` | Annual discount rate (e.g. `0.10` for 10%) |
| `-m, --management-factor F` | `0.0` | Demand management factor |
| `-t, --last-time T` | all | Stop at time T |
| `-F, --output-format {parquet,csv}` | `parquet` | Output file format for time-series data |
| `--input-format {parquet,csv}` | same as `-F` | Input format hint written to gtopt options |
| `-c, --compression ALG` | `gzip` | Parquet compression codec (`gzip`, `snappy`, `brotli`, `none`) |
| `-y, --hydrologies H1[,H2,…]` | `0` | Comma-separated hydrology scenario indices |
| `-p, --probability-factors P1[,P2,…]` | equal | Probability weights per hydrology scenario |
| `-k, --use-kirchhoff` | off | Enable Kirchhoff voltage-law (DC OPF) constraints |
| `-b, --use-single-bus` | off | Use single-bus (copper-plate) mode |
| `--use-line-losses` | gtopt default | Model transmission line losses |
| `--demand-fail-cost COST` | `1000.0` | Cost penalty for demand curtailment ($/MWh) |
| `--reserve-fail-cost COST` | not set | Cost penalty for reserve shortfall ($/MWh) |
| `--scale-objective FACTOR` | `1000.0` | Objective function scaling factor |
| `-z, --zip` | off | Bundle JSON + data files into a ZIP archive |
| `-l, --log-level LEVEL` | `INFO` | Logging verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`) |
| `-V, --version` | — | Print version and exit |

---

## PLP Input File Format

`plp2gtopt` reads the following `.dat` files from the input directory.  All
required files must be present; optional files are processed when found.

### Required files

| File | Parser | Description |
|------|--------|-------------|
| `plpblo.dat` | `BlockParser` | Block definitions (time resolution, durations) |
| `plpeta.dat` | `StageParser` | Stage definitions (planning periods) |
| `plpbar.dat` | `BusParser` | Bus (bar) definitions with voltage levels |
| `plpcnfli.dat` | `LineParser` | Transmission line configurations (impedance, capacity) |
| `plpcnfce.dat` | `CentralParser` | Generating central (plant) configurations |
| `plpdem.dat` | `DemandParser` | Demand (load) definitions at each bus |
| `plpcosce.dat` | `CostParser` | Generation cost curves per central |
| `plpmance.dat` | `ManceParser` | Maintenance schedule for generating centrals |
| `plpmanli.dat` | `ManliParser` | Maintenance schedule for transmission lines |
| `plpaflce.dat` | `AflceParser` | Inflow (afluent) data for hydro plants |
| `plpextrac.dat` | `ExtracParser` | Water extraction data |
| `plpmanem.dat` | `ManemParser` | Maintenance schedule for energy management |

### Optional storage files (mutually exclusive)

The converter supports two battery/storage representations.  If `plpess.dat`
exists, the ESS path takes priority; otherwise the battery path is used.

| File | Parser | Description |
|------|--------|-------------|
| `plpess.dat` | `EssParser` | Energy Storage System definition (discharge/charge efficiency, energy bounds, DCMod, CenCarga) |
| `plpmaness.dat` | `ManessParser` | Maintenance schedule for ESS (energy + power bounds per block) |
| `plpcenbat.dat` | `BatteryParser` | Battery central definition (alternative to ESS) |
| `plpmanbat.dat` | `ManbatParser` | Maintenance schedule for batteries (energy bounds per block) |

### Generation-coupled battery (`source_generator`)

When a battery (BESS or ESS) is coupled to a co-located generation asset
(e.g. a solar plant that directly charges the battery), plp2gtopt sets the
`source_generator` field on the battery JSON element.

- **`plpess.dat`** — when `DCMod = 1` and `CenCarga` (cenpc) is non-empty,
  that central is set as `source_generator`.
- **`plpcenbat.dat`** — when `NIny > 0` (injection centrals present), the
  first injection central name is set as `source_generator`.

`System::expand_batteries()` then creates an internal bus for the charge path
so that:
- The **discharge Generator** connects to the external battery bus.
- The **charge Demand** connects to the internal bus.
- The **source generator** is rewired to the internal bus.

This models the configuration where a renewable plant (solar, wind) feeds
directly into the battery rather than exporting to the grid bus.

For standalone batteries (no injection central, `DCMod = 0`), both charge
and discharge connect to the same external bus.

### Regulation tanks (`DCMod = 2` → hydro Reservoir)

When `DCMod = 2` and `CenCarga` references a hydro central (serie, pasada,
or embalse), the ESS entry represents a **regulation tank** (Embalse de
Regulación Diaria — ERD), not a real battery.  These entries typically have
`nc = nd = 1.0` (perfect efficiency — water storage, no electrical losses).

`plp2gtopt` maps DCMod=2 entries to gtopt **Reservoir** elements instead of
Battery elements:

- The ESS entry is **excluded** from `battery_array`.
- A **Reservoir** is created with:
  - `junction` = the paired hydro generator's central number (its junction UID)
  - `emax` = ESS energy capacity (MWh)
  - `annual_loss` = `mloss × 12`
  - `daily_cycle = true`

The paired hydro generator already has a junction with river inflow and a
turbine in the gtopt hydro topology.  The regulation reservoir attaches to
that junction, and the water balance naturally models charging (storing
water) and discharging (releasing water through the turbine):

```
river_inflow + reservoir_extraction = waterway_out + drain
power = conversion_rate × waterway_flow
```

The turbine's capacity (`pmax`) enforces the joint capacity constraint
(`P_gen + P_tank ≤ Pmax`), which is the physical meaning of DCMod=2 in PLP.

**Validation**: `plp2gtopt` raises `ValueError` if the paired generator
(`CenCarga`) is not a hydro central type (e.g. if it is `termica`), since
regulation tanks require a hydro junction and turbine.

#### Example

In `plpess.dat`:
```
ANGOSTURA_ERD  1.000 1.000  2.0  800.0  328.1  2  ANGOSTURA
```

ANGOSTURA is a "serie" hydro generator (uid=68, 328.1 MW) with junction 68
and river inflow.  `plp2gtopt` creates a Reservoir named `ANGOSTURA_ERD`
(800 MWh, daily_cycle) attached to junction 68.  No Battery is created.

### Field order notes

- In `plpess.dat`, the Fortran READ order is
  `Nombre nd nc mloss Emax DCMax [DCMod] [CenCarga]` where `nd` is the
  discharge efficiency and `nc` is the charge efficiency.  Note that `nd`
  (discharge) comes **before** `nc` (charge), even though some PLP data file
  comments label them in the opposite order.
- `plpmanbat.dat` has 3 fields per data line: `IBind EMin EMax`.
- `plpmaness.dat` has 5–6 fields per data line: `IBind Emin Emax DCMin DCMax [DCMod]`.

---

## Mapping PLP elements → gtopt

Conversion is one parser per `.dat` file (see
[`scripts/plp2gtopt/`](../../scripts/plp2gtopt/)) feeding a writer
that assembles the gtopt JSON + Parquet bundle.

### High-level table

| PLP source                       | gtopt target                                                | Status |
|----------------------------------|-------------------------------------------------------------|:------:|
| `plpblo.dat` (Bloques)           | `simulation.block_array`                                    | ✅      |
| `plpeta.dat` (Etapas)            | `simulation.stage_array`                                    | ✅      |
| `plpidsim.dat` / `plpidape.dat`  | `simulation.scenario_array` + apertures                     | ✅      |
| `plpbar.dat` (Bar / Bus)         | `system.bus_array`                                          | ✅      |
| `plpcnfli.dat` + `plpmanli.dat`  | `system.line_array` + per-stage `tmax_ab/ba.parquet`        | ✅      |
| `plpcnfce.dat`  ⟶ thermal        | `system.generator_array` (thermal)                          | ✅      |
| `plpcnfce.dat`  ⟶ embalse        | `Reservoir` + `Turbine` + `Junction` + `Waterway`           | ✅      |
| `plpcnfce.dat`  ⟶ serie/pasada   | `Junction` + `Waterway` cascade (RoR & series hydro)        | ✅      |
| `plpcnfce.dat`  ⟶ falla          | demand-curtailment generator (auto-derived)                 | ✅      |
| `plpcosce.dat`                   | generator `gcost` time-series                               | ✅      |
| `plpdem.dat`                     | `system.demand_array` + `Demand/lmax.parquet`               | ✅      |
| `plpmance.dat`                   | per-stage `pmin.parquet` / `pmax.parquet`                   | ✅      |
| `plpcenre.dat`                   | `generator_profile_array` (renewable shapes)                | ✅      |
| `plpcenbat.dat` / `plpess.dat`   | `system.battery_array` (mutually exclusive)                 | ✅      |
| `plpmanbat.dat` / `plpmaness.dat`| `Battery/emin.parquet` + `Battery/emax.parquet`             | ✅      |
| `plpaflce.dat`                   | `Afluent/afluent.parquet`                                   | ✅      |
| `plpfilemb.dat`                  | `Reservoir.filtration` curves                               | ✅      |
| `plpmanem.dat`                   | per-stage `Reservoir.emin/emax`                             | ✅      |
| `plpvrebemb.dat`                 | `Reservoir.efin_cost` (soft volume slack)                   | ✅      |
| `plpextrac.dat`                  | `Junction.drain` (water extraction)                         | ✅      |
| `plpminembh.dat`                 | per-stage `Reservoir.emin` (minimum stored energy)          | ✅      |
| `plpralco.dat`                   | irrigation overlay (Ralco-specific)                         | ✅      |
| `plpgnl.dat`                     | LNG fuel availability constraint                            | ✅      |
| `plplajam.dat` / `plpmaulen.dat` | Laja / Maule irrigation agreements                          | ✅      |
| `plpmat.dat` / `plpplem*.dat`    | `boundary_cuts.csv` / SDDP cut hot-start                    | ✅      |
| `plpcenpmax.dat` / `plpcenfi.dat`| per-stage capacity / forced outage caps                     | ✅      |
| `plpidap2.dat`                   | second-axis aperture/scenario indexing                      | ✅      |

### Stages, blocks and scenarios

| gtopt concept    | PLP source                                                                                              |
|------------------|---------------------------------------------------------------------------------------------------------|
| `block_array`    | `plpblo.dat` — one entry per block, `duration` is the block hour budget                                 |
| `stage_array`    | `plpeta.dat` — one entry per planning period (monthly by default)                                       |
| `scenario_array` | `-y` flag selects hydrologies from `plpaflce.dat`; `plpidsim.dat` maps logical → physical column index  |

When `plpidsim.dat` is absent, `-y` indices are interpreted directly
as columns in `plpaflce.dat`.  Probability weights default to a
uniform split; override with `-p`.

### Bus and network (`plpbar.dat`, `plpcnfli.dat`)

| PLP field                         | gtopt field            | Notes                                          |
|-----------------------------------|------------------------|------------------------------------------------|
| `IBar` (1-based row)              | `Bus.uid`              | Stable through conversion                      |
| `Nombre` (`'…'`)                  | `Bus.name`             | Quotes stripped                                |
| Voltage                           | (logged, not in JSON)  |                                                |
| `IBarA`/`IBarB` in `plpcnfli.dat` | `Line.bus_a/bus_b`     | Resolved to bus names                          |
| `FFalla`                          | (informational)        | Forced-outage flag                             |
| `Tmax_AB / Tmax_BA`               | `Line.tmax_ab/ba`      | Time-varying via `plpmanli.dat`                |
| `Resist`, `Reactanc`              | `Line.resistance / reactance` | Used when `--use-kirchhoff` is on       |

### Centrales (`plpcnfce.dat`) — the heaviest mapping

PLP groups generating units into five categories.  Each maps to a
distinct gtopt structure:

| PLP central type    | gtopt structure                                                            |
|---------------------|----------------------------------------------------------------------------|
| **Térmica**         | `Generator` only                                                           |
| **Embalse** (reservoir) | `Reservoir` + `Turbine` + intake `Junction` + spillway `Waterway`     |
| **Serie hidráulica**| Cascaded `Junction` + `Waterway` chain feeding a downstream `Reservoir`    |
| **Pasada pura** (RoR)| `Junction` + `Waterway` + flow-driven `Generator` (no reservoir)          |
| **Falla** (failure) | Inferred — produces a high-cost virtual generator on every bus             |
| **Renovable** (`plpcenre.dat`) | `Generator` + `generator_profile_array` (hourly shape)          |

**Spillway waterway**: every embalse central also gets a synthetic
`_ver` waterway from upstream junction to drain; suppress it with
`--drop-spillway-waterway` for cases that prefer junction-level drain
to absorb spills.

**Pasada classification**: gtopt has tech-detection that may divert
some "pasada" centrals into the renewable-profile path (solar/wind
look-alikes).  Use `--pasada-mode {hydro,flow-turbine,profile}` to
override; `--plp-legacy` forces `flow-turbine` for byte-faithful PLP
parity.

### Demand (`plpdem.dat`, `plpcosce.dat`)

| PLP source                 | gtopt target                          | Conversion                          |
|----------------------------|---------------------------------------|-------------------------------------|
| `plpdem.dat` MW values     | `Demand.lmax[stage][block]`           | direct (no GWh→MW step needed)      |
| `plpcosce.dat` `Falla`     | `options.demand_fail_cost`            | average of first-tier FALLA gcost   |
| `plpcosce.dat` per-segment | piecewise gcost on virtual generators | one segment per cost tier           |

When `--demand-fail-cost` is **not** explicitly set, `plp2gtopt`
auto-derives it from the first-tier FALLA gcost in `plpcnfce.dat`;
writing `demand_fail_cost = 0` in `~/.gtopt.conf` silently disables
that auto-detection (see `_parsers.py`).

### Hydrology (`plpaflce.dat`, `plpfilemb.dat`, `plpvrebemb.dat`)

| PLP source                 | gtopt target                                     |
|----------------------------|--------------------------------------------------|
| `plpaflce.dat`             | `Afluent/afluent.parquet`, m³/s per stage/scen   |
| Hydro `FP` (MW per m³/s)   | `Turbine.conversion_rate`                        |
| Reservoir `Vmin/Vmax`      | `Reservoir.vmin/vmax` (hm³)                      |
| Reservoir `Vinic`          | `Reservoir.vinic` (initial volume)               |
| `plpfilemb.dat` segments   | `Reservoir.filtration` (volume → loss curve)     |
| `plpvrebemb.dat`           | spillage cost feeding `Reservoir.efin_cost`      |
| `plpminembh.dat`           | per-stage `Reservoir.emin` minimum               |

`reservoir_energy_scale` is auto-derived from `emax` so SDDP
state-variable scaling stays well-conditioned; override with
`--reservoir-energy-scale`.

### Storage (`plpess.dat` / `plpcenbat.dat`)

PLP supports two mutually exclusive battery dialects.  See
[Optional storage files](#optional-storage-files-mutually-exclusive)
above for selection rules and the special `DCMod = 2` regulation-tank
case (mapped to `Reservoir`, not `Battery`).

### Soft constraints and slack costs

| PLP feature                          | gtopt expression                              |
|--------------------------------------|-----------------------------------------------|
| First-tier `falla` cost              | `options.demand_fail_cost`                    |
| `vrebemb` (spillage cost)            | `Reservoir.efin_cost` per stage               |
| `plpmanem` `emin/emax` per stage     | `Reservoir.soft_emin_cost` slack              |
| `plpmaness` capacity bands           | per-stage `Battery` `emin/emax/dcmin/dcmax`   |

`--soft-storage-bounds` (default **on**) routes per-reservoir
end-volume slack through the C++ `Reservoir.efin_cost` mechanism
instead of hard constraints, matching PLP's per-stage rebalse cost.

---

## Output Structure

### JSON + Parquet directory layout

A typical conversion produces:

```
gtopt_case_2y.json              ← main system / simulation / options config
gtopt_case_2y/                  ← input_directory (referenced in the JSON)
├── Demand/
│   └── lmax.parquet            ← demand time-series
├── Generator/
│   ├── pmin.parquet            ← generator minimum output
│   └── pmax.parquet            ← generator maximum output
├── Afluent/
│   └── afluent.parquet         ← hydro inflow data
├── Battery/
│   ├── emin.parquet            ← battery energy lower bounds
│   └── emax.parquet            ← battery energy upper bounds
└── Line/
    ├── tmax_ab.parquet         ← line capacity (direction A→B)
    └── tmax_ba.parquet         ← line capacity (direction B→A)
```

The `input_directory` field in the JSON options object matches the subdirectory
name, so gtopt can locate the data files without any extra configuration.

---

## ZIP Output (`-z` / `--zip`)

The `-z` flag creates a single **ZIP archive** that bundles the JSON and all
data files together, preserving the full directory structure:

```bash
plp2gtopt -z -i plp_case_2y -o gtopt_case_2y
# Produces: gtopt_case_2y.zip
```

ZIP layout:

```
gtopt_case_2y.zip
├── gtopt_case_2y.json
└── gtopt_case_2y/
    ├── Demand/
    │   └── lmax.parquet
    ├── Generator/
    │   ├── pmin.parquet
    │   └── pmax.parquet
    └── Afluent/
        └── afluent.parquet
```

This archive is directly compatible with **gtopt\_guisrv** (upload via the GUI)
and **gtopt\_websrv** (submit via the REST API).

---

## Conversion Statistics

After a successful conversion `plp2gtopt` logs statistics (at INFO level)
similar to the pre-solve statistics printed by the `gtopt` solver:

```
=== System statistics ===
  System name     : plp2gtopt
=== System elements  ===
  Buses           : 2
  Generators      : 3
  Generator profs : 1
  Demands         : 2
  Lines           : 1
  Batteries       : 0
  Converters      : 0
  Junctions       : 2
  Waterways       : 1
  Reservoirs      : 1
  Turbines        : 1
=== Simulation statistics ===
  Blocks          : 8760
  Stages          : 5
  Scenarios       : 2
=== Key options ===
  use_single_bus  : False
  scale_objective : 1000
  demand_fail_cost: 1000
  input_directory : gtopt_case_2y
  annual_discount : 0.1
=== Conversion time ===
  Elapsed         : 1.234s
```

Use `-l DEBUG` to see which individual `.dat` files are being parsed and their
parsing results.

---

## Error Messages

| Situation | Error message |
|-----------|---------------|
| Input directory missing | `Input directory does not exist: 'plp_case/'` |
| Required `.dat` file missing | `Required file not found: …/plpblo.dat` |
| Invalid data format in a `.dat` file | `Invalid data format: …` |
| Mismatched hydrology/probability counts | Number of `-p` values must match `-y` |

---

## Integration with gtopt\_guisrv and gtopt\_websrv

### gtopt\_guisrv (browser GUI)

1. Convert and ZIP:
   ```bash
   plp2gtopt -z -i plp_case -o gtopt_case
   ```
2. Start the GUI: `gtopt_guisrv`  (opens at `http://localhost:5001`)
3. Click **Upload case** and select `gtopt_case.zip`.
4. Edit parameters if needed, then click **Solve**.

### gtopt\_websrv (REST API)

Submit the ZIP directly:

```bash
curl -X POST http://localhost:3000/api/solve \
  -F "file=@gtopt_case.zip"
```

The server runs the gtopt solver and returns a results ZIP containing the
solution files.

---

## Example: Converting a 2-Year Hydro Case with 2 Scenarios

Suppose you have a PLP case directory `plp_case_2y/` containing 24 monthly
stages (2 years) with hydro plants, thermal generators, and two hydrology
scenarios (dry and wet).

```bash
# Convert with 2 hydrology scenarios, 10% discount rate, Kirchhoff mode
plp2gtopt \
  -i plp_case_2y \
  -o gtopt_case_2y \
  -n gtopt_case_2y \
  -s 24 \
  -y 1,2 \
  -p 0.6,0.4 \
  -d 0.10 \
  -k \
  -z

# This produces:
#   gtopt_case_2y.json   — main planning configuration
#   gtopt_case_2y/       — Parquet time-series data
#   gtopt_case_2y.zip    — bundled archive

# Solve with gtopt
gtopt gtopt_case_2y.json

# Or upload the ZIP to the GUI service
gtopt_guisrv  # then upload gtopt_case_2y.zip in the browser
```

The conversion statistics will show the full system inventory — buses, thermal
and hydro generators, demands, lines, junctions, waterways, reservoirs, and
turbines — along with the simulation structure (blocks × stages × scenarios).

---

## Test fixture library

`scripts/cases/` ships a graded set of PLP cases used by the parser
unit tests, the integration suite, and the smoke tests:

| Fixture                       | Purpose                                                              |
|-------------------------------|----------------------------------------------------------------------|
| `plp_dat_ex/`                 | Minimal valid `.dat` collection (one of every required file)         |
| `plp_min_1bus/`               | Single-bus toy case (smoke test for the JSON pipeline)               |
| `plp_min_2bus/`               | Two-bus + one line                                                   |
| `plp_min_hydro/`              | One reservoir, one turbine, AR-1 inflow                              |
| `plp_min_hydro_ms/`           | Multi-stage hydro with maintenance                                   |
| `plp_min_reservoir/`          | Reservoir-only flow path                                             |
| `plp_min_battery/`            | Standalone battery (`plpcenbat.dat`)                                 |
| `plp_min_bess/`               | BESS via `plpess.dat` (DCMod = 0)                                    |
| `plp_min_ess/`                | Source-coupled ESS (DCMod = 1)                                       |
| `plp_min_cenre/`              | Renewable profile generator                                          |
| `plp_min_filemb/`             | Reservoir filtration curves                                          |
| `plp_min_mance/` `_manli/`    | Maintenance schedules for centrals / lines                           |
| `plp_hydro_4b/`               | 4-bus hydro benchmark                                                |
| `plp_bat_4b_24/`              | 4-bus, 24-block battery scheduling                                   |
| `plp_case_2y/`                | Realistic 2-year case (used by the slow integration tests)           |

Each fixture is a self-contained directory of `.dat` files; running
`plp2gtopt -i scripts/cases/<fixture> -o /tmp/gtopt_<fixture>` then
`gtopt --lp-only -s /tmp/gtopt_<fixture>/<fixture>.json` is the
canonical end-to-end smoke test.

---

## Limitations and known gaps

| Area                          | Status                                                                |
|-------------------------------|-----------------------------------------------------------------------|
| Multi-segment thermal bid     | Cost segments collapsed to a per-stage scalar `gcost`                 |
| Forced-outage stochasticity   | `FFalla` flag honoured for capacity, not for stochastic failure draws |
| AR-P inflow draws             | gtopt's own SDDP engine generates samples; PLP `plpaflemb.dat` is read but not directly passed through |
| Multi-currency studies        | Single `currency` per planning; PSR-style multi-currency cases collapse to the reference currency |
| Calendar-aware stage hours    | `plpblo.dat` durations honoured; per-month nominal mapping when blocks are missing |

---

## See Also

- [SCRIPTS.md](../scripts-guide.md) — overview of all gtopt Python scripts
- [PLANNING\_GUIDE.md](../planning-guide.md) — gtopt planning concepts
  (stages, blocks, scenarios, expansion)
- [INPUT\_DATA.md](../input-data.md) — gtopt input file format reference
- [sddp2gtopt](sddp2gtopt.md) — sister tool for PSR SDDP cases
- [pp2gtopt](pp2gtopt.md) — convert pandapower networks to gtopt
- [ts2gtopt](ts2gtopt.md) — project hourly time-series onto gtopt horizons
- [PLP source code](https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src)
  — authoritative Fortran readers (`lee*.f`) for every `.dat` file
