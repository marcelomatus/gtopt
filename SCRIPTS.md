# gtopt Python Scripts

The `scripts/` directory contains four Python command-line utilities for
preparing and converting data for use with gtopt.

## Table of Contents

- [Installation](#installation)
- [plp2gtopt](#plp2gtopt)
- [pp2gtopt](#pp2gtopt)
- [igtopt](#igtopt)
- [cvs2parquet](#cvs2parquet)
- [Using with gtopt\_guisrv and gtopt\_websrv](#using-with-gtopt_guisrv-and-gtopt_websrv)

---

## Installation

Install all four tools with a single `pip` command from the repository root:

```bash
pip install ./scripts
```

This registers the `plp2gtopt`, `pp2gtopt`, `igtopt`, and `cvs2parquet` commands on your
`PATH`.  An editable install is useful during development:

```bash
pip install -e "./scripts[dev]"
```

### Dependencies

| Package | Purpose |
|---------|---------|
| `numpy` | Numerical array processing |
| `pandas` | DataFrame I/O |
| `pyarrow` | Parquet read/write |
| `openpyxl` | Excel file support (`igtopt`) |
| `pandapower` | Power system network data (`pp2gtopt`) |

---

## plp2gtopt

Converts a **PLP (PLPMAX/PLPOPT)** case directory to the gtopt JSON + Parquet
format.  Reads the standard PLP data files (`plpblo.dat`, `plpbar.dat`,
`plpcosce.dat`, `plpcnfce.dat`, `plpcnfli.dat`, `plpdem.dat`, `plpeta.dat`,
and others) and writes:

- A **gtopt JSON file** (`<output-dir>.json`) with the complete system,
  simulation, and options configuration.
- **Parquet time-series files** organised in subdirectories under
  `<output-dir>/` (e.g. `Demand/lmax.parquet`, `Generator/pmin.parquet`,
  `Afluent/afluent.parquet`).

### Basic usage

```bash
# Default: reads ./input, writes ./output.json + ./output/
plp2gtopt

# Explicit input/output directories
plp2gtopt -i plp_case_dir -o gtopt_case_dir

# Limit conversion to the first 5 stages
plp2gtopt -i input/ -s 5

# Two hydrology scenarios with 60/40 probability split
plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

# Apply a 10% annual discount rate
plp2gtopt -i input/ -d 0.10

# Verbose debug output
plp2gtopt -i input/ -l DEBUG
```

### ZIP output (`-z` / `--zip`)

The `-z` flag creates a single **ZIP archive** that bundles the JSON
configuration file and all Parquet/CSV data files together, preserving the
full output directory structure.  This archive is directly compatible with
**gtopt\_guisrv** (upload via the GUI) and **gtopt\_websrv** (submit via the
REST API):

```bash
plp2gtopt -z -i plp_case_2y -o gtopt_case_2y
# Produces: gtopt_case_2y.zip
```

The ZIP layout is:

```
gtopt_case_2y.zip
├── gtopt_case_2y.json          ← main system/simulation/options config
└── gtopt_case_2y/              ← input_directory (data files)
    ├── Demand/
    │   └── lmax.parquet
    ├── Generator/
    │   ├── pmin.parquet
    │   └── pmax.parquet
    └── Afluent/
        └── afluent.parquet
```

> **Note**: the `input_directory` field in the JSON options matches the name
> of the subdirectory inside the ZIP, so gtopt\_guisrv and gtopt\_websrv can
> locate the data files without any extra configuration.

### Conversion statistics

After a successful conversion, `plp2gtopt` logs statistics (at INFO level)
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

Use `-l DEBUG` to also see which individual `.dat` files are being parsed.

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `-i, --input-dir DIR` | `input` | PLP input directory |
| `-o, --output-dir DIR` | `output` | Output directory for Parquet/CSV files |
| `-f, --output-file FILE` | `<output-dir>.json` | JSON output file path |
| `-z, --zip` | off | Create a ZIP archive of the JSON + data files |
| `-s, --last-stage N` | all | Stop after stage N |
| `-d, --discount-rate RATE` | `0.0` | Annual discount rate (e.g. `0.10` for 10%) |
| `-m, --management-factor F` | `0.0` | Demand management factor |
| `-t, --last-time T` | all | Stop at time T |
| `-c, --compression ALG` | `gzip` | Parquet compression (`gzip`, `snappy`, `brotli`, `none`) |
| `-y, --hydrologies H1[,H2,…]` | `0` | Hydrology scenario indices |
| `-p, --probability-factors P1[,P2,…]` | equal | Probability weights per scenario |
| `-l, --log-level LEVEL` | `INFO` | Verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`) |
| `-V, --version` | — | Print version and exit |

### Error messages

`plp2gtopt` raises descriptive errors for common problems:

| Situation | Error message |
|-----------|---------------|
| Input directory missing | `Input directory does not exist: 'plp_case/'` |
| Required `.dat` file missing | `Required file not found: …/plpblo.dat` |
| Invalid data format | `Invalid data format: …` |

---

## pp2gtopt

Converts a **pandapower** network to gtopt JSON format.  Accepts either a
built-in IEEE test network (via `-n`) or any pandapower network file saved to
disk (via `-f`).  Writes a self-contained gtopt JSON file ready to be solved
directly with the `gtopt` binary or submitted via `gtopt_guisrv` / `gtopt_websrv`.

### Basic usage

```bash
# Convert the default IEEE 30-bus built-in network → ieee30b.json
pp2gtopt

# Convert a saved pandapower JSON file
pp2gtopt -f my_network.json -o my_case.json

# Convert a MATPOWER case file
pp2gtopt -f case39.m -o case39.json

# Convert a pandapower Excel workbook
pp2gtopt -f network.xlsx -o network.json

# Use a specific built-in test network
pp2gtopt -n case14 -o ieee14b.json

# List all available built-in test networks
pp2gtopt --list-networks
```

### Input from file (`-f / --file`)

`-f FILE` loads any pandapower network saved to disk.  The format is
auto-detected from the file extension:

| Extension | Format | Produced by |
|-----------|--------|-------------|
| `.json` | pandapower JSON | `pandapower.to_json()` |
| `.xlsx` / `.xls` | pandapower Excel | `pandapower.to_excel()` |
| `.m` | MATPOWER case file | MATPOWER / Octave |

The output JSON system `name` is derived from the file stem (e.g. `case39.m`
→ `"case39"`).

### Available built-in networks (`-n / --network`)

| Network name | pandapower function | Description |
|---|---|---|
| `ieee30b` *(default)* | `case_ieee30` | IEEE 30-bus (Washington) |
| `case4gs` | `case4gs` | 4-bus Glover-Sarma |
| `case5` | `case5` | 5-bus example |
| `case6ww` | `case6ww` | 6-bus Wood-Wollenberg |
| `case9` | `case9` | IEEE 9-bus |
| `case14` | `case14` | IEEE 14-bus |
| `case33bw` | `case33bw` | 33-bus Baran-Wu |
| `case57` | `case57` | IEEE 57-bus |
| `case118` | `case118` | IEEE 118-bus |

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `-f, --file FILE` | — | pandapower network file (`.json`, `.xlsx`/`.xls`, `.m`) |
| `-n, --network NAME` | `ieee30b` | built-in test network (mutually exclusive with `-f`) |
| `-o, --output FILE` | `<stem>.json` | Output JSON file path |
| `--list-networks` | — | Print all available built-in network names and exit |
| `-V, --version` | — | Print version and exit |

---

## igtopt

Converts an **Excel workbook** to a gtopt JSON case.

```bash
# Basic usage
igtopt case.xlsx

# Write output to a specific JSON file
igtopt case.xlsx -j output/case.json

# Pretty-print JSON
igtopt case.xlsx --pretty
```

Sheets whose names start with `@` (e.g. `Demand@lmax`) are written as
Parquet time-series files to the `input_directory` specified in the workbook's
`options` sheet.

---

## cvs2parquet

Converts **CSV time-series files** to Parquet format.

```bash
# Convert a single file
cvs2parquet input.csv output.parquet

# Use an explicit PyArrow schema for type enforcement
cvs2parquet --schema input.csv output.parquet
```

Columns named `stage`, `block`, or `scenario` are cast to `int32`; all other
columns are cast to `float64`.

---

## Using with gtopt\_guisrv and gtopt\_websrv

Both services accept a **ZIP archive** containing the JSON configuration file
and its associated Parquet/CSV data files.  Use `plp2gtopt -z` to produce a
ZIP that is ready to upload without any extra packaging step.

### gtopt\_guisrv (browser GUI)

1. Start the GUI service:

   ```bash
   gtopt_guisrv
   # Open http://localhost:5001
   ```

2. Click **Upload case** and select the `.zip` file produced by
   `plp2gtopt -z`.

3. Edit the case if needed, then click **Solve** to submit it to the
   webservice.

For installation and service setup, see
[guiservice/INSTALL.md](guiservice/INSTALL.md).

### gtopt\_websrv (REST API)

Submit a ZIP directly to the REST API:

```bash
curl -X POST http://localhost:3000/api/solve \
  -F "file=@gtopt_case_2y.zip"
```

The server runs the `gtopt` solver and returns a results ZIP containing the
solution files.

For full API reference and deployment instructions, see
[webservice/INSTALL.md](webservice/INSTALL.md).
