# plp2gtopt — PLP to gtopt Converter

Converts a **PLP (PLPMAX/PLPOPT)** case directory into the gtopt JSON + Parquet
format, enabling legacy PLP hydro-thermal cases to be solved with the gtopt
solver.

---

## Overview

PLP (also known as PLPMAX or PLPOPT) is a widely used hydro-thermal scheduling
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
| `plpess.dat` | `EssParser` | Energy Storage System definition (discharge/charge efficiency, energy bounds) |
| `plpmaness.dat` | `ManessParser` | Maintenance schedule for ESS (energy + power bounds per block) |
| `plpcenbat.dat` | `BatteryParser` | Battery central definition (alternative to ESS) |
| `plpmanbat.dat` | `ManbatParser` | Maintenance schedule for batteries (energy bounds per block) |

### Field order notes

- In `plpess.dat`, the Fortran READ order is
  `Nombre nd nc mloss Emax DCMax [DCMod] [CenCarga]` where `nd` is the
  discharge efficiency and `nc` is the charge efficiency.  Note that `nd`
  (discharge) comes **before** `nc` (charge), even though some PLP data file
  comments label them in the opposite order.
- `plpmanbat.dat` has 3 fields per data line: `IBind EMin EMax`.
- `plpmaness.dat` has 5–6 fields per data line: `IBind Emin Emax DCMin DCMax [DCMod]`.

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

## See Also

- [SCRIPTS.md](../../SCRIPTS.md) — Overview of all gtopt Python scripts
- [PLANNING\_GUIDE.md](../../PLANNING_GUIDE.md) — Guide to gtopt planning concepts
  (stages, blocks, scenarios, expansion)
- [INPUT\_DATA.md](../../INPUT_DATA.md) — gtopt input file format reference
- [pp2gtopt](pp2gtopt.md) — Convert pandapower networks to gtopt
- [ts2gtopt](ts2gtopt.md) — Project hourly time-series onto gtopt horizons
